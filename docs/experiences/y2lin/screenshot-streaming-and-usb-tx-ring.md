<p align="right">
  <strong>English</strong> · <a href="screenshot-streaming-and-usb-tx-ring.zh_CN.md">简体中文</a>
</p>

# FAP_SCREENSHOT_V1, Second Landing: Streamed Capture and the USB TX Ring

While publishing **AP_CyberClock** (the cyberpunk clock, another AI Passport app) to the
community market I implemented the `FAP_SCREENSHOT_V1` protocol a second time, covering
firmware v10.4 → v10.4.2 (commits `59a821e` through `855138f`). The first entry
([serial-screenshot-protocol](serial-screenshot-protocol.md)) took the snapshot route:
render the whole screen once into a statically reserved buffer, then send it. This entry
records the other route — intercepting the display flush callback and streaming the frame
out band by band — plus the USB driver TX ring and Web Serial host-side pitfalls the first
landing never hit. Both routes work on the PSRAM-less ESP32-C3; selection criteria at the end.

## Enlarge the TX ring to 4KB at driver-install time

The `usb_serial_jtag` driver transmit side is a FreeRTOS ring buffer whose default
`tx_buffer_size` is only 256 bytes, and `xRingbufferSend()` requires entries to be placed
contiguously: any single write larger than the ring fails immediately ("data will never
ever fit") without a single byte entering. Before v10.4.2 every capture answered
`ERR SHOT`, and the host reported "pixel data ended before the declared length, 10/153600"
— those 10 bytes were actually the device's `ERR SHOT\r\n` text, not pixels. The fix does
both halves: set `tx_buffer_size = 4096` explicitly when installing the driver, and send
pixel payloads in 1KB pieces. Two lessons: size the pieces against the TX ring capacity,
not against the payload; and when the host reports "data too short", first suspect the
device actually answered with an error line.

## On a PSRAM-less board, a 150KB whole-frame allocation always fails

v10.4 initially followed the snapshot idea and `malloc(153600)` (240×320×2) from the
system heap. That works when the heap is roomy, but on real hardware with LVGL, NimBLE
and the task set running, the largest free contiguous block never reaches 150KB, so every
capture returned `ERR SHOT`. v10.4.1 switched to streaming: temporarily wrap the display
flush callback, copy each 20-row band into one of three ring slots (240×24×2 = 11.5KB per
slot, 34.5KB total — about 1/4.4 of the old peak), and have the USB task send while the
capture side fills; the producer sleeps for backpressure when the ring is full, and a
shared abort flag keeps failure paths clean. On PSRAM-less chips, never count on the
runtime heap for any contiguous buffer approaching full-screen size.

## LVGL 9.2 has no public flush-callback getter

Streamed capture needs to temporarily replace `flush_cb`, but LVGL 9.2 lacks a public
`lv_display_get_flush_cb()` (added only in 9.5). The only way out is including
`display/lv_display_private.h` and reading `disp->flush_cb` directly. Beyond touching a
private field, the real hazard is asymmetric hooking: if the unhook path stores the wrapper
itself as the "original" callback, the next hook cycles into infinite recursion. The
defense is a lifecycle state machine for the hook (idle / awaiting hook / active /
sending / aborted): unhook only on paths that are certain the hook is installed, never
touch the callback while active, and keep a belt-and-braces unhook on both the completion
and timeout paths.

## The VFS console translates \n into \r\n — silence all logs inside the binary window

When logs travel the VFS console path, every `\n` becomes `\r\n`, and any log byte that
leaks into the pixel stream shifts the whole image from that byte onward. Call
`esp_log_level_set("*", ESP_LOG_NONE)` before the first byte of the header line and
restore the default level after the last pixel byte. Same conclusion as the first entry,
with one addition: silencing and restoring must cover every exit path (success, timeout,
abort, USB disconnect) — a global mute left behind by one failed transfer turns all later
diagnosis into flying blind.

## Make failures machine-readable: ERR SHOT plus a reason code

v10.4.2 appended reason codes to the `ERR SHOT` reply: `BUSY` (previous capture still
running), `MEM` (ring allocation failed), `HOOK` (hook not installed within 3s), `DATA`
(no first band within 3s after hooking), `STALL` (no new band for 3s mid-stream), `USB`
(USB write failed), `SHORT` (short send). Fixing `MEM` in v10.4.1 and `USB` in v10.4.2
each took one glance at the code, and the web capture page now surfaces the reason
instead of retrying blindly. Define machine-readable failure semantics in the first
version of any debug protocol — it is far cheaper than retrofitting logs.

## Four Web Serial host-side disciplines

- **Hold DTR/RTS permanently low**: otherwise the level edge when closing the port trips
  the ESP32-C3 built-in download-reset path and the device reboots on the spot (this app
  already paid for that lesson in v9.1; it applies to capture sessions too).
- **One reader, pump loop**: the serial read loop owns the port and consumes sequentially;
  never race concurrent readers.
- **Every read carries a timeout**: an unbounded `read()` freezes the whole page.
- **Surface device error lines**: do not silently drop non-pixel bytes — they are often
  exactly `ERR SHOT <reason>`, the key evidence for diagnosis.

## Publishing-flow side notes (publisher facts)

- The screenshot receipt is **valid for 24 hours**: re-shoot the connection proof on the
  spot when submitting; yesterday's receipt does not count.
- **A new version is rejected while another is under review** ("a version is already
  under review"): publishing must be serialized — wait for the review to finish before
  submitting the next one.

## Generalizing to the next app

- Payload piece size = min(app need, TX ring capacity − headroom); configure the TX ring
  explicitly at driver install and never ride the default.
- On PSRAM-less chips, replace any contiguous buffer budget beyond ~1/8 of the screen
  with band-by-band streaming plus bounded ring slots.
- Check whether your LVGL version exposes a public flush getter before hooking; if you
  must read a private header, pair it with symmetric unhooking and a recursion-proof
  state machine.
- Binary protocol and text logs are mutually exclusive in time; mute/restore must cover
  every exit path.
- Snapshot vs streaming: pick snapshot when you need a consistent whole frame at any
  moment and can reserve a full-screen buffer statically; pick streaming when the heap
  is tight, you want render-and-send overlap, or plan to compress on the fly.

## Related documents

- [`serial-screenshot-protocol.md`](serial-screenshot-protocol.md) — the first experience entry for this protocol (snapshot route).
- [`../shinku-chen/post-release-follow-up.md`](../shinku-chen/post-release-follow-up.md) — the wrap-up view of the same publishing flow.
- <https://github.com/Y2Lin/AP_CyberClock> — the implementation behind this entry (`main/usb_time_sync.c`).
- [`../../CHANGELOG.md`](../../CHANGELOG.md) — per-version records for v10.4 through v10.4.2.
