# Connect YouTube Music on this Mac

The first spinning-record display test was visually confirmed by Akbar.
The display receives album artwork and play/pause state from Chrome on the Mac.
Wi-Fi transport and direct iPhone BLE support are now implemented; see
[wall setup](WALL-SETUP.md) for provisioning and pending real-device checks.
Mac playback requires an awake Mac. USB is needed for initial Wi-Fi provisioning,
then serves as a fallback.

## One-time Chrome setup

1. Open `chrome://extensions` in Chrome and enable Developer mode.
2. Click **Load unpacked** and select this project's `extension` folder:
   `<this repo>/extension`
3. Refresh the existing YouTube Music tab. The extension starts reading only
   the current player: title, artist, artwork, and play/pause state.
4. Start `companion/Start Music Matrix.command` and keep its Terminal open.
5. Play a song at `https://music.youtube.com/`.

The extension reads only music.youtube.com, downloads artwork from the listed
YouTube image hosts, and sends a 64x64 image plus metadata to a service bound
to `127.0.0.1:18765` on this Mac. No credentials, cookies, browsing history, or
audio are read by this extension. The Mac path does not send metadata to another third-party service. The separate
iPhone path uses Apple catalog lookup, described in WALL-SETUP.md.
Local requests require the generated pairing token; do not publish `pairing.js`
or `pairing-token.txt`. Those files are ignored by Git.

Chrome's extension popup shows the last bridge result. `ON` means the last frame
was acknowledged by the ESP32; `!` means the bridge, USB device, or artwork
request failed. If you reload the extension, refresh YouTube Music as well.

## Panel behavior

- Current selection: picture disc inspired by Akbar's reference photo. Full-size
  circular artwork, a 3.5-pixel-radius black center hole, a thin material edge,
  faint concentric grooves, and a subtle fixed-light reflection. Outer disc radius
  is 30.5 pixels; printed artwork radius is 29.25 pixels. Rotation remains 4 RPM
  (one revolution every 15 seconds at steady speed). No halo, tonearm, name,
  song title, or other text. The selection is recorded in
  `album_display/design.json`; it documents the compiled settings, not a live
  configuration endpoint.
- Before artwork arrives: a quiet dark disc with the same opening and edge.
- Playing: interpolated artwork rotation at a target 30 frames per second,
  with smooth acceleration and deceleration.
- Track changes: 900 ms artwork crossfade.
- Paused, or no updates for 15 seconds: record coasts to a stop without text.

The image is center-cropped to a square before the circular display mask.
YouTube changes its site occasionally; the current player selectors were
checked against the actual Chrome page on September 23, 2026.

## Firmware

```sh
python3 build_test.py --sketch album_display --upload
```

The speaker and microSD are unused. Audio continues through the Mac's existing
output. Brightness remains 40/255. The factory backup is retained.

USB packets are `MXA1`, then little-endian `uint32 sequence`, `uint16 length`
(8192), `uint16 playing` (0 or 1), `uint32 CRC32`, then 8192 bytes of RGB565
pixels, low byte first. The board validates length and CRC before replacing the
display image and returns `ACK <sequence> PLAY` or `ACK <sequence> PAUSE`.
The turntable version also accepts flag 2 with 1-60 printable ASCII title bytes
and the same CRC/header layout, acknowledging `ACK <sequence> TITLE`. Repeated
artwork and title updates do not restart the transition or title scroll.
The companion sends titles on changes; the Chrome extension is unchanged.
The minimal preset acknowledges these titles for compatibility but does not
draw them.

Stop the companion with Ctrl+C. Remove the extension from Chrome to stop browser
access. The optional per-user login helper is documented in WALL-SETUP.md.

## Verified September 23, 2026

- Album receiver compiled and uploaded with write-hash verification.
- Actual ESP32 acknowledged the Fiesta artwork as PAUSE, matching the Chrome
  player at the time of the test.
- Corrupted-image CRC and incomplete-transfer timeout tests passed on hardware;
  valid frames were accepted after both errors.
- The running loopback HTTP bridge delivered the same artwork successfully over
  USB and received the board's acknowledgement.
- Three HTTP boundary tests passed: unauthorized requests rejected, invalid
  frames rejected, valid paused image preserved. Extension JavaScript syntax
  checks passed.
- Akbar subsequently loaded the Chrome extension and reported that it works.
  The earlier Chrome installation blocker was resolved manually by the user.

## Turntable animation revision

The prior working sketch, companion, and firmware binaries are retained in
`backups/before-turntable-2026-09-23/`. Restoring that version requires restoring
both the firmware and its companion. The factory backup is also retained.

Validation: compiled (370,678 bytes program, 48,772 bytes static RAM), uploaded
with flash hashes verified, and inspected a software-rendered 64x64 preview
using the firmware drawing code and actual artwork. On the physical ESP32,
image/title transfers and play/pause ACKs passed; corrupt CRC, oversized title,
and partial transfer rejection/recovery passed. Five companion tests passed.
Hardware evidence: `build/turntable-hardware-check.txt`. Appearance on the actual
LED panel still needs Akbar's judgment; the preview does not simulate LED optics.
After restarting the companion, the Chrome feed resumed automatically with
Billie Jean by Michael Jackson, playing, and a fresh `ACK 5 PLAY` from the panel.

## Selected minimal design

User selected the minimal circle at 4 RPM with center pin only. The prior
turntable sketch, companion, and binaries are saved in `backups/before-minimal-20260923-223437/`.
Compiled: 367,586 bytes program, 48,772 bytes static RAM. Upload flash hashes
verified. Physical device accepted artwork/title and PLAY/PAUSE frames.
After bridge restart, the live Chrome feed resumed with Stayin Alive by
Bee Gees and a fresh ACK 4 PLAY. Evidence: `build/minimal-hardware-check.txt`
and `build/minimal-live-status.json`. Rendered preview checked; final appearance
on the physical LED panel is for Akbar to judge.

## Picture disc revision

Based on the supplied photo: enlarged black opening, thin bevel, faint grooves,
full-cover printing, 4 RPM, and no text. Previous minimal version saved in
`backups/before-picture-disc-20260923-223724/`.
Compiled (369,614 bytes program, 48,772 bytes static RAM), uploaded with flash
hashes verified, rendered preview inspected, and black center opening checked.
The existing Chrome feed reconnected with a fresh board ACK, recorded in
`build/picture-disc-live-status.json`. Actual LED appearance awaits user judgment.

## Buffer timing correction

The animation now waits for safe buffer reuse after asynchronous DMA swaps.
Static shading is cached to reduce render time without changing the picture.
The USB serial diagnostics include a compact PERF report every two seconds.
See [flicker investigation](FLICKER-FIX.md) for measured before/after timing,
regression checks, and rollback details. Akbar confirmed the final higher-refresh/static-pause version looks good.

The first buffer correction did not resolve all visible flicker. The current
trial requests faster panel refresh (driver-calculated 300 Hz) and stops all
redrawing after a paused image settles. Resume and artwork changes still render.
Akbar confirmed this removed the visible flicker; see FLICKER-FIX.md.
