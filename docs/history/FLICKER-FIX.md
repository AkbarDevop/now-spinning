# Flicker investigation

Status: Akbar confirmed the higher-refresh/static-pause version fixed the visible flicker.
The first buffer timing correction alone was insufficient.

## Symptom and cause

Akbar reported visible flicker even with the song paused. The bundled Waveshare
ESP32-S3 driver changes the DMA descriptor chain in `flip_dma_output_buffer()`
and returns immediately; its old-buffer completion wait is commented out.
The animation schedules from the start of its previous render, then clears its
new back buffer. When a render consumes most or all of the 33 ms frame interval,
that clear can happen before DMA has stopped scanning the old displayed buffer.

Hardware diagnostics reproduced unsafe reuse with PLAY and PAUSE:

| State | Maximum render time | Minimum buffer reuse gap |
|---|---:|---:|
| Before, playing | 38.47 ms | 0.005 ms |
| Before, paused | 44.15 ms | 0.008 ms |
| After, playing | 19.61 ms | 19.787 ms |
| After, paused | 25.31 ms | 19.463 ms |

The driver estimates 105 Hz for its configured 8 MHz clock. Its S3 backend
actually selects 10 MHz for this setting, so the estimate is conservative.
These are driver-derived scan timings, not oscilloscope measurements.

## Change

`album_display/frame_timing.h` calculates a 19,448 microsecond reuse interval:
two complete driver-estimated scans plus a 400 microsecond pipeline margin.
`album_display.ino` checks this interval before any framebuffer clear or drawing,
including when a previous render overruns its frame budget. Subtraction handles
the micros counter wrapping. The bound is tied to the bundled driver/configuration.

Static per-pixel disc shading is calculated once instead of recalculating
square roots, cosines, and powers each frame. Steady draw time fell from about
32 ms to 13.2 ms. A regression check compares twelve rotation/crossfade states
against the previous renderer checksum; the pixels are identical. The small
center hole, disc edge, 4 RPM, brightness, and USB protocol remain unchanged.

## Verification

- Two native firmware tests passed. The actual `loop()` refuses to touch a DMA
  buffer before the interval, including a forced overdue frame and clock wrap.
  Removing the guard reproduces the test failure.
- Five companion/HTTP boundary tests passed.
- ESP32 compilation succeeded: 369,918 bytes program, 130,708 bytes static RAM.
- Upload completed with flash hashes verified.
- Every post-fix hardware timing sample respected the reuse interval in PLAY
  and PAUSE. The board acknowledged artwork and title packets.
- The live Chrome feed reconnected with Slip Away by Tokyo Tea Room, paused.

Evidence: `build/flicker-before-timing.txt`, `build/flicker-after-timing.txt`,
`build/flicker-fix-upload.log`, and `build/flicker-fix-live-status.json`.
Regression tests: `tests/test_frame_timing.py` and `companion/test_bridge.py`.

This verifies the buffer race correction. Only Akbar can confirm the visible
result on the actual panel; scan-rate flicker, camera banding, or a separate
power/wiring issue would require further investigation if flicker remains.

Rollback: `backups/before-flicker-fix-20260923-224338/` contains the prior sketch, design settings,
and binaries. `frame_timing.h` is only used by the new sketch.

## Follow-up: flicker persisted while paused

The initial buffer correction did not eliminate the reported visual problem.
The second trial changes the minimum requested scan rate from 60 to 240 Hz
without changing the pixel clock or brightness setting. The bundled driver
reports its calculated rate increasing from 105 to 300 Hz. It achieves this
by changing the low-bit/high-bit scan schedule; physical color quantization
may therefore differ even though the rendered RGB565 artwork is unchanged.
These rates are driver calculations, not optical measurements.

Settled paused images now stop drawing and swapping buffers completely, while
the hardware continues scanning the same image. New artwork or playback resume
reactivates drawing. On hardware, the paused log reached `frames=0`, and resume
returned to 60 frames over a two-second reporting window. Every observed reuse
gap exceeded the new 7,067 microsecond bound. Eight tests passed, including
actual-loop checks that a paused image stays untouched while new artwork and
resume still render. The Chrome bridge reconnected to Slip Away, paused.

Evidence: `build/refresh-test-timing.txt`, `build/refresh-test-upload.log`, and
`build/refresh-test-live-status.json`. Akbar subsequently confirmed: "yes its good now." This is the visually confirmed
configuration to preserve in further changes.
Rollback: `backups/before-refresh-test-20260923-225251/`.
