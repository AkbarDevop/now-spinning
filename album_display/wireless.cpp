#include "wireless.h"
#include "source_policy.h"
#include "secrets.h"
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <TJpg_Decoder.h>
#include <esp_http_client.h>
#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <atomic>
#include <mbedtls/base64.h>
#include <mbedtls/platform.h>
#include <ArduinoOTA.h>
#include <esp_system.h>
#if __has_include("build_info.h")
#include "build_info.h"   // written by ./matrix build (git revision + time)
#endif
#ifndef MATRIX_BUILD
#define MATRIX_BUILD "dev"
#endif
// POSIX time zone for the clock and night mode (US Central). Change for your location.
#ifndef MATRIX_TZ
#define MATRIX_TZ "CST6CDT,M3.2.0,M11.1.0"
#endif

namespace {
constexpr size_t N=8192;
SemaphoreHandle_t guard;
uint8_t *mailbox=nullptr;
uint8_t *networkPixels=nullptr;
bool pending=false, pendingPlay=false;
uint32_t pendingCRC=0;
bool phoneConnected=false, phonePlaying=false;
SourcePolicy sources;
uint32_t phoneRevision=0;
char track[241]={}, artist[241]={}, album[241]={};
char ssid[33]={}, password[65]={};
bool configurePending=false;
std::atomic<bool> wifiOnline{false};
std::atomic<bool> otaActive{false};
std::atomic<int> demoMode{0};std::atomic<uint32_t> demoUntil{0};  // temporary preview of idle/night screens
std::atomic<int> macCommand{-1};std::atomic<uint32_t> macCommandAt{0};  // picked up by the next Mac /frame
std::atomic<int> otaProgress{0};
uint32_t bootCount=0;
const char *resetReasonName(){
 switch(esp_reset_reason()){
  case ESP_RST_POWERON:return "power-on";case ESP_RST_SW:return "software";case ESP_RST_PANIC:return "panic";
  case ESP_RST_INT_WDT:return "interrupt-watchdog";case ESP_RST_TASK_WDT:return "task-watchdog";case ESP_RST_WDT:return "watchdog";
  case ESP_RST_BROWNOUT:return "brownout";case ESP_RST_EXT:return "external";default:return "other";
 }
}
// Password-protected Wi-Fi firmware updates (espota / Arduino IDE network port).
// The render loop shows a progress bar and artwork lookups pause while active.
void startOta(){
 ArduinoOTA.setHostname("akbar-matrix");ArduinoOTA.setMdnsEnabled(false);ArduinoOTA.setPassword(MATRIX_TOKEN);
 ArduinoOTA.onStart([]{otaProgress=0;otaActive=true;Serial.println("OTA start");});
 ArduinoOTA.onProgress([](unsigned int done,unsigned int total){if(total)otaProgress=int(uint64_t(done)*100/total);});
 ArduinoOTA.onEnd([]{otaProgress=100;Serial.println("OTA done");});
 ArduinoOTA.onError([](ota_error_t e){otaActive=false;Serial.printf("OTA error %d\n",int(e));});
 ArduinoOTA.begin();
}
char ip[20]="", wifiMessage[64]="Wi-Fi not configured", artMessage[64]="Waiting for iPhone";
WebServer server(18766);
uint32_t checksum(const uint8_t *p,size_t n) {
 uint32_t c=0xffffffff;while(n--){c^=*p++;for(int b=0;b<8;b++)c=(c>>1)^((c&1)?0xedb88320:0);}return ~c;
}
void lock(){xSemaphoreTake(guard,portMAX_DELAY);} void unlock(){xSemaphoreGive(guard);}
bool allowMac(bool play){
 return sources.acceptMac(play,phoneConnected&&phonePlaying,millis());
}
void publish(const uint8_t *p,bool play,bool fromPhone){
 lock();
 bool allowed=fromPhone ? (phoneConnected && sources.acceptPhone(phonePlaying,millis())) : allowMac(play);
 if(allowed){memcpy(mailbox,p,N);pendingPlay=play;pendingCRC=checksum(p,N);pending=true;}
 unlock();
}
bool authorized(){return server.header("X-Matrix-Token")==MATRIX_TOKEN;}
void networkTask(void*){
 Preferences prefs;prefs.begin("matrix-wifi",false);
 String saved=prefs.getString("ssid","");String pass=prefs.getString("pass","");
 WiFi.mode(WIFI_STA);WiFi.setHostname("akbar-matrix");WiFi.setAutoReconnect(true);
 if(saved.length())WiFi.begin(saved.c_str(),pass.c_str());
 const char *headers[]={"X-Matrix-Token"};server.collectHeaders(headers,1);
 server.on("/status",HTTP_GET,[]{
  if(!authorized()){server.send(403,"text/plain","Not paired");return;}
  char status[1536];wirelessStatus(status,sizeof(status));server.send(200,"application/json",status);
 });
 server.on("/audio",HTTP_GET,[]{
  if(!authorized()){server.send(403,"text/plain","Not paired");return;}
  int gain=server.hasArg("gain")?server.arg("gain").toInt():-1;
  int glow=server.hasArg("glow")?int(server.arg("glow")=="on"||server.arg("glow")=="1"):-1;
  int strength=server.hasArg("strength")?server.arg("strength").toInt():-1;
  audioConfigure(gain,glow,strength);
  char d[160];audioDiagnostics(d,sizeof(d));JsonDocument r;r["audio"]=d;String out;serializeJson(r,out);
  server.send(200,"application/json",out);
 });
 server.on("/demo",HTTP_GET,[]{
  if(!authorized()){server.send(403,"text/plain","Not paired");return;}
  String m=server.arg("mode");int s=server.hasArg("seconds")?constrain(server.arg("seconds").toInt(),5,600):60;
  int mode=m=="clock"?1:m=="dark"?2:m=="dim"?3:0;
  demoUntil=millis()+uint32_t(s)*1000;demoMode=mode;
  char reply[80];snprintf(reply,sizeof(reply),"{\"demo\":\"%s\",\"seconds\":%d}",mode?m.c_str():"off",mode?s:0);
  server.send(200,"application/json",reply);
 });
 server.on("/frame",HTTP_POST,[]{
  if(!authorized()){server.send(403,"text/plain","Not paired");return;}
  const String &body=server.arg("plain");
  // Arduino WebServer constructs its plain body as a C string, so raw RGB565
  // (which contains zero bytes) must be encoded before HTTP transport.
  if(body.length()>16000){server.send(413,"text/plain","Frame too large");return;}
  JsonDocument doc;size_t decodedLength=0;
  if(deserializeJson(doc,body)||!doc["playing"].is<bool>()||!doc["pixels"].is<const char*>()){
   server.send(400,"text/plain","Invalid frame");return;
  }
  const char *encoded=doc["pixels"];
  if(mbedtls_base64_decode(networkPixels,N,&decodedLength,(const unsigned char*)encoded,strlen(encoded))||decodedLength!=N){
   server.send(400,"text/plain","Invalid pixels");return;
  }
  publish(networkPixels,doc["playing"].as<bool>(),false);
  int cmd=macCommand.exchange(-1);
  const char *name=cmd==3?"next":cmd==2?"toggle":cmd==4?"previous":nullptr;
  if(name&&millis()-macCommandAt.load()<10000){char reply[64];snprintf(reply,sizeof(reply),"{\"accepted\":true,\"command\":\"%s\"}",name);server.send(200,"application/json",reply);}
  else server.send(200,"application/json","{\"accepted\":true}");
 });
 server.begin();bool wasOnline=false,otaReady=false;uint32_t retryAt=millis();
 for(;;){
  char newSSID[33]={},newPass[65]={};bool change=false;
  lock();if(configurePending){strlcpy(newSSID,ssid,sizeof(newSSID));strlcpy(newPass,password,sizeof(newPass));memset(password,0,sizeof(password));configurePending=false;change=true;}unlock();
  if(change){saved=newSSID;pass=newPass;prefs.putString("ssid",saved);prefs.putString("pass",pass);WiFi.disconnect();WiFi.begin(newSSID,newPass);memset(newPass,0,sizeof(newPass));retryAt=millis();}
  bool online=WiFi.status()==WL_CONNECTED;wifiOnline=online;
  if(online&&!wasOnline){MDNS.end();MDNS.begin("akbar-matrix");MDNS.addService("matrix","tcp",18766);MDNS.addService("arduino","tcp",3232);configTzTime(MATRIX_TZ,"time.apple.com","pool.ntp.org");
   if(!otaReady){startOta();otaReady=true;}}
  lock();strlcpy(ip,online?WiFi.localIP().toString().c_str():"",sizeof(ip));
  strlcpy(wifiMessage,online?"Connected":(saved.length()?"Connecting (2.4 GHz required)":"Wi-Fi not configured"),sizeof(wifiMessage));unlock();
  if(!online&&saved.length()&&millis()-retryAt>30000){WiFi.reconnect();retryAt=millis();}
  wasOnline=online;if(otaReady)ArduinoOTA.handle();server.handleClient();phoneTick();vTaskDelay(pdMS_TO_TICKS(5));
 }
}
// Arduino's prebuilt TLS library defaults to internal-only allocations. Its
// two 16 KB record buffers exceed the internal heap left by HUB75 + BLE + Wi-Fi.
// Install before either radio starts; heap_caps_free handles both memory types.
void *tlsCalloc(size_t count,size_t size){return heap_caps_calloc(count,size,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);}
struct Download {uint8_t *data;size_t size,capacity;bool overflow;char error[64];};
esp_err_t httpEvent(esp_http_client_event_t *event){
 if(event->event_id==HTTP_EVENT_ON_DATA){auto *d=static_cast<Download*>(event->user_data);
  if(event->data_len<0 || d->size+size_t(event->data_len)>d->capacity){d->overflow=true;return ESP_FAIL;}
  memcpy(d->data+d->size,event->data,event->data_len);d->size+=event->data_len;
 }return ESP_OK;
}
bool download(const String &url,Download &out){
 out.size=0;out.overflow=false;out.error[0]=0;
 esp_http_client_config_t cfg={};cfg.url=url.c_str();cfg.timeout_ms=7000;
 cfg.crt_bundle_attach=esp_crt_bundle_attach;cfg.event_handler=httpEvent;cfg.user_data=&out;
 cfg.disable_auto_redirect=true;cfg.buffer_size=1024;
 auto client=esp_http_client_init(&cfg);if(!client){strlcpy(out.error,"HTTP client allocation failed",sizeof(out.error));return false;}
 esp_err_t result=esp_http_client_perform(client);int status=esp_http_client_get_status_code(client);
 int tlsError=0,tlsFlags=0;esp_http_client_get_and_clear_last_tls_error(client,&tlsError,&tlsFlags);
 if(out.overflow)strlcpy(out.error,"Response exceeds artwork buffer",sizeof(out.error));
 else if(result!=ESP_OK)snprintf(out.error,sizeof(out.error),"Download %s TLS:%x",esp_err_to_name(result),unsigned(tlsError));
 else if(status!=200)snprintf(out.error,sizeof(out.error),"Download HTTP %d",status);
 esp_http_client_cleanup(client);return out.error[0]==0;
}
String encoded(const char *input){
 String out;const char hex[]="0123456789ABCDEF";
 for(const unsigned char *p=(const unsigned char*)input;*p;p++){
  unsigned char c=*p;if(isalnum(c)||c=='-'||c=='_'||c=='.'||c=='~')out+=char(c);
  else{out+='%';out+=hex[c>>4];out+=hex[c&15];}
 }return out;
}
String normalized(const char *input){String s;for(const unsigned char *p=(const unsigned char*)input;*p;p++)if(*p>=128||isalnum(*p))s+=char(tolower(*p));return s;}
uint16_t *decodeTarget=nullptr;
bool decoded(int16_t x,int16_t y,uint16_t w,uint16_t h,uint16_t *pixels){
 if(x<0||y<0||x+w>64||y+h>64)return false;
 for(uint16_t row=0;row<h;row++)memcpy(decodeTarget+(y+row)*64+x,pixels+row*w,w*2);return true;
}
void fallback(uint16_t *pixels){
 for(int y=0;y<64;y++)for(int x=0;x<64;x++){
  float r=hypotf(x-31.5f,y-31.5f);uint8_t c=uint8_t(18+6*(1+cosf(r*2)));
  uint8_t red=c,green=c+8,blue=c+12;if(r<11){red=32;green=112;blue=134;}
  pixels[y*64+x]=((red>>3)<<11)|((green>>2)<<5)|(blue>>3);
 }
}

// ---- Cover lookup helpers -------------------------------------------------
String primaryArtist(const char *artist){
 String s=artist;int cut=s.length();
 for(const char *sep:{","," & "," feat"," Feat"," ft."," x "," X "," and "," va "}){int i=s.indexOf(sep);if(i>0&&i<cut)cut=i;}
 s=s.substring(0,cut);s.trim();return s;
}
String baseTitle(const char *title){
 String s=title;int cut=s.length();
 for(const char *sep:{" (","("," [","["," - "}){int i=s.indexOf(sep);if(i>0&&i<cut)cut=i;}
 return normalized(s.substring(0,cut).c_str());
}
// Score a catalog candidate; -1 means reject. Title must match (exactly, or
// ignoring "(Remix)"/"- Live" style suffixes), and one artist name must contain the other.
int candidateScore(const char *a,const char *t,const char *al,const char *ca,const char *ct,const char *cal){
 String wa=normalized(a),na=normalized(ca);
 if(!wa.length()||!na.length()||(wa.indexOf(na)<0&&na.indexOf(wa)<0))return -1;
 int score=0;
 if(normalized(t)==normalized(ct))score+=4;else if(baseTitle(t)==baseTitle(ct)&&baseTitle(t).length())score+=2;else return -1;
 if(wa==na)score+=2;
 if(al[0]&&normalized(al)==normalized(cal))score+=1;
 return score;
}
bool fetchCover(String cover,const char *hostSuffix,const char *from,const char *to,Download &data,char *outcome){
 int hostEnd=cover.indexOf('/',8);String host=hostEnd>8?cover.substring(8,hostEnd):String();
 if(!cover.startsWith("https://")||hostEnd<=8||!host.endsWith(hostSuffix)){strlcpy(outcome,"Catalog cover URL unsupported",64);return false;}
 cover.replace(from,to);
 if(!download(cover,data)){strlcpy(outcome,data.error,64);return false;}
 uint16_t w=0,h=0;
 if(TJpgDec.getJpgSize(&w,&h,data.data,data.size)!=JDR_OK||w!=64||h!=64){strlcpy(outcome,"Cover is not a 64x64 baseline JPEG",64);return false;}
 if(TJpgDec.drawJpg(0,0,data.data,data.size)!=JDR_OK){strlcpy(outcome,"Cover JPEG decoding failed",64);return false;}
 return true;
}
bool coverFromItunes(const char *a,const char *t,const char *al,Download &data,char *outcome){
 String url="https://itunes.apple.com/search?media=music&entity=song&limit=8&term="+encoded(a)+"%20"+encoded(t);
 if(!download(url,data)){strlcpy(outcome,data.error,64);return false;}
 JsonDocument doc;auto err=deserializeJson(doc,data.data,data.size);
 if(err){snprintf(outcome,64,"Apple JSON: %s",err.c_str());return false;}
 String cover;int best=-1;
 for(JsonObject item:doc["results"].as<JsonArray>()){
  int score=candidateScore(a,t,al,item["artistName"]|"",item["trackName"]|"",item["collectionName"]|"");
  if(score>best){best=score;cover=item["artworkUrl100"]|"";}
 }
 if(best<0)return false;
 return fetchCover(cover,".mzstatic.com","100x100bb","64x64bb",data,outcome);
}
bool coverFromDeezer(const char *a,const char *t,const char *al,Download &data,char *outcome){
 // Deezer search dislikes commas/ampersands, so query title + first artist only.
 String url="https://api.deezer.com/search?limit=8&q="+encoded(t)+"%20"+encoded(primaryArtist(a).c_str());
 if(!download(url,data)){strlcpy(outcome,data.error,64);return false;}
 JsonDocument filter;
 filter["data"][0]["title"]=true;filter["data"][0]["artist"]["name"]=true;
 filter["data"][0]["album"]["title"]=true;filter["data"][0]["album"]["cover_small"]=true;
 JsonDocument doc;auto err=deserializeJson(doc,data.data,data.size,DeserializationOption::Filter(filter));
 if(err){snprintf(outcome,64,"Deezer JSON: %s",err.c_str());return false;}
 String cover;int best=-1;
 for(JsonObject item:doc["data"].as<JsonArray>()){
  int score=candidateScore(a,t,al,item["artist"]["name"]|"",item["title"]|"",item["album"]["title"]|"");
  if(score>best){best=score;cover=item["album"]["cover_small"]|"";}
 }
 if(best<0)return false;
 return fetchCover(cover,".dzcdn.net","/56x56-","/64x64-",data,outcome);
}
// ---- Per-song generated art when no cover exists ---------------------------
void hsv(float h,float s,float v,float &r,float &g,float &b){
 h=fmodf(h,360.0f)/60.0f;int i=int(h);float f=h-i,p=v*(1-s),q=v*(1-s*f),u=v*(1-s*(1-f));
 switch(i){case 0:r=v;g=u;b=p;break;case 1:r=q;g=v;b=p;break;case 2:r=p;g=v;b=u;break;
  case 3:r=p;g=q;b=v;break;case 4:r=u;g=p;b=v;break;default:r=v;g=p;b=q;}
}
uint16_t pack565(float r,float g,float b){
 auto c=[](float v){return uint8_t(fmaxf(0,fminf(255,v)));};
 return ((c(r)>>3)<<11)|((c(g)>>2)<<5)|(c(b)>>3);
}
void songArt(uint16_t *pixels,const char *artist,const char *title){
 uint32_t h=2166136261u;
 for(const char *s=artist;*s;s++){h^=uint8_t(*s);h*=16777619u;}
 for(const char *s=title;*s;s++){h^=uint8_t(*s);h*=16777619u;}
 float hue=float(h%360),hue2=hue+35+float((h>>9)%80),split=float((h>>17)%628)/100.0f;
 float lr,lg,lb,ar,ag,ab;hsv(hue,0.72f,235,lr,lg,lb);hsv(hue2,0.6f,210,ar,ag,ab);
 for(int y=0;y<64;y++)for(int x=0;x<64;x++){
  float dx=x-31.5f,dy=y-31.5f,r=hypotf(dx,dy),ang=atan2f(dy,dx),R,G,B;
  if(r<14.5f){                       // colored label, two-tone so the spin is visible
   float k=0.5f+0.5f*cosf(ang-split),v=0.5f+0.5f*k;R=lr*v;G=lg*v;B=lb*v;
   if(r>13.2f){R=ar;G=ag;B=ab;}      // thin accent ring
   if(r<2.2f){R=G=B=235;}            // bright spindle dot
  }else{                             // dark vinyl, fine grooves, tinted sheen
   float groove=0.8f+0.2f*cosf(r*2.6f),sheen=powf(fmaxf(0,cosf(2*(ang-split))),6)*0.45f;
   R=(14+ar*sheen)*groove;G=(14+ag*sheen)*groove;B=(18+ab*sheen)*groove;
  }
  pixels[y*64+x]=pack565(R,G,B);
 }
}
void artworkTask(void*){
 auto *pixels=(uint16_t*)ps_malloc(N);auto *bytes=(uint8_t*)ps_malloc(65536);
 if(!pixels||!bytes){lock();strlcpy(artMessage,"Artwork memory unavailable",sizeof(artMessage));unlock();vTaskDelete(nullptr);return;}
 TJpgDec.setJpgScale(1);TJpgDec.setSwapBytes(false);TJpgDec.setCallback(decoded);decodeTarget=pixels;
 String lastKey="";uint32_t doneRevision=0,lastLookup=0,lastPublish=0,lastAttempt=0;
 bool matched=false;fallback(pixels);
 for(;;){
  char t[241],a[241],al[241];bool connected,play;uint32_t revision;
  lock();strlcpy(t,track,sizeof(t));strlcpy(a,artist,sizeof(a));strlcpy(al,album,sizeof(al));connected=phoneConnected;play=phonePlaying;revision=phoneRevision;unlock();
  String key=String(a)+"\n"+t+"\n"+al;
  if(connected&&t[0]){
   bool changed=key!=lastKey;
   if(changed){lastKey=key;matched=false;lastAttempt=0;songArt(pixels,a,t);publish((uint8_t*)pixels,play,true);}
   // One lookup per new track, plus bounded retries after network failures.
   if(!matched&&wifiOnline&&!otaActive&&time(nullptr)>1700000000&&(changed||!lastAttempt||millis()-lastAttempt>60000)&&millis()-lastLookup>=4000){
    lastLookup=lastAttempt=millis();Download data={bytes,0,65535,false,{}};
    char outcome[64]="No cover in Apple or Deezer; showing song art";
    lock();strlcpy(artMessage,"Looking up cover",sizeof(artMessage));unlock();
    // The phone sends only title/artist/album, so look the cover up online:
    // Apple first, then Deezer (better coverage for Uzbek and regional music).
    const char *source="";
    if(coverFromItunes(a,t,al,data,outcome))source="Apple";
    else if(wifiOnline&&!otaActive&&coverFromDeezer(a,t,al,data,outcome))source="Deezer";
    matched=source[0]!=0;
    // Metadata can change while HTTPS is in flight. Never publish old art.
    lock();bool current=strcmp(t,track)==0&&strcmp(a,artist)==0&&strcmp(al,album)==0;
    if(matched)snprintf(artMessage,sizeof(artMessage),"Matched cover (%s)",source);else strlcpy(artMessage,outcome,sizeof(artMessage));unlock();
    if(!current){lastKey="";continue;}
    if(!matched)songArt(pixels,a,t);
   }
   if(changed||revision!=doneRevision||millis()-lastPublish>2000){
    // Re-read playback after a potentially slow download.
    lock();play=phonePlaying;bool current=strcmp(t,track)==0&&strcmp(a,artist)==0&&strcmp(al,album)==0;unlock();
    if(current){publish((uint8_t*)pixels,play,true);lastPublish=millis();doneRevision=revision;}
   }
  }
  vTaskDelay(pdMS_TO_TICKS(100));
 }
}
}
void wirelessBegin(){
 guard=xSemaphoreCreateMutex();mailbox=(uint8_t*)ps_malloc(N);networkPixels=(uint8_t*)ps_malloc(N);
 if(!guard||!mailbox||!networkPixels){Serial.println("ERR wireless memory");while(true)delay(1000);}
 {Preferences sys;sys.begin("matrix-sys",false);bootCount=sys.getUInt("boots",0)+1;sys.putUInt("boots",bootCount);sys.end();}
 heap_caps_malloc_extmem_enable(2048);
 mbedtls_platform_set_calloc_free(tlsCalloc,heap_caps_free);
 phoneBegin();
 xTaskCreatePinnedToCore(networkTask,"matrix-network",8192,nullptr,1,nullptr,0);
 xTaskCreatePinnedToCore(artworkTask,"matrix-artwork",12288,nullptr,1,nullptr,0);
 gestureBegin();
 audioBegin();
}
bool wirelessOtaActive(){return otaActive;}
int wirelessDemoMode(){int m=demoMode.load();return m&&int32_t(millis()-demoUntil.load())<0?m:0;}
bool wirelessCommand(uint8_t cmd){
 lock();bool toMac=sources.owner==SourcePolicy::MAC&&uint32_t(millis()-sources.macSeen)<20000;unlock();
 if(!toMac)return phoneCommand(cmd);
 macCommandAt=millis();macCommand=cmd;return true;
}
int wirelessOtaProgress(){return otaProgress;}
bool wirelessMacAllowed(bool playing){if(!guard)return true;lock();bool allowed=allowMac(playing);if(allowed)pending=false;unlock();return allowed;}
bool wirelessTakeFrame(uint8_t *pixels,bool &playing,uint32_t &crc){
 if(!guard)return false;lock();bool available=pending;
 if(available){memcpy(pixels,mailbox,N);playing=pendingPlay;crc=pendingCRC;pending=false;}unlock();return available;
}
bool wirelessConfigure(const uint8_t *data,size_t n){
 if(n<2||data[0]<1||data[0]>32||data[1]>64||n!=size_t(data[0])+data[1]+2)return false;
 for(size_t i=2;i<n;i++)if(data[i]==0)return false;
 lock();memcpy(ssid,data+2,data[0]);ssid[data[0]]=0;memcpy(password,data+2+data[0],data[1]);password[data[1]]=0;configurePending=true;unlock();return true;
}
void wirelessStatus(char *out,size_t capacity){
 JsonDocument doc;lock();doc["wifi"]=wifiMessage;doc["ip"]=ip;doc["phone_connected"]=phoneConnected;doc["phone_playing"]=phonePlaying;
 doc["phone_title"]=track;doc["phone_artist"]=artist;doc["artwork"]=artMessage;unlock();
 doc["hostname"]="akbar-matrix.local";doc["free_heap"]=ESP.getFreeHeap();doc["largest_block"]=heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
 doc["free_psram"]=ESP.getFreePsram();doc["firmware"]=MATRIX_BUILD;
 doc["uptime_s"]=millis()/1000;doc["boot_count"]=bootCount;doc["reset_reason"]=resetReasonName();doc["reset_code"]=int(esp_reset_reason());
 doc["ota"]=otaActive?"updating":"ready";
 char phoneState[192];phoneDiagnostics(phoneState,sizeof(phoneState));doc["phone_debug"]=phoneState;
 char gesture[160];gestureDiagnostics(gesture,sizeof(gesture));doc["gesture"]=gesture;
 char audio[160];audioDiagnostics(audio,sizeof(audio));doc["audio"]=audio;
 serializeJson(doc,out,capacity);
}
void wirelessPhoneState(bool connected,bool play,const char *t,const char *a,const char *al){
 lock();if(phoneConnected!=connected||phonePlaying!=play||strcmp(track,t)||strcmp(artist,a)||strcmp(album,al))phoneRevision++;
 phoneConnected=connected;phonePlaying=play;strlcpy(track,t,sizeof(track));strlcpy(artist,a,sizeof(artist));strlcpy(album,al,sizeof(album));unlock();
}
