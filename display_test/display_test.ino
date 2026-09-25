#include <Arduino.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <math.h>

// Waveshare ESP32-S3-RGB-Matrix, one P3 64x64 panel, direct HUB75 connection.
// Uses the board-specific pin mapping in the pinned Waveshare driver.
MatrixPanel_I2S_DMA *panel = nullptr;
constexpr uint8_t BRIGHTNESS = 40;  // 16% for initial power and wiring validation.
uint32_t lastReport = 0;

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.printf("MATRIX_TEST starting; flash=%u PSRAM=%u\n",
                ESP.getFlashChipSize(), ESP.getPsramSize());
  HUB75_I2S_CFG cfg(64, 64, 1);
  cfg.gpio.e = 9;
  cfg.clkphase = false;
  cfg.driver = HUB75_I2S_CFG::FM6126A;  // Vendor's P3 example configuration.
  cfg.double_buff = true;
  panel = new MatrixPanel_I2S_DMA(cfg);
  if (!panel->begin()) {
    Serial.println("MATRIX_TEST ERROR: display buffer allocation failed");
    while (true) delay(1000);
  }
  panel->setBrightness8(BRIGHTNESS);
  panel->clearScreen();
  panel->fillRect(0, 0, 32, 32, panel->color565(255, 0, 0));
  panel->fillRect(32, 0, 32, 32, panel->color565(0, 255, 0));
  panel->fillRect(0, 32, 32, 32, panel->color565(0, 0, 255));
  panel->fillRect(32, 32, 32, 32, panel->color565(255, 255, 255));
  panel->flipDMABuffer();
  Serial.println("MATRIX_TEST quadrants: top red/green; bottom blue/white");
  delay(3000);
  Serial.println("MATRIX_TEST READY: spinning record demo (no music connection yet)");
}

void loop() {
  // Synthetic record artwork. This animation is independent of the music service.
  const float angle = millis() * 0.0015f;
  panel->fillScreen(panel->color565(8, 14, 24));
  panel->fillCircle(31, 29, 26, panel->color565(1, 2, 3));
  for (int r = 13; r <= 25; r += 4)
    panel->drawCircle(31, 29, r, panel->color565(34, 38, 44));
  for (int k = 0; k < 2; ++k) {
    const float a = angle + k * PI;
    panel->drawLine(31 + cosf(a) * 13, 29 + sinf(a) * 13,
                    31 + cosf(a) * 24, 29 + sinf(a) * 24,
                    panel->color565(110, 122, 140));
  }
  panel->fillCircle(31, 29, 10, panel->color565(245, 90, 30));
  panel->fillCircle(31 + cosf(angle) * 6, 29 + sinf(angle) * 6, 2,
                    panel->color565(255, 220, 120));
  panel->fillCircle(31, 29, 2, panel->color565(1, 2, 3));
  panel->setTextWrap(false);
  panel->setTextSize(1);
  panel->setTextColor(panel->color565(150, 195, 220));
  panel->setCursor(17, 56);
  panel->print("AKBAR");
  panel->flipDMABuffer();
  if (millis() - lastReport >= 5000) {
    lastReport = millis();
    Serial.printf("MATRIX_TEST OK uptime=%lus heap=%u brightness=%u/255\n",
                  millis()/1000, ESP.getFreeHeap(), BRIGHTNESS);
  }
  delay(50);
}
