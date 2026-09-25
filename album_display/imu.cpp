// Tap gestures from the onboard QMI8658 accelerometer (I2C SDA 47 / SCL 48).
// Double tap on the frame = next track, triple tap = play/pause, sent to the
// iPhone through Apple Media Service. Thresholds are tuned from /status.
#include "wireless.h"
#include <Wire.h>
#include <atomic>
#include <math.h>

namespace {
uint8_t imuAddr = 0;
std::atomic<int> imuState{0};             // 0 = not found, 1 = running
std::atomic<uint32_t> taps{0}, doubles{0}, triples{0}, rejected{0};
std::atomic<int> lastPeakMg{0}, lastCommand{-1}, lastSent{-1};

bool writeReg(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(imuAddr); Wire.write(reg); Wire.write(value);
  return Wire.endTransmission() == 0;
}
bool readRegs(uint8_t reg, uint8_t *buffer, size_t count) {
  Wire.beginTransmission(imuAddr); Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(imuAddr, (uint8_t)count) != count) return false;
  for (size_t i = 0; i < count; i++) buffer[i] = Wire.read();
  return true;
}
bool probe(uint8_t address) {
  imuAddr = address; uint8_t id = 0;
  return readRegs(0x00, &id, 1) && id == 0x05;   // WHO_AM_I
}
void fire(int count) {
  uint8_t command = count == 2 ? 3 : 2;           // AMS: 3 = NextTrack, 2 = TogglePlayPause
  bool sent = wirelessCommand(command);
  lastCommand = command; lastSent = sent;
  if (count == 2) doubles++; else triples++;
  Serial.printf("GESTURE taps=%d command=%u sent=%d\n", count, command, sent);
}

void gestureTask(void *) {
  constexpr float LSB_PER_G = 8192.0f;            // +-4 g range
  constexpr float TAP_G = 0.30f;                  // spike above gravity baseline
  constexpr uint32_t MAX_TAP_MS = 60, RING_MS = 70, GAP_MS = 450, COOLDOWN_MS = 1500;
  float baseline = 1.0f, noise = 0.0f;
  uint32_t tapTimes[4] = {}; int tapCount = 0;
  uint32_t spikeStart = 0, lastSpike = 0, cooldownUntil = 0; bool inSpike = false; int peak = 0;
  for (;;) {
    uint8_t s[6];
    if (readRegs(0x35, s, 6)) {                   // AX_L .. AZ_H
      float ax = int16_t(s[0] | s[1] << 8) / LSB_PER_G;
      float ay = int16_t(s[2] | s[3] << 8) / LSB_PER_G;
      float az = int16_t(s[4] | s[5] << 8) / LSB_PER_G;
      float magnitude = sqrtf(ax * ax + ay * ay + az * az);
      float d = fabsf(magnitude - baseline);
      uint32_t now = millis();
      if (!inSpike) { baseline += (magnitude - baseline) * 0.01f; noise += (d - noise) * 0.01f; }
      if (d > TAP_G && d > noise * 6) {
        if (!inSpike) { inSpike = true; spikeStart = now; peak = 0; }
        peak = max(peak, int(d * 1000));
      } else if (inSpike && d < TAP_G * 0.5f) {
        inSpike = false; lastPeakMg = peak;
        if (now - spikeStart > MAX_TAP_MS || int32_t(now - cooldownUntil) < 0) { rejected++; tapCount = 0; }
        else if (now - lastSpike >= RING_MS) {
          taps++; lastSpike = now;
          if (tapCount > 0 && now - tapTimes[tapCount - 1] > GAP_MS) tapCount = 0;
          if (tapCount < 4) tapTimes[tapCount++] = now;
        }
      }
      if (inSpike && now - spikeStart > 300) { inSpike = false; rejected++; tapCount = 0; }  // moving, not tapping
      if (!inSpike && tapCount > 0 && now - tapTimes[tapCount - 1] > GAP_MS) {
        if (tapCount == 2 || tapCount == 3) { fire(tapCount); cooldownUntil = now + COOLDOWN_MS; }
        else if (tapCount > 3) rejected++;
        tapCount = 0;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(2));
  }
}
}  // namespace

void gestureBegin() {
  Wire.begin(47, 48, 400000);
  if (!probe(0x6B) && !probe(0x6A)) { Serial.println("IMU not found"); return; }
  writeReg(0x60, 0xB0); delay(20);                // soft reset
  writeReg(0x02, 0x40);                           // CTRL1: auto-increment, little endian
  writeReg(0x03, 0x14);                           // CTRL2: accel +-4 g, 500 Hz
  writeReg(0x08, 0x01);                           // CTRL7: accelerometer on
  imuState = 1;
  xTaskCreatePinnedToCore(gestureTask, "matrix-gesture", 4096, nullptr, 2, nullptr, 0);
  Serial.printf("IMU ready at 0x%02X\n", imuAddr);
}

void gestureDiagnostics(char *out, size_t capacity) {
  snprintf(out, capacity, "imu=%d taps=%lu double=%lu triple=%lu rejected=%lu peak_mg=%d last_cmd=%d sent=%d",
           imuState.load(), (unsigned long)taps.load(), (unsigned long)doubles.load(), (unsigned long)triples.load(),
           (unsigned long)rejected.load(), lastPeakMg.load(), lastCommand.load(), lastSent.load());
}
