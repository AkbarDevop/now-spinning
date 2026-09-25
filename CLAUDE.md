# Now Spinning — context for AI assistants

A 64×64 LED "picture disc" that spins the cover of whatever Akbar is playing:
YouTube Music on the Mac (Chrome extension) or on the iPhone (Bluetooth, Apple Media Service).
Read this first; `docs/history/` has the long Codex-era bring-up logs if you need detail.

## Hardware
- Waveshare ESP32-S3-RGB-Matrix: ESP32-S3 rev 0.2, **32 MB octal flash, 16 MB octal PSRAM**,
  QMI8658 IMU + SHTC3 on I2C (SDA 47, SCL 48), ES8311 codec + mics + speaker
  (I2S MCLK 12, BCLK 43, WS 38, DOUT 21, DIN 39, amp enable 11), BOOT button GPIO 0, microSD.
- Waveshare P3 64×64 HUB75 panel (192 mm). Controller plugs straight onto the panel's JIN header.
- 27 W USB-C supply → board's **POWER** port. The other USB-C is data (only needed for rescue).

## Data flow
- **Mac:** YouTube Music tab → `extension/content.js` (reads player bar) → `background.js` (fetches art,
  64×64 RGB565) → `POST 127.0.0.1:18765/frame` → `companion/bridge.py` (LaunchAgent
  `one.akbar.music-matrix`, installed copy in `~/Library/Application Support/Akbar Matrix/`)
  → `POST http://<display>:18766/frame` with `X-Matrix-Token`.
- **iPhone:** iOS → BLE AMS (`album_display/phone.cpp`, NimBLE GATT client, bonded) → title/artist/album/state
  → `wireless.cpp` artwork task → Apple Search API → Deezer → `songArt()` generated fallback.
  AMS never sends images, so regional songs/covers can miss.
- **Taps:** `imu.cpp` (accelerometer, software tap detection) → `wirelessCommand()` → iPhone via AMS
  Remote Command, or Mac via a `command` field in the `/frame` reply → extension clicks the YT Music button.
  Double tap = next, triple tap = play/pause.
- **Rendering:** `album_display.ino` loop (core 1) alone owns the HUB75 DMA buffers; network/artwork/gesture
  tasks on core 0 publish complete frames through a mutex-protected PSRAM mailbox.
  Idle 5 min → clock; 23:00–07:00 → brightness 16 (dark when idle). Day brightness 40/255.

## Commands
```sh
./matrix init           # first clone only: generate the gitignored pairing token files
./matrix build          # compile (sets TMPDIR, writes album_display/build_info.h)
./matrix flash          # build + install over Wi-Fi (OTA, password = pairing token)
./matrix flash --usb    # rescue path over USB (app partition only); add --full on a brand-new board
./matrix status         # Wi-Fi, iPhone, cover source, taps, memory
./matrix demo clock     # preview idle screens for 60 s (clock | dark | dim | off)
./matrix test           # host-side unit tests (needs .venv with pyserial)
./matrix helper         # reinstall the Mac helper after editing companion/bridge.py
```

## Gotchas (learned the hard way)
- **Opening the USB serial port resets the ESP32-S3** (reset reason `USB`, code 11). The helper only falls
  back to USB after 10 min without Wi-Fi. Leave the data cable unplugged in normal use.
- arduino-cli's ctags fails with "cannot open temporary file" when `TMPDIR` is unset — `./matrix` sets it.
- USB uploads: write only the app at `0x10000` with `--after watchdog-reset` (a plain hard reset left the S3
  in ROM download mode). This keeps NVS: Wi-Fi credentials and the iPhone bond.
- USB not detected → hold BOOT, tap RESET, release BOOT.
- Internal RAM is tight (~20 KB free with BLE + Wi-Fi + TLS). TLS allocations are redirected to PSRAM;
  put any new big buffer in PSRAM (`ps_malloc`) and check `./matrix status` memory after changes.
- During OTA the network task is blocked inside `ArduinoOTA.handle()`; the renderer shows a progress bar.
- Flicker fix: 300 Hz driver scan, buffer-reuse fence (`frame_timing.h`), never redraw a settled paused frame.
- iPhone pairing lives in NVS (NimBLE store). If it ever breaks: iPhone Settings → Bluetooth → Forget
  "Akbar Matrix", reconnect once with nRF Connect. iOS then reconnects on its own.
- Secrets (gitignored — never commit or print): `album_display/secrets.h`, `companion/pairing-token.txt`,
  `extension/pairing.js`.
- The Chrome extension is loaded unpacked from `extension/`; after edits reload it in `chrome://extensions`
  and refresh the YouTube Music tab.

## Status (Sept 24, 2026)
Working and verified on hardware: Wi-Fi OTA, iPhone auto-reconnect after power cycle, Deezer covers,
per-song art, tap routing to phone and Mac, night mode + idle clock.
Next ideas: mic-reactive edge glow (watch internal RAM), printed frame + wall mount, portfolio write-up
with a demo video and measured numbers.
