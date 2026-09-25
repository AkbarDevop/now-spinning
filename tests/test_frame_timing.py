"""Exercise the actual firmware loop with a simulated display and MCU clock."""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
ARDUINO = r'''
#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cstdio>
#include <algorithm>
using std::min; using std::max;
#define PI 3.14159265358979323846
uint32_t fakeUs=100000;
uint32_t millis(){return fakeUs/1000;}
uint32_t micros(){return fakeUs;}
void delay(int){}
struct SerialMock {void setRxBufferSize(int){} void begin(int){} int available(){return 0;} int read(){return 0;} void println(const char*){} template<class... A> void printf(const char*,A...){} } Serial;
struct ESPMock {unsigned getFlashChipSize(){return 33554432;} unsigned getPsramSize(){return 16777216;}} ESP;
'''
PANEL = r'''
#pragma once
struct HUB75_I2S_CFG {struct{int e;}gpio; bool clkphase,double_buff; int driver; int min_refresh_rate=60; enum {FM6126A}; HUB75_I2S_CFG(int,int,int){} };
struct MatrixPanel_I2S_DMA {
 int calculated_refresh_rate=300, fills=0, flips=0; uint16_t pixels[4096]={};
 MatrixPanel_I2S_DMA(HUB75_I2S_CFG){} bool begin(){return true;}
 void setBrightness8(int){} void clearScreen(){} void setTextWrap(bool){} void setTextSize(int){}
 uint16_t color565(uint8_t r,uint8_t g,uint8_t b){return ((r>>3)<<11)|((g>>2)<<5)|(b>>3);}
 void drawPixel(int x,int y,uint16_t c){if(x>=0&&x<64&&y>=0&&y<64)pixels[y*64+x]=c;}
 void fillScreen(uint16_t c){++fills;std::fill(pixels,pixels+4096,c);} void flipDMABuffer(){++flips;}
};
'''
SCHEDULER = r'''
#include <cassert>
#include "sketch.cpp"
int main(){
 setup();
 // Even when a slow previous render makes the next frame overdue, the DMA
 // buffer must remain untouched throughout its asynchronous handover.
 const uint32_t hold=bufferHoldUs;
 assert(hold==7067);
 for(uint32_t age: {5u,914u,hold-1}){
   lastBufferSwapUs=100000; fakeUs=100000+age;
   lastFrame=millis()-40; panel->fills=panel->flips=0;
   loop(); assert(panel->fills==0 && panel->flips==0);
 }
 lastBufferSwapUs=100000;fakeUs=100000+hold;lastFrame=millis()-40;
 loop(); assert(panel->fills==1 && panel->flips==1);
 // micros() wraparound must not disable or bypass the fence.
 assert(!bufferCanBeReused(3,UINT32_MAX-2,hold));
 assert(bufferCanBeReused(hold-3,UINT32_MAX-2,hold));
}
'''
RENDER = r'''
#include "sketch.cpp"
int main(){
 setup(); haveArtwork=true;
 for(int i=0;i<4096;i++){artwork[i]=(i*37)&65535;previousArtwork[i]=(i*53)&65535;}
 uint32_t hash=2166136261u;
 for(int step=0;step<12;step++){
  angle=step*.31f;panel->fillScreen(0);drawRecord(step/11.0f,{120,80,190});
  for(uint16_t pixel:panel->pixels){hash^=pixel;hash*=16777619u;}
 }
 printf("%u\n",hash);
}
'''

PAUSE = r'''
#include <cassert>
#include "sketch.cpp"
int main(){
 setup(); haveArtwork=true; playing=false; speed=0; artworkCRC=123;
 fakeUs=2000000;updated=millis();lastFrame=0;lastBufferSwapUs=0;
 panel->fills=panel->flips=0;loop();assert(panel->fills==1);
 for(int i=0;i<100;i++){fakeUs+=40000;updated=millis();loop();}
 assert(panel->fills==1 && panel->flips==1);
 playing=true;fakeUs+=40000;updated=millis();loop();assert(panel->fills==2);
 playing=false;speed=0;fakeUs+=40000;updated=millis();loop();assert(panel->fills==3);
 artworkCRC=456;fakeUs+=40000;updated=millis();loop();assert(panel->fills==4);
}
'''

class FrameTimingTests(unittest.TestCase):
    def run_sketch(self, source, main):
        with tempfile.TemporaryDirectory() as d:
            d=pathlib.Path(d)
            (d/'Arduino.h').write_text(ARDUINO)
            (d/'ESP32-HUB75-MatrixPanel-I2S-DMA.h').write_text(PANEL)
            (d/'frame_timing.h').write_text((ROOT/'album_display/frame_timing.h').read_text())
            (d/'sketch.cpp').write_text(source)
            (d/'main.cpp').write_text(main)
            subprocess.run(['clang++','-std=c++17','-DMATRIX_NATIVE_TEST','-I',str(d),str(d/'main.cpp'),'-o',str(d/'test')],check=True,capture_output=True)
            return subprocess.run([str(d/'test')],capture_output=True,text=True)

    def test_actual_loop_protects_pending_dma_buffer(self):
        source=(ROOT/'album_display/album_display.ino').read_text()
        result=self.run_sketch(source,SCHEDULER)
        self.assertEqual(result.returncode,0,result.stderr)
        # Remove only the fix; the same scenario must reproduce early writes.
        mutant=source.replace(' || !bufferCanBeReused(micros(),lastBufferSwapUs,bufferHoldUs)','')
        self.assertNotEqual(source,mutant)
        result=self.run_sketch(mutant,SCHEDULER)
        self.assertNotEqual(result.returncode,0,'Regression was not reproduced')

    def test_paused_image_is_not_redrawn_but_resume_and_new_artwork_work(self):
        result=self.run_sketch((ROOT/'album_display/album_display.ino').read_text(),PAUSE)
        self.assertEqual(result.returncode,0,result.stderr)

    def test_cached_shading_preserves_pixels(self):
        # Golden checksum from the pre-optimization renderer: twelve angles and
        # crossfade fractions with the same deterministic artwork above.
        new=self.run_sketch((ROOT/'album_display/album_display.ino').read_text(),RENDER)
        self.assertEqual(new.returncode,0,new.stderr)
        self.assertEqual(new.stdout, '3542962808\n', 'The optimization changed the disc appearance')

if __name__=='__main__':unittest.main()
