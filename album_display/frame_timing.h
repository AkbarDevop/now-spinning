#pragma once
#include <stdint.h>

// The bundled S3 driver returns from a flip before DMA leaves the old buffer.
// Reserve two full scans for a prefetched link, plus peripheral pipeline margin.
// Its actual 10 MHz clock is faster than the configured 8 MHz used to calculate
// scan Hz, so this bound is conservative for this board/driver configuration.
inline uint32_t bufferReuseDelayUs(int scanHz) {
  if(scanHz<1) scanHz=1;
  return (2000000u+uint32_t(scanHz)-1)/uint32_t(scanHz)+400u;
}
inline bool bufferCanBeReused(uint32_t now, uint32_t lastSwap, uint32_t holdUs) {
  return uint32_t(now-lastSwap)>=holdUs;
}
