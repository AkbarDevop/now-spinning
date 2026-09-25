#pragma once
#include <stdint.h>
struct SourcePolicy {
 enum Source {NONE,MAC,PHONE};
 Source owner=NONE;
 bool macPlaying=false;
 uint32_t macSeen=0;
 bool acceptMac(bool playing,bool phoneActive,uint32_t now){
  macPlaying=playing;macSeen=now;
  if(phoneActive||(!playing&&owner==PHONE))return false;
  owner=MAC;return true;
 }
 bool acceptPhone(bool playing,uint32_t now){
  bool activeMac=macPlaying&&uint32_t(now-macSeen)<15000;
  if(!playing&&(activeMac||owner==MAC))return false;
  owner=PHONE;return true;
 }
};
