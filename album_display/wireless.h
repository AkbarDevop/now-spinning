#pragma once
#include <Arduino.h>

// Network/Bluetooth workers never draw or touch DMA buffers. The render loop
// copies a completed mailbox frame between its normal receive and draw steps.
void wirelessBegin();
bool wirelessTakeFrame(uint8_t *pixels, bool &playing, uint32_t &crc);
bool wirelessMacAllowed(bool playing);
bool wirelessConfigure(const uint8_t *data, size_t length);
void wirelessStatus(char *output, size_t capacity);
bool wirelessOtaActive();
int wirelessDemoMode();   // ./matrix demo: 0 off, 1 clock, 2 dark, 3 dim
bool wirelessCommand(uint8_t command); // routes to the Mac (via helper) or the iPhone
bool phoneCommand(uint8_t command);   // AMS remote command (3 = next, 2 = play/pause)
void gestureBegin();
void gestureDiagnostics(char *output, size_t capacity);
int wirelessOtaProgress();
void phoneBegin();
void phoneTick();
void phoneDiagnostics(char *output, size_t capacity);
void wirelessPhoneState(bool connected, bool playing, const char *title,
                        const char *artist, const char *album);
