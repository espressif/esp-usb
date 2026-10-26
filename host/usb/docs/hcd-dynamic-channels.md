# Dynamic host-channel scheduling (phase 1)

## Problem

The DWC OTG host has a small fixed pool of hardware channels (16 on
ESP32-S31/P4, from GHWCFG). The HCD historically bound one channel to every
pipe for the pipe's whole lifetime (`hcd_pipe_alloc` → `hcd_pipe_free`), so
every *open* pipe cost a channel even while completely idle:

- every enumerated device's EP0 pipe: 1 channel each,
- every external hub: 2 channels (EP0 + interrupt),
- every claimed endpoint: 1 channel each.

On the esp-usbip-bridge bench this capped a populated tree at ~8 devices +
3 hubs = 14/16 channels while *nothing* was transferring, and the 15th/16th
claim failed (`hcd_pipe_alloc` → "No more HCD channels available" →
`ESP_ERR_NOT_SUPPORTED`). Linux's dwc2 driver instead treats channels as a
pool assigned per active transfer.

## Phase 1 (this change): bind on demand for CTRL and BULK pipes

A CTRL or BULK pipe no longer owns a channel. Instead:

- `hcd_pipe_alloc` allocates the pipe and its `usb_dwc_hal_chan_t` object but
  does **not** take a hardware channel. Pipe allocation for these types can
  no longer fail with channel exhaustion.
- The channel is **bound** (allocated + `set_ep_char` + PID programmed) when
  the pipe's first URB is enqueued, and **unbound** when the pipe goes idle
  (its last done URB is dequeued and nothing is pending or executing).
- If no channel is free at enqueue, the pipe joins the port's FIFO
  **waiter queue** and its URBs stay PENDING. Whenever a channel frees (a
  dynamic pipe going idle, or any pipe being freed), the longest-waiting
  pipe with pending URBs is bound and its buffers are filled/executed.
  Exhaustion therefore becomes *queuing* instead of a hard error for
  CTRL/BULK.
- The **bulk data toggle** is preserved across rebinds: HCTSIZ.PID is read
  into the pipe at unbind (`usb_dwc_hal_chan_get_pid`) and programmed at
  bind. Control transfers always restart at SETUP, so CTRL pipes bind with
  PID 0; `HCD_PIPE_CMD_RESET_TOGGLE` clears the saved toggle too.

INTR and ISOC pipes keep the static alloc-time binding: their schedule lives
in the periodic frame list (programmed via `set_ep_char` against a concrete
channel), and in practice they always have a URB posted (hub/HID polling),
so dynamic binding would buy nothing and cost frame-list reprogramming.
They can still fail allocation with `ESP_ERR_NOT_SUPPORTED` on exhaustion.

### What this buys on the bench

An enumerated-but-idle device now costs **zero** channels (its EP0 is
unbound). A fully populated 8-device/3-hub tree idles at ~3 channels (hub
interrupt pipes) instead of 14, and a 10-port hub of composite devices can
sit enumerated with sessions scheduled per-transfer against the real pool.
Steady-state holders remain: periodic pipes, and bulk IN pipes with reads
parked (a console session that keeps a read posted holds its channel — that
is correct, it is actively listening).

### Invariants kept

- Cancellation/halt discipline is unchanged: halt/flush/clear and the
  hold-until-retired rules operate on bound channels; on an unbound pipe a
  halt is a pure state change (there is no hardware side), and clear on an
  unbound pipe re-enters the waiter queue instead of executing.
- `_port_reset_all_pipes` reprograms only bound channels; unbound pipes get
  their `ep_char` at next bind. `update_mps`/`update_dev_addr` update the
  stored `ep_char` always and the registers only when bound.
- All bind/unbind/kick operations run inside the existing HCD critical
  section, in the same contexts that already called fill/exec.

### Observability

`hcd_port_get_channel_stats()` is unchanged (allocated = truly bound).
A new `hcd_port_get_channel_waiters()` reports pipes queued for a channel,
plumbed through `hub_get_channel_stats_waiters()`/`usb_host_channel_stats()`
consumers (the esp-usbip-bridge surfaces it as `channels_waiting` in
`GET /api/usb/claims`).

## Phase 2 (this change): rotation among parked listeners

When a pipe has to wait for a channel, `_port_request_chan_yield()` asks one
eligible holder to give its channel up: a bound **BULK IN** pipe whose
executing buffer is a **single-packet read** (`num_bytes <= MPS`) — the
parked-listener pattern (cdc-acm keeps 64-byte read URBs posted). Single
packets are atomic on the wire, so when the requested halt lands the buffer
has transferred either nothing (it is simply un-executed: `executing`
cleared, to-exec count untouched, so the ordinary exec path re-runs it on
rebind with the preserved toggle) or everything (the completion event wins
and the yield dissolves). The yielding pipe re-queues at the **tail** of the
waiter queue, which round-robins channel time across any number of parked
listeners. A real halt command always takes precedence over a pending yield.

### Phase 2b: parked-listener bind cap + rotation tick

A bound parked listener NAK-polls the bus continuously; on a full-speed bus,
seven parked console readers saturated the bus and starved a concurrent
mass-storage read into SCSI timeout. Phase 2b therefore **caps** how many
parked listeners (bulk IN pipes whose next transfer fits in one packet,
`num_bytes <= MPS`) may hold channels concurrently:
`CONFIG_USB_HOST_PARKED_IN_BIND_CAP` (default 3, 0 = uncapped). A
parked-class bind over the cap is refused *distinctly* from channel
exhaustion — the pipe waits in the ordinary waiter queue, but
`_port_chan_waiters_kick` skips it and keeps serving later waiters instead
of stalling the queue. Multi-packet transfers are never capped. A periodic
**rotation tick** (`CONFIG_USB_HOST_PARKED_IN_ROTATE_MS`, default 20 ms; an
esp_timer per port, started when a pipe enters the waiter queue and
self-stopping when the queue drains) requests a phase-2 yield while any
waiter has pending URBs, so over-cap listeners rotate onto the bus and each
still gets regular bus time — worst-case extra latency is roughly
(listeners / cap) ticks. Yield victim selection now prefers pipes counted
against the cap (`parked_bound`), with the single-packet-buffer check kept
as the atomicity guard.

### Phase 2c: packet-count threshold + partial-progress yield

A hardware retest showed the phase-2b cap never engaged: classification
required `num_bytes <= MPS`, but real console readers post multi-packet URBs
(Linux cdc-acm keeps 2×MPS = 128-byte reads posted, cp210x larger still), so
no reader was ever classified parked — all 7 bound channels, NAK-saturated
the full-speed bus, starved a concurrent mass-storage read into SCSI timeout,
and rotation never fired (victim selection requires `parked_bound`). Phase 2c:

- The parked-class threshold becomes `num_bytes <=
  CONFIG_USB_HOST_PARKED_IN_MAX_PACKETS × MPS` (default 4, range 1–16),
  covering cdc-acm/cp210x read URBs while mass-storage data transfers stay
  far above it. The same threshold guards yield-victim selection.
- Classification now looks at the pipe's *next transfer* — the oldest
  filled-but-unexecuted buffer's URB when one exists, else the first pending
  URB. After a phase-2 yield the un-executed buffer's URB has left the
  pending queue, so the old pending-queue-only classification saw an empty
  queue on rebind: the yielded listener rebound uncapped (losing its
  `parked_bound` marker) and, with a single posted URB, was even dropped from
  the waiter queue with its buffer stranded. The waiter-drop and
  rebind-on-clear conditions now also count un-executed buffers as work.
- With a multi-packet threshold, a yield halt can land mid-transfer with some
  whole packets already received by DMA. The ISR reads the remaining transfer
  size the controller writes back into the in-progress QTD at halt: zero
  progress keeps the old un-execute-untouched path; partial progress retires
  the QTD as a successful short completion and routes it through the normal
  buffer-parse/URB-done pipeline (partial `actual_num_bytes`, status
  completed, pipe event + callback exactly as a short-packet completion),
  then unbinds and kicks waiters. Only whole packets land, so the partial
  count is a multiple of MPS.
- Stale-QTD fix (zero progress). A halt writes the in-progress QTD back
  (remaining `xfer_size`, possibly active cleared). The zero-progress path
  previously cleared only the executing flags, so rebind re-executed a
  written-back descriptor: the controller raised BNA
  (`ERROR_URB_NOT_AVAIL`), the ERROR branch reached `_buffer_parse_error`,
  and the default case aborted. The zero-progress path now re-arms QTD[0]
  with `_buffer_fill_bulk()` (the same call `_buffer_fill()` uses for bulk
  IN, with the same flags and no MPS rounding) followed by the C2M cache
  sync, so the buffer re-executes as a freshly filled transfer. Parked-class
  buffers are bulk IN with `num_bytes <= CONFIG_USB_HOST_PARKED_IN_MAX_PACKETS
  × MPS`, which `_buffer_fill_bulk()` always fills as one QTD; the
  victim-selection guard restricts yields to bulk IN accordingly.
- Partial-progress `stop_idx`: `_buffer_parse_bulk()` never reads the stop
  index (it parses QTD[0] only), so the value does not change the result. The
  partial path now passes `usb_dwc_hal_chan_get_qtd_idx()`, matching the CPLT,
  ERROR and non-yield HALT_REQ paths.
- Fork divergence (defensive, not upstream): `_buffer_parse_error()`'s default
  case (an unexpected pipe event, normally URB_DONE or
  ERROR_URB_NOT_AVAIL) logs with `ESP_EARLY_LOGE` (ISR-safe) and sets the URB
  to `USB_TRANSFER_STATUS_ERROR` with `actual_num_bytes = 0`, instead of
  calling `abort()`. Upstream ESP-IDF aborts here. A residual race should
  fail one transfer, not reboot a multi-device bridge.

### Phase 2d: queue-depth parked profile, promotion, round-robin rotor

Hardware isolation (1 MiB MSC read, dynamic channels): with no parked readers
the read passes in 1.4 s. With three parked console readers filling the cap,
the same read times out with periodic device resets, and claims showed more
parked-class pipes than consoles. Cause: the MSC bulk-IN CSW status read
(13 bytes) matched the size-only parked test (`num_bytes <= 4 x MPS`). Each
SCSI command's status phase was then cap-refused into the waiter FIFO when
consoles held the cap, or, once bound, was a rotation-yield victim and was
thrashed off the channel every tick before the device returned status.
Victim selection also always picked the first parked pipe, so one listener
could be victimized every tick while later ones never yielded.

- Queue-depth rule. The parked profile is BULK IN, next transfer
  `num_bytes <= CONFIG_USB_HOST_PARKED_IN_MAX_PACKETS x MPS`, AND at least
  two queued reads. Queued reads are `num_urb_pending +
  buffer_num_to_exec` (`buffer_num_to_exec` includes the executing buffer;
  done-but-unparsed buffers are excluded). Linux cdc-acm posts 16 reads and
  cp210x 2; an MSC CSW is a single URB. A single small read binds uncapped
  and is never a yield victim.
- Promotion. A cdc-acm pipe's first URB binds at depth 1 (uncapped), then
  its later reads arrive. `_pipe_maybe_promote_parked()` runs on URB enqueue
  to a bound pipe and in the rotation tick, before victim selection; a pipe
  that now matches the profile gets `parked_bound` and increments
  `num_parked_bound`. Promotion can take the count above the cap transiently;
  the cap gates only new binds, and rotation converges it. Demotion is not
  needed: unbind clears `parked_bound`.
- Rotor. Each port keeps `yield_cursor`, the last victim. The yield search
  starts at the pipe after the cursor in the active list and wraps once
  around. The cursor is cleared when its pipe unbinds or is freed, so it
  cannot dangle.

### Phase 2e: over-cap rotation and the parked channel reserve

Hardware retest of 2d (c65d76b): three parked consoles plus a storage read
passed, but five parked consoles did not, for two structural gaps:

- Cap never engaged. Each console's first URB binds at queue depth 1
  (uncapped by design), and promotion then marks it parked. All five ended up
  bound, the waiter queue stayed empty, and the rotation timer only ran while
  waiters existed, so it never ran. Five bound NAK-pollers saturated the bus.
- No channel headroom at 16/16. After the resets, device re-claims need INTR
  pipes, whose allocation is static and hard-fails with
  `ESP_ERR_NOT_SUPPORTED`. The console logged about 1100 "No more HCD channels
  available" messages and interface claim failures: a reset and re-claim
  death spiral that offlined the disk.

Changes:

- Over-cap rotation. The rotation tick runs while a waiter has pending URBs
  OR the parked count exceeds `CONFIG_USB_HOST_PARKED_IN_BIND_CAP`. A
  promotion that leaves the count above the cap requests the timer start from
  URB enqueue, outside the HCD lock, as the waiter path already does. (Phase 2f:
  the tick no longer stops itself; see below.)
  The tick yields one parked holder. That holder re-queues and rebinds through
  the capped path, which refuses it while the count is still at the cap, so
  the system settles at exactly the cap with the rest queued and rotating
  round-robin.
- Parked channel reserve (`CONFIG_USB_HOST_PARKED_IN_CHAN_RESERVE`, default 2,
  range 0-8). A parked-sized bind is refused unless MORE than the reserve
  channels are free (phase 2f: see below). Non-parked binds (control, mass-storage data and status,
  and static INTR/ISOC allocation) may use the reserve. Free channels are
  `hal->constant_config.chan_num_total - hal->channels.num_allocated`, read
  inside the HCD critical section, the same bookkeeping the HAL allocator uses.
  The refusal reuses the cap-refusal path, so the pipe waits and the waiter
  kick skips it.
- A depth-1 initial bind of a future console is not parked-marked, so the
  reserve does not gate it, and the cap does not count it. Such a pipe is
  promoted on its next enqueue, and rotation corrects any excess. This is
  accepted: the bind cannot know the future queue depth.

### Phase 2f: audit, reserve scope, observability

Desk-check of the eviction chain (promotion -> over-cap timer start -> tick ->
victim halt -> ISR yield -> capped rebind), after the 2e retest showed zero
evictions and zero waiters at five parked consoles:

- Promotion return value and both call sites, the enqueue timer start for the
  promotion-only case, and the tick's branch order (over-cap checked before any
  stop) were verified against the code. The timer uses `ESP_TIMER_TASK` with the
  port as its argument. `buffer_is_executing` is set on exec and cleared on
  done, and `usb_dwc_hal_chan_request_halt()` returns true only when the
  channel's active flag is clear, which a bound, executing channel does not have.
- Defect fixed: the tick called `esp_timer_stop()` from inside the HCD spinlock.
  An enqueue that saw the timer active skipped its start while the stop was
  in flight, which could strand the waiter. The tick now runs once started and
  does its yield work only when needed.
- Reserve scope widened: the reserve gates every parked-SIZED bulk IN bind
  (`num_bytes <= CONFIG_USB_HOST_PARKED_IN_MAX_PACKETS x MPS`), regardless of
  queue depth, so a first bind of a console at depth 1 can no longer take the
  last channels. The MSC CSW is also parked-sized, but it almost always runs on
  an already-bound bulk pipe; if a reserve refusal does hit it, the pipe waits
  and the waiter kick serves it as channels free. Cap accounting still uses the
  queue-depth profile.
- Observability (`usb_host_parked_stats()`): `parked_bound`, `rotor_active`,
  `promotions`, `yields_req`, `yields_done`, `cap_refusals`,
  `reserve_refusals`. Refusal counters count attempts, not distinct pipes.
  Comparing `promotions` with `parked_bound`, and `yields_req` with
  `yields_done`, shows which link of the chain stalls.

Still future:
- Rotation for periodic (interrupt) pipes.
- Fairness/priority classes for the waiter queue (e.g. EP0 first).
