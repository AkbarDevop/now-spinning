#include <Arduino.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <math.h>
#include "frame_timing.h"
#ifndef MATRIX_NATIVE_TEST
#include "wireless.h"
#endif

MatrixPanel_I2S_DMA *panel;
constexpr uint8_t BRIGHTNESS = 40;
constexpr uint8_t NIGHT_BRIGHTNESS = 16;
constexpr int NIGHT_START_HOUR = 23, NIGHT_END_HOUR = 7;   // local (Central) time
constexpr uint32_t IDLE_CLOCK_MS = 5UL * 60 * 1000;          // clock after 5 min without music
constexpr size_t IMAGE_BYTES = 64 * 64 * 2;
constexpr float CX = 31.5f, CY = 31.5f, ART_RADIUS = 29.25f;
constexpr float DISC_RADIUS = 30.5f, HOLE_RADIUS = 3.5f;
constexpr float ROTATION_RPM = 4.0f;
uint8_t incoming[IMAGE_BYTES], header[12];
uint16_t artwork[4096], previousArtwork[4096];
size_t magicPos = 0, headerPos = 0, bodyPos = 0;
enum ReceiveState { MAGIC, HEADER, BODY } rx = MAGIC;
uint32_t rxTime = 0, updated = 0, lastFrame = 0, sequence = 0, expectedCRC = 0;
uint32_t artworkCRC = 0, transitionStart = 0, titleStart = 0;
uint16_t payloadSize = 0, payloadFlags = 0;
bool haveArtwork = false, playing = false;
float angle = 0, speed = 0;
uint32_t lastBufferSwapUs=0, bufferHoldUs=0;
uint32_t profileMinGap=UINT32_MAX, profileMaxDraw=0, profileReport=0, profileFrames=0;
bool stationaryFrameDrawn=false;
uint32_t renderedArtworkCRC=0;
char songTitle[61] = "";
uint32_t lastPlayingAt = 0; uint8_t currentBrightness = 0; int idleShown = -1;

struct RGB { float r, g, b; };
struct DiscGeometry { float radius, light, shade, reflection, edgeGain; };
#ifdef MATRIX_NATIVE_TEST
DiscGeometry discGeometry[4096];
#else
DiscGeometry *discGeometry=nullptr;
#endif
void prepareDiscGeometry() {
  for(int y=0;y<64;y++) for(int x=0;x<64;x++) {
    float dx=x-CX, dy=y-CY, r=sqrtf(dx*dx+dy*dy);
    float light=fmaxf(0,(-0.8f*dx-0.6f*dy)/fmaxf(r,1));
    float groove=0.5f+0.5f*cosf(r*2*PI/3.4f);
    discGeometry[y*64+x]={r,light,(r>7)?1-0.065f*groove:1,
      0.028f*powf(light,8),fminf(1,(DISC_RADIUS-r)*2)*(0.28f+0.5f*powf(light,4))};
  }
}
RGB accent = {80, 160, 190}, previousAccent = accent;
RGB unpack(uint16_t p) { return {float((p >> 11) * 255 / 31), float(((p >> 5) & 63) * 255 / 63), float((p & 31) * 255 / 31)}; }
RGB mix(RGB a, RGB b, float t) { return {a.r+(b.r-a.r)*t, a.g+(b.g-a.g)*t, a.b+(b.b-a.b)*t}; }
uint16_t color(RGB a, float gain=1) { return panel->color565(uint8_t(a.r*gain), uint8_t(a.g*gain), uint8_t(a.b*gain)); }
RGB coverAccent() {
  // Favor colored midtones rather than black backgrounds or white text.
  RGB sum = {0,0,0}; float total = 0;
  for (int y=4; y<60; y+=3) for (int x=4; x<60; x+=3) {
    RGB p=unpack(artwork[y*64+x]);
    float hi=fmaxf(p.r,fmaxf(p.g,p.b)), lo=fminf(p.r,fminf(p.g,p.b));
    float weight=(hi-lo)+4;
    if (hi<35 || lo>220) continue;
    sum.r+=p.r*weight; sum.g+=p.g*weight; sum.b+=p.b*weight; total+=weight;
  }
  if(total==0) return {100,155,185};
  sum.r/=total; sum.g/=total; sum.b/=total;
  float peak=fmaxf(sum.r,fmaxf(sum.g,sum.b));
  float gain=190/fmaxf(peak,1);
  return {sum.r*gain,sum.g*gain,sum.b*gain};
}
uint32_t read32(const uint8_t *p) {
  return uint32_t(p[0]) | (uint32_t(p[1])<<8) | (uint32_t(p[2])<<16) | (uint32_t(p[3])<<24);
}
uint32_t crc32(const uint8_t *p, size_t n) {
  uint32_t c = 0xFFFFFFFF;
  while (n--) { c ^= *p++; for (int b=0;b<8;b++) c=(c>>1)^((c&1)?0xEDB88320:0); }
  return ~c;
}
void resetReceiver() { rx=MAGIC; magicPos=headerPos=bodyPos=0; }
void applyImage(const uint8_t *pixels,bool isPlaying,uint32_t crc);
void acceptPayload() {
#ifndef MATRIX_NATIVE_TEST
  if(payloadFlags==3) {
    Serial.printf(wirelessConfigure(incoming,payloadSize)?"ACK %lu WIFI_SAVED\n":"ERR wifi\n",sequence);return;
  }
  if(payloadFlags==4) {
    char status[1536];wirelessStatus(status,sizeof(status));Serial.printf("ACK %lu STATUS %s\n",sequence,status);return;
  }
  if(payloadFlags<=1 && !wirelessMacAllowed(payloadFlags==1)) {
    Serial.printf("ACK %lu PHONE_ACTIVE\n",sequence);return;
  }
#endif
  if(payloadFlags==2) {
    for(size_t i=0; i<payloadSize; i++) if(incoming[i]<32 || incoming[i]>126) {
      Serial.println("ERR title"); return;
    }
    incoming[payloadSize]=0;
    if(strcmp(songTitle, reinterpret_cast<char*>(incoming))!=0) {
      memcpy(songTitle,incoming,payloadSize+1); titleStart=millis();
    }
    Serial.printf("ACK %lu TITLE\n",sequence); return;
  }
  applyImage(incoming,payloadFlags==1,expectedCRC);
  Serial.printf("ACK %lu %s\n",sequence,playing?"PLAY":"PAUSE");
}
void applyImage(const uint8_t *pixels,bool isPlaying,uint32_t crc) {
  if(!haveArtwork || crc!=artworkCRC) {
    bool hadArtwork=haveArtwork;
    memcpy(previousArtwork,artwork,sizeof(artwork)); previousAccent=accent;
    for(size_t i=0;i<4096;i++) artwork[i]=pixels[i*2] | (uint16_t(pixels[i*2+1])<<8);
    accent=coverAccent(); transitionStart=millis(); artworkCRC=crc;
    if(!hadArtwork) { memcpy(previousArtwork,artwork,sizeof(artwork)); previousAccent=accent; }
  }
  haveArtwork=true; playing=isPlaying; updated=millis();
}
void receiveFrames() {
  if (rx!=MAGIC && millis()-rxTime>2000) { resetReceiver(); Serial.println("ERR timeout"); }
  for (size_t budget=0; budget<2048 && Serial.available(); ++budget) {
    uint8_t b=Serial.read(); rxTime=millis();
    if(rx==MAGIC) {
      const char *magic="MXA1";
      if(b==magic[magicPos]) ++magicPos; else magicPos=b=='M'?1:0;
      if(magicPos==4) { rx=HEADER; headerPos=0; }
    } else if(rx==HEADER) {
      header[headerPos++]=b;
      if(headerPos==12) {
        sequence=read32(header); payloadSize=header[4] | (uint16_t(header[5])<<8);
        payloadFlags=header[6] | (uint16_t(header[7])<<8); expectedCRC=read32(header+8);
        bool valid=(payloadFlags<=1 && payloadSize==IMAGE_BYTES) ||
                   (payloadFlags==2 && payloadSize>=1 && payloadSize<=60) ||
                   (payloadFlags==3 && payloadSize>=3 && payloadSize<=98) ||
                   (payloadFlags==4 && payloadSize==1);
        if(!valid) { resetReceiver(); Serial.println("ERR header"); }
        else { rx=BODY; bodyPos=0; }
      }
    } else {
      incoming[bodyPos++]=b;
      if(bodyPos==payloadSize) {
        if(crc32(incoming,payloadSize)==expectedCRC) acceptPayload(); else Serial.println("ERR crc");
        resetReceiver();
      }
    }
  }
}
RGB sample(const uint16_t *image, float x, float y) {
  x=fmaxf(0,fminf(x,63)); y=fmaxf(0,fminf(y,63));
  int x0=int(x), y0=int(y), x1=x0<63?x0+1:x0, y1=y0<63?y0+1:y0;
  return mix(mix(unpack(image[y0*64+x0]),unpack(image[y0*64+x1]),x-x0),
             mix(unpack(image[y1*64+x0]),unpack(image[y1*64+x1]),x-x0),y-y0);
}
// Picture disc: artwork printed across the record, with a real dark hole.
void drawRecord(float fade, RGB tint) {
  float c=cosf(angle), s=sinf(angle);
  RGB edgeTint=mix({100,110,120},tint,0.18f);
  for(int y=0;y<64;y++) for(int x=0;x<64;x++) {
    const DiscGeometry &g=discGeometry[y*64+x];
    float dx=x-CX, dy=y-CY, r=g.radius;
    if(r>DISC_RADIUS || r<HOLE_RADIUS) continue;
    float light=g.light;
    RGB p={7,10,14};
    if(r<=ART_RADIUS) {
      if(haveArtwork) {
        float sx=(c*dx+s*dy)*(31/ART_RADIUS)+31.5f;
        float sy=(-s*dx+c*dy)*(31/ART_RADIUS)+31.5f;
        p=sample(artwork,sx,sy);
        if(fade<1) p=mix(sample(previousArtwork,sx,sy),p,fade);
      }
      // Very faint concentric grooves and a fixed-light surface reflection.
      float shade=g.shade;
      p={p.r*shade,p.g*shade,p.b*shade};
      p=mix(p,{205,215,225},g.reflection);
      if(r<5.1f) {
        // A narrow pressed center ring around the black opening.
        float ring=fmaxf(0,1-fabsf(r-4.25f)/0.85f);
        p=mix(p,mix({40,45,50},edgeTint,light*0.4f),ring*0.8f);
      }
      p=mix({0,0,0},p,fminf(1,(r-HOLE_RADIUS)*2));
    } else {
      // Thin material edge, with a brighter upper-left bevel rather than a halo.
      float gain=g.edgeGain;
      p={edgeTint.r*gain,edgeTint.g*gain,edgeTint.b*gain};
    }
    panel->drawPixel(x,y,color(p));
  }
}
void setup() {
  Serial.setRxBufferSize(16384); Serial.begin(115200); delay(700);
#ifndef MATRIX_NATIVE_TEST
  // Never let USB logging stall rendering when the Mac is unplugged or not reading.
  Serial.setTxTimeoutMs(10);
#endif
#ifndef MATRIX_NATIVE_TEST
  discGeometry=(DiscGeometry*)ps_malloc(4096*sizeof(DiscGeometry));
  if(!discGeometry){Serial.println("ERR geometry memory");while(true)delay(1000);}
#endif
  prepareDiscGeometry();
  HUB75_I2S_CFG cfg(64,64,1);
  cfg.min_refresh_rate=240; // Request faster scanning without changing the pixel clock.
  cfg.gpio.e=9; cfg.clkphase=false; cfg.driver=HUB75_I2S_CFG::FM6126A; cfg.double_buff=true;
  panel=new MatrixPanel_I2S_DMA(cfg);
  if(!panel->begin()) { Serial.println("ERR display"); while(true) delay(1000); }
  bufferHoldUs=bufferReuseDelayUs(panel->calculated_refresh_rate);
  lastBufferSwapUs=micros();
  panel->setBrightness8(BRIGHTNESS); currentBrightness=BRIGHTNESS; panel->clearScreen(); panel->setTextWrap(false); panel->setTextSize(1);
#ifndef MATRIX_NATIVE_TEST
  wirelessBegin();
  // Reboot automatically if the render loop ever hangs (default 5 s task watchdog).
  enableLoopWDT();
#endif
  Serial.printf("MATRIX_PICTURE_DISC_REFRESH READY flash=%u psram=%u\n",ESP.getFlashChipSize(),ESP.getPsramSize());
}
void reportPerformance(uint32_t now) {
  if(now-profileReport<2000) return;
  Serial.printf("PERF frames=%lu max_draw_us=%lu min_reuse_gap_us=%lu scan_hz=%d hold_us=%lu\n",
    profileFrames,profileMaxDraw,profileFrames?profileMinGap:0,panel->calculated_refresh_rate,bufferHoldUs);
  profileReport=now; profileMaxDraw=0; profileMinGap=UINT32_MAX; profileFrames=0;
}
#ifndef MATRIX_NATIVE_TEST
void applyBrightness(uint8_t b) { if(b!=currentBrightness) { panel->setBrightness8(b); currentBrightness=b; } }
// Idle screen: a quiet clock by day, fully dark at night. Redrawn once per minute.
void drawIdle(bool night, const struct tm &local) {
  int key = night ? -2 : local.tm_hour*60 + local.tm_min;
  if(key==idleShown || !bufferCanBeReused(micros(),lastBufferSwapUs,bufferHoldUs)) return;
  panel->fillScreen(0);
  if(!night) {
    static const char *days[]={"SUN","MON","TUE","WED","THU","FRI","SAT"};
    char hm[6], date[12]; int h12=local.tm_hour%12;
    snprintf(hm,sizeof(hm),"%d:%02d",h12?h12:12,local.tm_min);
    snprintf(date,sizeof(date),"%s %d",days[local.tm_wday%7],local.tm_mday);
    int16_t x1,y1; uint16_t w,h;
    panel->setTextSize(2); panel->getTextBounds(hm,0,0,&x1,&y1,&w,&h);
    panel->setTextColor(color(accent,0.8f)); panel->setCursor((64-int(w))/2,21); panel->print(hm);
    panel->setTextSize(1); panel->getTextBounds(date,0,0,&x1,&y1,&w,&h);
    panel->setTextColor(color(accent,0.4f)); panel->setCursor((64-int(w))/2,42); panel->print(date);
  }
  panel->flipDMABuffer(); lastBufferSwapUs=micros(); idleShown=key; stationaryFrameDrawn=false;
}
// Minimal progress bar while a Wi-Fi firmware update is being written.
void drawUpdateProgress() {
  static uint32_t last=0; static int shown=-1;
  int pct=wirelessOtaProgress();
  if(pct==shown && millis()-last<1000) return;
  if(!bufferCanBeReused(micros(),lastBufferSwapUs,bufferHoldUs)) return;
  shown=pct; last=millis();
  panel->fillScreen(0);
  panel->drawRect(3,29,58,6,panel->color565(40,45,50));
  int w=(pct*56)/100; if(w>0) panel->fillRect(4,30,w,4,panel->color565(60,150,190));
  panel->flipDMABuffer(); lastBufferSwapUs=micros(); stationaryFrameDrawn=false;
}
#endif
void loop() {
  receiveFrames();
#ifndef MATRIX_NATIVE_TEST
  if(wirelessOtaActive()) { drawUpdateProgress(); delay(20); return; }
  // Never overwrite a USB packet that is still being received.
  if(rx==MAGIC) {bool play;uint32_t crc;if(wirelessTakeFrame(incoming,play,crc))applyImage(incoming,play,crc);}
  {
    uint32_t t=millis();
    if(haveArtwork && playing && t-updated<15000) lastPlayingAt=t;
    time_t epoch=time(nullptr); struct tm local={};
    bool clockValid=epoch>1700000000 && localtime_r(&epoch,&local);
    int demo=wirelessDemoMode();   // ./matrix demo clock|dark|dim previews these screens
    bool night=(clockValid && (local.tm_hour>=NIGHT_START_HOUR || local.tm_hour<NIGHT_END_HOUR)) || demo==2 || demo==3;
    if(demo==1) night=false;
    applyBrightness(night?NIGHT_BRIGHTNESS:BRIGHTNESS);
    bool idle=(t-lastPlayingAt>IDLE_CLOCK_MS || demo==1 || demo==2) && demo!=3;
    if(clockValid && idle) { drawIdle(night,local); delay(20); return; }
    idleShown=-1;
  }
#endif
  uint32_t now=millis(); reportPerformance(now);
  if(now-lastFrame<33 || !bufferCanBeReused(micros(),lastBufferSwapUs,bufferHoldUs)) { delay(1); return; }
  float dt=fminf((now-lastFrame)*0.001f,0.1f); lastFrame=now;
  bool live=haveArtwork && now-updated<15000;
  float target=(!haveArtwork || (live && playing))?(ROTATION_RPM*2*PI/60):0;
  speed+=(target-speed)*(1-expf(-dt/0.45f));
  if(target==0 && speed<0.003f) speed=0;
  angle=fmodf(angle+speed*dt,2*PI);
  float fade=fminf(1,(now-transitionStart)/900.0f); fade=fade*fade*(3-2*fade);
  bool stationary=haveArtwork && speed==0 && fade>=1;
  if(stationary && stationaryFrameDrawn && renderedArtworkCRC==artworkCRC) return;
  RGB tint=mix(previousAccent,accent,fade);
  uint32_t drawStart=micros();
  profileMinGap=min(profileMinGap,drawStart-lastBufferSwapUs);
  panel->fillScreen(0); drawRecord(fade,tint); panel->flipDMABuffer();
  lastBufferSwapUs=micros(); profileMaxDraw=max(profileMaxDraw,lastBufferSwapUs-drawStart);
  stationaryFrameDrawn=stationary; renderedArtworkCRC=artworkCRC; ++profileFrames;
}
