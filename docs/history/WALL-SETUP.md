# Wall setup

## Current installation status (September 24, 2026)

**Wall-powered Mac playback is confirmed:** after disconnecting both USB cables
and reconnecting only the wall adapter, the user confirmed that real Mac music
covers display over Wi-Fi. This recovered the black panel; its original cause
is not established. Earlier serial telemetry showed about 30 rendered frames/s
with the existing 300 Hz scan configuration and safe buffer reuse delays.
Evidence: user confirmation and build/black-panel-serial.log.

**Remaining issue: iPhone reconnection after a cold boot.** Fresh pairing has
successfully delivered metadata and artwork, but a subsequent restart again
left the Bluetooth link connected without encryption (NimBLE timeout 13).
Diagnostic build `phone-bond-check-1` compiled successfully (1,462,627 program
bytes, 83,200 static RAM bytes), and the 16 existing software tests passed. It
checks restored bond records and delays the initial security request. Upload
is pending because macOS detects no USB serial device. The running firmware
remains `phone-diagnostics-1`; this is not yet a verified fix.

The current firmware compiles (1,461,907 bytes; 83,160 bytes static RAM) and all
16 software tests passed. Firmware `phone-diagnostics-1` was uploaded with its
flash hash verified in build/phone-diagnostics-upload.log and confirmed over Wi-Fi.
This adds Wi-Fi-readable Bluetooth handshake diagnostics. The phone initially
reached BLE link establishment but encryption timed out:
`link=1 encrypted=0 subscribed=0 phase=1 error=13 busy=0`.
NimBLE error 13 is BLE_HS_ETIMEOUT. The user confirms nRF Connect shows Connected;
that does not yet mean secure pairing or an Apple Media Service subscription.
After the user forgot Akbar Matrix in iPhone Bluetooth Settings and reconnected,
the board reported `link=1 encrypted=1 subscribed=1 phase=6 error=0 busy=0`.
It received the Las Ketchup title/artist and successfully downloaded and decoded
the artwork (`Matched cover`). Playback was paused at verification. The old
pairing state was implicated by recovery after re-pairing; its original cause
is not established. Evidence: build/phone-diagnostics-live.json.

The artwork investigation found Arduino's TLS library restricted to internal
memory, with two 16 KB record buffers but only about 29 KB internal heap free.
TLS allocations now use PSRAM, installed before the radio tasks start; certificate
verification remains enabled. Status distinguishes transport, catalog, JSON,
and JPEG errors instead of reporting every failure as an absent cover. The Mac
verified a matching Las Ketchup catalog entry and its 64x64 baseline JPEG.
The updated board reports over 16 MB free PSRAM. A successful on-device artwork
lookup is now verified after re-pairing; automatic iPhone reconnection still needs a successful live test.
Wall-only operation with Mac playback is user-confirmed.
Diagnostic evidence: build/cover-debug.json and build/cover-fix-live.json.

Recovery required manual BOOT/RESET entry. Native USB Serial/JTAG remained in
ROM download mode after the default hard reset, confirmed by a successful ROM
read-mac command while application packets received no ACK. Espressif's supported
`--after watchdog-reset` exited that mode; application PLAY/PAUSE and performance
reports then resumed. The manual upload script now uses that reset method.
This explains the post-upload failure to start; it does not establish the cause
of the earlier USB loss before manual recovery.

USB close/reopen tests passed after both 2-second and 12-second gaps. PLAY/PAUSE
and 300 Hz driver scan configuration passed, including zero redraws while paused.
The per-user LaunchAgent is running, auto-start at login is enabled, and the
local setup page responds HTTP 200.
Home Wi-Fi has been provisioned. The display returned an HTTP status over Wi-Fi
at 192.168.1.141 and accepted real extension updates over Wi-Fi. Mac playback with USB unplugged is user-confirmed; reliable iPhone
reconnection remains outstanding. A CoreBluetooth scan on the
Mac found connectable advertisements named Akbar Matrix, soliciting Apple Media
Service, with RSSI between -32 and -38 dBm. The user reports it does not appear
in the iPhone's standard Bluetooth Settings list. After the user connected from
the phone, the board reported an active media subscription and received
"The Ketchup Song (Aserejé) (Spanish Version)" by Las Ketchup, paused. The cover
initially failed, but now loads with the TLS memory fix and renewed secure pairing.
Play/pause/skip, automatic reconnection, and Mac-independent operation remain
unverified.
Evidence: build/wireless-manual-upload.log, build/wireless-exit-download.log,
build/wireless-retry-hardware.txt, and build/wireless-install-status.json.

Reference: [Espressif USB download-mode troubleshooting](https://github.com/espressif/esptool/blob/master/docs/en/troubleshooting.rst).

## One-time setup

Keep the 5 V adapter in POWER and the Mac data USB cable connected during setup.
Open <http://127.0.0.1:18765/setup> while the companion is running. Enter the
2.4 GHz home Wi-Fi SSID and password there. Credentials travel through USB and
are stored in the ESP32's NVS; the helper does not save the password on the Mac.
This form supports ordinary open/WPA personal networks, not enterprise login
or captive portal authentication. Campus networking needs a separate decision.

When status reports Wi-Fi and an IP address, unplug only the data cable and
verify a real Mac track change. Keep wall power connected. The extension and
YouTube Music Chrome tab still need an awake Mac for Mac playback.

For iPhone, check Settings → Bluetooth for Akbar Matrix. The standard Settings
list may omit this BLE accessory. If absent, use
[nRF Connect for Mobile](https://apps.apple.com/us/app/nrf-connect-for-mobile/id1054362403),
allow Bluetooth, scan for Akbar Matrix, tap Connect, and accept pairing if
prompted. This connection path has delivered song metadata on this phone. Start
YouTube Music, then check that the setup page reports the correct track and
playback state. Test pause/resume, skipping, locking the phone, and putting the
Mac to sleep. This is a BLE metadata accessory: it does not take over the phone's
audio output. The firmware is configured to persist pairing bonds; persistence and encrypted
reconnection are currently under investigation. Advertising resumes after a
disconnect so the phone can reconnect; verify automatic reconnection on this
phone before mounting permanently.

## Artwork and source selection

Apple Media Service exposes artist, album, title, and playback state, but no
cover image. The board searches Apple's public music catalog over HTTPS using
the artist and title, and accepts a normalized exact artist/title match,
preferring the same album. It decodes a 64×64 JPEG itself. Thus iPhone mode is
designed to operate without the Mac once paired and connected to Wi-Fi.
Covers for YouTube-only uploads, unofficial remixes, or unmatched metadata may
not exist in that catalog; a neutral record is shown instead of a previous
song's cover. This lookup sends artist/title to Apple. Bluetooth metadata stays
local. Requests are bounded, rate-limited, and TLS certificate-verified; no
search result can redirect the board to an arbitrary image host.

Playing iPhone audio has priority. A paused Mac's repeated messages cannot
replace the phone's record. When the phone pauses, playing music on the Mac
can take over; paused sources do not keep stealing the display from each other.
The renderer retains the last image and coasts to a stop after 15 seconds
without an update. Idle state does not turn the panel's power supply off.

## Mac helper

The existing Chrome extension is unchanged. It sends to the local helper on
127.0.0.1:18765; the helper sends authenticated frames to the display on port
18766, preferring the learned local IP and falling back to akbar-matrix.local.
USB remains a fallback. No port forwarding is needed. Use a trusted local
network; local frame transport is HTTP, not encrypted HTTPS.

Install automatic startup, using the project's Python environment:

```sh
../.tools/esp-venv/bin/python companion/install_autostart.py
```

This creates the per-user LaunchAgent `one.akbar.music-matrix`, starts the
helper at login, and restarts it after a crash. It does not launch Chrome.
Remove automatic startup with the same command plus `--remove`.
The installed helper and private logs are under
`~/Library/Application Support/Akbar Matrix/`. This keeps login startup independent
of the Documents project folder. Run the installer again after changing helper code.
The setup page can be revisited anytime.
Do not publish pairing-token.txt, extension/pairing.js, album_display/secrets.h,
flash backups, or companion/device.json.

## Architecture and build

The render loop alone owns display buffers. Network and artwork workers run on
core 0 and publish a complete frame through a mutex-protected PSRAM mailbox.
Static disc geometry also moves to PSRAM, leaving internal RAM for Bluetooth,
Wi-Fi, and DMA. The confirmed flicker fix remains: requested scan minimum 240,
driver-calculated 300 Hz, 7,067 µs reuse fence, and no redraws for a settled
paused image. These scan rates are driver estimates, not optical measurements.

Additional libraries: ArduinoJson 7.4.2 and TJpg_Decoder 1.1.0. The installed
Arduino ESP32 3.3.7 S3 build uses NimBLE; phone.cpp uses its asynchronous GATT
client on the iPhone-initiated peripheral connection. It solicits AMS, bonds,
subscribes to Entity Update, reads truncated attributes, monitors Service
Changed, and retries discovery. No classic Bluetooth/A2DP, SD files, microphone,
or speaker streaming is used.

USB protocol additions: flag 3 body `[ssid_bytes, password_bytes, SSID, password]`
(3–98 bytes), flag 4 body `?` returns a JSON status suffix. Both use the existing
MXA1 header and CRC. Network POST /frame contains a JSON playing boolean and 8192 RGB565 bytes
encoded as base64 (the Arduino HTTP parser does not preserve raw zero bytes),
authorized with X-Matrix-Token. GET /status requires the same token.
Network provisioning is deliberately unavailable.

Reference specifications:
- [Apple Media Service](https://developer.apple.com/library/archive/documentation/CoreBluetooth/Reference/AppleMediaService_Reference/Specification/Specification.html)
- [NimBLE GATT client](https://mynewt.apache.org/latest/network/ble_hs/ble_gattc.html)
- [Apple Search API](https://developer.apple.com/library/archive/documentation/AudioVideo/Conceptual/iTuneSearchAPI/Searching.html)

Working pre-wireless firmware is retained in
backups/before-wireless-20260923-231556/; the full factory image is also retained.
Rollback restores that sketch/headers and binary, then removes automatic startup
if no longer wanted. It does not require formatting the microSD.
