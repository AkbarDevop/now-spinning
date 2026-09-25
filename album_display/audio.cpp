// Sound-reactive glow: the board's two MEMS mics feed an ES7210 ADC (I2C 0x40,
// I2S slave). We read 16 kHz stereo, track loudness above the room's noise floor
// and detect beats, then publish one 0..255 glow level for the renderer.
// ES7210 register sequence follows Espressif's esp_codec_dev es7210 driver
// (Apache-2.0): slave mode, MIC1+MIC2, 16-bit standard I2S.
#include "wireless.h"
#include <Wire.h>
#include <driver/i2s_std.h>
#include <atomic>
#include <math.h>
#include <freertos/idf_additions.h>

namespace {
constexpr int SAMPLE_RATE = 16000;
constexpr int BLOCK_FRAMES = 128;                 // 8 ms per DMA read; two reads per analysis block
uint8_t codecAddr = 0;
i2s_chan_handle_t rx = nullptr;
std::atomic<uint8_t> glowLevel{0};
std::atomic<bool> glowEnabled{true};
std::atomic<int> strength{55};                    // overall glow strength, 0..100
std::atomic<int> micGain{10};                     // ES7210 gain index: 10 = 30 dB (0..14, 3 dB steps)
std::atomic<int> state{0};                        // 0 not found, 1 running, <0 error
std::atomic<int> levelDb{-120}, floorDb{-120};
std::atomic<uint32_t> beats{0};

bool writeReg(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(codecAddr); Wire.write(reg); Wire.write(value);
  return Wire.endTransmission() == 0;
}
int readReg(uint8_t reg) {
  Wire.beginTransmission(codecAddr); Wire.write(reg);
  if (Wire.endTransmission(false) != 0 || Wire.requestFrom(codecAddr, (uint8_t)1) != 1) return -1;
  return Wire.read();
}
bool updateReg(uint8_t reg, uint8_t mask, uint8_t data) {
  int v = readReg(reg); if (v < 0) return false;
  return writeReg(reg, uint8_t((v & ~mask) | (mask & data)));
}
bool probe(uint8_t address) {
  Wire.beginTransmission(address);
  if (Wire.endTransmission() != 0) return false;
  codecAddr = address; return true;
}
void applyGain(int gain) {
  gain = constrain(gain, 0, 14);
  for (uint8_t reg = 0x43; reg <= 0x44; reg++) updateReg(reg, 0x0F, uint8_t(gain));   // MIC1, MIC2 PGA
}
bool configureCodec() {
  bool ok = true;
  ok &= writeReg(0x00, 0xFF); ok &= writeReg(0x00, 0x41);          // reset
  ok &= writeReg(0x01, 0x3F);                                        // clocks off while configuring
  ok &= writeReg(0x09, 0x30); ok &= writeReg(0x0A, 0x30);            // state / power-up cycles
  ok &= writeReg(0x23, 0x2A); ok &= writeReg(0x22, 0x0A);            // ADC12 high-pass (removes DC)
  ok &= writeReg(0x20, 0x0A); ok &= writeReg(0x21, 0x2A);            // ADC34 high-pass
  ok &= updateReg(0x08, 0x01, 0x00);                                 // I2S slave (ESP drives MCLK/BCLK/WS)
  ok &= writeReg(0x40, 0x43);                                        // analog power, VMID
  ok &= writeReg(0x41, 0x70); ok &= writeReg(0x42, 0x70);            // mic bias 2.87 V
  ok &= writeReg(0x07, 0x20);                                        // OSR
  ok &= writeReg(0x02, 0xC1);                                        // main clock divider, clear state
  for (uint8_t reg = 0x43; reg <= 0x46; reg++) ok &= updateReg(reg, 0x10, 0x00);
  ok &= writeReg(0x4B, 0xFF); ok &= writeReg(0x4C, 0xFF);
  ok &= updateReg(0x01, 0x0B, 0x00);                                 // MIC1/2 clocks on
  ok &= writeReg(0x4B, 0x00);                                        // MIC1/2 bias, ADC, PGA on
  ok &= updateReg(0x43, 0x10, 0x10); ok &= updateReg(0x44, 0x10, 0x10);
  ok &= writeReg(0x12, 0x00);                                        // normal I2S (not TDM) for 2 mics
  int iface = readReg(0x11); if (iface < 0) return false;
  ok &= writeReg(0x11, uint8_t(((iface & 0x1F) | 0x60) & 0xFC));     // 16-bit, standard I2S
  int off = readReg(0x01); if (off < 0) return false;
  // Start (es7210_start): power up and release the ADCs.
  ok &= writeReg(0x01, uint8_t(off)); ok &= writeReg(0x06, 0x00); ok &= writeReg(0x40, 0x43);
  for (uint8_t reg = 0x47; reg <= 0x4A; reg++) ok &= writeReg(reg, 0x08);
  ok &= writeReg(0x4B, 0x00);
  ok &= writeReg(0x40, 0x43); ok &= writeReg(0x00, 0x71); ok &= writeReg(0x00, 0x41);
  applyGain(micGain);
  return ok;
}

void audioTask(void *) {
  auto *buffer = (int16_t *)ps_malloc(BLOCK_FRAMES * sizeof(int16_t));   // mono (MIC1)
  if (!buffer) { state = -3; vTaskDelete(nullptr); return; }
  float floorRms = 30, level = 0, pulse = 0, avgEnergy = 0, glowOut = 0;
  uint32_t lastBeat = 0;
  for (;;) {
    double sum = 0; size_t frames = 0;
    for (int part = 0; part < 2; part++) {                           // 2 x 8 ms = one 16 ms block
      size_t got = 0;
      if (i2s_channel_read(rx, buffer, BLOCK_FRAMES * sizeof(int16_t), &got, pdMS_TO_TICKS(200)) != ESP_OK) continue;
      size_t n = got / sizeof(int16_t);
      for (size_t i = 0; i < n; i++) { float v = buffer[i]; sum += double(v) * v; }
      frames += n;
    }
    if (!frames) continue;
    float rms = sqrtf(float(sum / double(frames))) + 1.0f;
    // Noise floor: follows quiet moments quickly, rises only very slowly (about a minute).
    if (rms < floorRms) floorRms += (rms - floorRms) * 0.05f;
    else floorRms += (rms - floorRms) * 0.0004f;
    floorRms = fmaxf(floorRms, 4.0f);
    float aboveDb = 20.0f * log10f(rms / floorRms);                  // loudness above the room
    float target = constrain((aboveDb - 6.0f) / 24.0f, 0.0f, 1.0f);   // 6 dB .. 30 dB -> 0 .. 1
    level += (target - level) * (target > level ? 0.2f : 0.035f);    // gentle attack, ~0.5 s release
    // Beat: short-term energy jumps well above its recent (~1 s) average. Kept subtle.
    float energy = rms * rms; avgEnergy += (energy - avgEnergy) * 0.016f;
    uint32_t now = millis();
    if (energy > avgEnergy * 2.2f && aboveDb > 10.0f && now - lastBeat > 300) {
      lastBeat = now; pulse = 1.0f; beats++;
    }
    pulse *= 0.93f;                                                  // ~0.2 s decay
    float wanted = glowEnabled ? fminf(1.0f, level * 0.6f + pulse * 0.35f) : 0.0f;
    glowOut += (wanted - glowOut) * 0.22f;                           // final smoothing: no flicker
    glowLevel = uint8_t(constrain(glowOut * strength.load() / 100.0f, 0.0f, 1.0f) * 255.0f);
    levelDb = int(20.0f * log10f(rms / 32768.0f)); floorDb = int(20.0f * log10f(floorRms / 32768.0f));
  }
}
}  // namespace

void audioBegin() {
  pinMode(11, OUTPUT); digitalWrite(11, LOW);      // keep the speaker amplifier off (no hiss)
  i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  chan.dma_desc_num = 3; chan.dma_frame_num = BLOCK_FRAMES;          // 768 B of internal DMA RAM
  if (i2s_new_channel(&chan, nullptr, &rx) != ESP_OK) { state = -1; return; }
  i2s_std_config_t cfg = {
    .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),              // MCLK = 256 x fs
    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
    .gpio_cfg = {.mclk = GPIO_NUM_12, .bclk = GPIO_NUM_43, .ws = GPIO_NUM_38,
                 .dout = I2S_GPIO_UNUSED, .din = GPIO_NUM_39,
                 .invert_flags = {.mclk_inv = false, .bclk_inv = false, .ws_inv = false}},
  };
  if (i2s_channel_init_std_mode(rx, &cfg) != ESP_OK || i2s_channel_enable(rx) != ESP_OK) { state = -2; return; }
  delay(10);                                       // let MCLK run before configuring the codec
  if (!probe(0x40) && !probe(0x41) && !probe(0x42) && !probe(0x43)) { Serial.println("MIC codec not found"); return; }
  if (!configureCodec()) { state = -4; Serial.println("MIC codec config failed"); return; }
  state = 1;
  // Stack in PSRAM: internal RAM is reserved for Wi-Fi, Bluetooth and TLS.
  xTaskCreatePinnedToCoreWithCaps(audioTask, "matrix-audio", 3072, nullptr, 2, nullptr, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  Serial.printf("MIC ready: ES7210 at 0x%02X\n", codecAddr);
}

uint8_t audioGlowLevel() { return glowLevel.load(); }

void audioConfigure(int gain, int glow, int strengthPercent) {
  if (glow >= 0) glowEnabled = glow != 0;
  if (strengthPercent >= 0) strength = constrain(strengthPercent, 0, 100);
  if (gain >= 0 && state.load() == 1) { micGain = constrain(gain, 0, 14); applyGain(micGain); }
}

void audioDiagnostics(char *out, size_t capacity) {
  snprintf(out, capacity, "mic=%d addr=0x%02X gain=%d strength=%d level_db=%d floor_db=%d glow=%u beats=%lu on=%d",
           state.load(), codecAddr, micGain.load(), strength.load(), levelDb.load(), floorDb.load(), glowLevel.load(),
           (unsigned long)beats.load(), bool(glowEnabled));
}
