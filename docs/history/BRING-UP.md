# Akbar's music matrix

First bring-up: September 23, 2026. The folder retains the original Spotify
project name, but the display test works independently of the music service.
User now prefers YouTube Music and listens on both Mac and phone.

## Hardware

- Waveshare-style ESP32-S3-RGB-Matrix controller; USB chip query confirmed
  ESP32-S3 revision 0.2, 32 MB **octal** flash, 16 MB embedded PSRAM.
- Waveshare P3 64x64 panel, one panel only.
- 5 V USB-C wall supply into POWER; programming USB-C into Mac.
- Controller female HUB75 socket fits **directly onto the panel JIN header**.
  JOUT stays empty. The included female/female ribbon is not needed here.
- Controller 5V screw terminal -> red -> panel VCC; GND -> black -> panel GND.
- Speaker connected by user. Old classroom Raspberry Pi microSD inserted.
  This test does not access the SD card, microphones, speaker, or Wi-Fi.

## First display test

`display_test/display_test.ino` shows red/green/blue/white quadrants for three
seconds, then a synthetic spinning record with AKBAR below it. Brightness is
40/255, approximately 16%. This is an animation test, not live album art.

Build prerequisites: Arduino ESP32 core **3.3.7**, Adafruit GFX Library and
its Adafruit BusIO dependency. Arduino CLI is taken from the installed macOS
Arduino IDE. The driver snapshot and upstream commit are in `vendor/waveshare`.

```sh
python3 build_test.py
python3 build_test.py --upload --port /dev/cu.usbmodem101
```

The build explicitly uses OPI flash/PSRAM because the connected chip reports
octal flash in eFuse. The generic vendor screenshot instead shows QIO.

## Verification

- User reported the factory sensor demo displaying temperature and humidity.
- Hardware query confirmed flash/PSRAM configuration.
- Arduino ESP32 3.3.7 compilation passed: 364,410 bytes program storage,
  24,036 bytes static RAM.
- Uploaded to `/dev/cu.usbmodem101`; esptool verified written flash hashes.
- Runtime serial confirms 32 MB flash, 16 MB PSRAM, completed quadrant stage,
  animation READY, and an OK heartbeat at five seconds with 265,560 bytes free
  heap. Captured in `build/runtime-check.txt`.
- Akbar confirmed the spinning record and AKBAR label looked correct on the
  physical panel. This validates the initial drawing configuration.

## Factory backup

`backups/factory-2026-09-23.bin` contains a completed read of all 33,554,432
bytes of flash. SHA-256 is recorded in the adjacent JSON manifest and is checked
by the upload script before replacing firmware. Restoring it has not been tested.
No microSD formatting or file changes are part of this process.
Keep flash backups private because firmware storage can contain configuration.

To restore the factory image later, from this directory, with the board plugged
into USB (this command replaces the display test):

```sh
../.tools/esp-venv/bin/python -m esptool --chip esp32s3 --port /dev/cu.usbmodem101 --baud 921600 write-flash 0 backups/factory-2026-09-23.bin
```

The normal core installer exhausted free disk space while installing tools for
unrelated chips. Only newly downloaded/installed unused components were removed.
The official core and ESP32-S3 library ZIPs were verified against the package
index SHA-256 checksums and extracted to Arduino's package directory. This is an
ESP32-S3-only installation; other ESP32 variants will need their own components.

## Music integration

The Chrome integration is in `album_display`, `extension`, and `companion`; see
[connection instructions](CONNECT-YOUTUBE-MUSIC.md). Wi-Fi transport and direct
iPhone Bluetooth metadata/online cover lookup are implemented for wall use.
See [wall setup and verification requirements](WALL-SETUP.md). Pairing and a
real wireless playback test are required before calling it ready to mount.
Music continues playing on the source device; the speaker is not a music client.
