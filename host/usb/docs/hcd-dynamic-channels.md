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

Still future:
- Rotation for multi-packet bulk IN and for periodic (interrupt) pipes —
  both need partial-progress resume, not just atomic re-execution.
- Fairness/priority classes for the waiter queue (e.g. EP0 first).
