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

Still future:
- Rotation for periodic (interrupt) pipes.
- Fairness/priority classes for the waiter queue (e.g. EP0 first).
