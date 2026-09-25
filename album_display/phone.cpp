#include "wireless.h"
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEAdvertising.h>
#include <host/ble_gatt.h>
#include <host/ble_hs.h>
#include <host/ble_store.h>
#include <atomic>

namespace {
const ble_uuid128_t AMS=BLE_UUID128_INIT(0xdc,0xf8,0x55,0xad,0x02,0xc5,0xf4,0x8e,0x3a,0x43,0x36,0x0f,0x2b,0x50,0xd3,0x89);
const ble_uuid128_t UPDATE=BLE_UUID128_INIT(0x02,0xc1,0x96,0xba,0x92,0xbb,0x0c,0x9a,0x1f,0x41,0x8d,0x80,0xce,0xab,0x7c,0x2f);
const ble_uuid128_t REMOTE=BLE_UUID128_INIT(0xc2,0x51,0xca,0xf7,0x56,0x0e,0xdf,0xb8,0x8a,0x4a,0xb1,0x57,0xd8,0x81,0x3c,0x9b);
const ble_uuid128_t ATTRIBUTE=BLE_UUID128_INIT(0xd7,0xd5,0xbb,0x70,0xa8,0xa3,0xab,0xa6,0xd8,0x46,0xab,0x23,0x8c,0xf3,0xb2,0xc6);
std::atomic<uint16_t> connection{BLE_HS_CONN_HANDLE_NONE};
std::atomic<bool> secure{false},ready{false},discovering{false};
std::atomic<uint32_t> retryAt{0};
std::atomic<int> lastError{0},phase{0};
std::atomic<uint32_t> securityAt{0};
std::atomic<bool> securityPending{false};
std::atomic<int> ourBonds{0},peerBonds{0},storeError{0},ourKey{-1},peerKey{-1};
// Address diagnostics: over-the-air type / identity type, whether the phone used a
// resolvable private address, and whether the controller resolved it to a bond.
std::atomic<int> peerOtaType{-1},peerIdType{-1},peerRpa{-1},peerResolved{-1};
ble_store_write_fn *originalStoreWrite=nullptr;
void countBonds(){
 int ours=0,peers=0;
 ble_store_util_count(BLE_STORE_OBJ_TYPE_OUR_SEC,&ours);
 ble_store_util_count(BLE_STORE_OBJ_TYPE_PEER_SEC,&peers);
 ourBonds=ours;peerBonds=peers;
}
int storeWrite(int type,const union ble_store_value *value){
 int rc=originalStoreWrite?originalStoreWrite(type,value):BLE_HS_ENOTSUP;
 storeError=rc;countBonds();return rc;
}
void inspectBond(uint16_t handle){
 ble_gap_conn_desc desc;
 if(ble_gap_conn_find(handle,&desc))return;
 peerOtaType=desc.peer_ota_addr.type;peerIdType=desc.peer_id_addr.type;
 peerRpa=desc.peer_ota_addr.type==BLE_ADDR_RANDOM&&(desc.peer_ota_addr.val[5]&0xC0)==0x40;
 peerResolved=desc.peer_ota_addr.type!=desc.peer_id_addr.type||memcmp(desc.peer_ota_addr.val,desc.peer_id_addr.val,6)!=0;
 ble_store_key_sec key={};key.peer_addr=desc.peer_id_addr;
 ble_store_value_sec value={};
 int rc=ble_store_read_our_sec(&key,&value);ourKey=rc? -rc:int(value.ltk_present);
 memset(&value,0,sizeof(value));
 rc=ble_store_read_peer_sec(&key,&value);peerKey=rc? -rc:int(value.irk_present);
 memset(&value,0,sizeof(value));countBonds();
}
uint16_t startHandle=0,endHandle=0,updateHandle=0,attributeHandle=0,updateEnd=0,cccd=0;
uint16_t gattStart=0,gattEnd=0,changedHandle=0,changedCCCD=0;
// AMS Remote Command: lets taps on the frame skip / pause on the iPhone.
uint16_t remoteHandle=0,remoteEnd=0,remoteCCCD=0;
std::atomic<int> remoteState{0},remoteResult{-99};  // state: 0 idle, 1 discovering, 2 subscribed, <0 error
std::atomic<uint32_t> remoteSupported{0},readyAt{0};
SemaphoreHandle_t metadataLock;
char song[241]={},performer[241]={},collection[241]={};bool play=false;
uint32_t metadataChanged=0;
std::atomic<uint8_t> truncated{0};std::atomic<bool> reading{false};uint8_t readingAttribute=0;
void report(){Serial.printf("PHONE connected=%d encrypted=%d subscribed=%d\n",connection!=BLE_HS_CONN_HANDLE_NONE,bool(secure),bool(ready));}
void failed(int error=-1){lastError=error;discovering=false;ready=false;retryAt=millis()+10000;}
void discover();
int remoteSubscribed(uint16_t c,const ble_gatt_error *e,ble_gatt_attr*,void*){if(c==connection)remoteState=e->status?-int(e->status):2;return 0;}
int remoteDescriptor(uint16_t c,const ble_gatt_error *e,uint16_t,const ble_gatt_dsc *d,void*){
 if(c!=connection)return 0;
 if(e->status==0){if(ble_uuid_u16(&d->uuid.u)==0x2902&&!remoteCCCD)remoteCCCD=d->handle;}
 else if(e->status==BLE_HS_EDONE){if(!remoteCCCD){remoteState=-4;return 0;}uint16_t on=1;if(ble_gattc_write_flat(c,remoteCCCD,&on,2,remoteSubscribed,nullptr))remoteState=-5;}
 else remoteState=-int(e->status);
 return 0;
}
int remoteWritten(uint16_t,const ble_gatt_error *e,ble_gatt_attr*,void*){remoteResult=e->status;return 0;}
int serviceChangedDescriptor(uint16_t c,const ble_gatt_error *e,uint16_t,const ble_gatt_dsc *d,void*){
 if(c!=connection)return 0;
 if(e->status==0&&ble_uuid_u16(&d->uuid.u)==0x2902)changedCCCD=d->handle;
 if(e->status==BLE_HS_EDONE&&changedCCCD){uint16_t indications=2;ble_gattc_write_flat(c,changedCCCD,&indications,2,nullptr,nullptr);}return 0;
}
int serviceChangedCharacteristic(uint16_t c,const ble_gatt_error *e,const ble_gatt_chr *chr,void*){
 if(c!=connection)return 0;
 if(e->status==0)changedHandle=chr->val_handle;
 if(e->status==BLE_HS_EDONE&&changedHandle)ble_gattc_disc_all_dscs(c,changedHandle,gattEnd,serviceChangedDescriptor,nullptr);return 0;
}
int subscribed(uint16_t c,const ble_gatt_error *e,ble_gatt_attr*,void *arg){
 if(c!=connection)return 0;if(e->status){failed(e->status);return 0;}
 uintptr_t stage=(uintptr_t)arg;int rc=0;
 if(stage==0){const uint8_t values[]={0,1};rc=ble_gattc_write_flat(c,updateHandle,values,sizeof(values),subscribed,(void*)1);}
 else if(stage==1){const uint8_t values[]={2,0,1,2};rc=ble_gattc_write_flat(c,updateHandle,values,sizeof(values),subscribed,(void*)2);}
 else{ready=true;discovering=false;phase=6;lastError=0;readyAt=millis();remoteState=0;report();if(gattStart){ble_uuid16_t uuid=BLE_UUID16_INIT(0x2a05);ble_gattc_disc_chrs_by_uuid(c,gattStart,gattEnd,&uuid.u,serviceChangedCharacteristic,nullptr);}}
 if(rc)failed(rc);return 0;
}
int descriptor(uint16_t c,const ble_gatt_error *e,uint16_t,const ble_gatt_dsc *d,void*){
 if(c!=connection)return 0;
 if(e->status==0&&ble_uuid_u16(&d->uuid.u)==0x2902)cccd=d->handle;
 else if(e->status==BLE_HS_EDONE){
  if(!cccd){failed(-4);return 0;}uint16_t notifications=1;phase=5;
  int rc=ble_gattc_write_flat(c,cccd,&notifications,2,subscribed,nullptr);if(rc)failed(rc);
 }else if(e->status)failed(e->status);return 0;
}
int characteristic(uint16_t c,const ble_gatt_error *e,const ble_gatt_chr *chr,void*){
 if(c!=connection)return 0;
 if(e->status==0){
  if(updateHandle&&chr->def_handle>updateHandle&&updateEnd==endHandle)updateEnd=chr->def_handle-1;
  if(remoteHandle&&chr->def_handle>remoteHandle&&remoteEnd==endHandle)remoteEnd=chr->def_handle-1;
  if(!ble_uuid_cmp(&chr->uuid.u,&REMOTE.u)){remoteHandle=chr->val_handle;remoteEnd=endHandle;}
  if(!ble_uuid_cmp(&chr->uuid.u,&UPDATE.u)){updateHandle=chr->val_handle;updateEnd=endHandle;}
  if(!ble_uuid_cmp(&chr->uuid.u,&ATTRIBUTE.u))attributeHandle=chr->val_handle;
 }else if(e->status==BLE_HS_EDONE){
  if(!updateHandle){failed(-3);return 0;}phase=4;
  int rc=ble_gattc_disc_all_dscs(c,updateHandle,updateEnd,descriptor,nullptr);if(rc)failed(rc);
 }else failed(e->status);return 0;
}
int service(uint16_t c,const ble_gatt_error *e,const ble_gatt_svc *s,void*){
 if(c!=connection)return 0;
 if(e->status==0){
  if(!ble_uuid_cmp(&s->uuid.u,&AMS.u)){startHandle=s->start_handle;endHandle=s->end_handle;}
  if(ble_uuid_u16(&s->uuid.u)==0x1801){gattStart=s->start_handle;gattEnd=s->end_handle;}
 }else if(e->status==BLE_HS_EDONE){
  if(!startHandle){failed(-2);return 0;}phase=3;
  int rc=ble_gattc_disc_all_chrs(c,startHandle,endHandle,characteristic,nullptr);if(rc)failed(rc);
 }else failed(e->status);return 0;
}
void discover(){
 if(discovering.exchange(true))return;
 ready=false;startHandle=endHandle=updateHandle=attributeHandle=cccd=gattStart=gattEnd=changedHandle=changedCCCD=0;
 remoteHandle=remoteEnd=remoteCCCD=0;remoteState=0;remoteSupported=0;
 phase=2;lastError=0;int rc=ble_gattc_disc_all_svcs(connection,service,nullptr);if(rc)failed(rc);
}
void setTrack(uint8_t attribute,const uint8_t *value,size_t length){
 if(attribute>2)return;
 xSemaphoreTake(metadataLock,portMAX_DELAY);
 char *dst=attribute==0?performer:(attribute==1?collection:song);
 length=min(length,size_t(240));memcpy(dst,value,length);dst[length]=0;metadataChanged=millis();
 xSemaphoreGive(metadataLock);
}
int fullValue(uint16_t c,const ble_gatt_error *e,ble_gatt_attr *attr,void*){
 if(c==connection&&e->status==0){uint8_t value[240];uint16_t n=0;
  if(ble_hs_mbuf_to_flat(attr->om,value,sizeof(value),&n)==0)setTrack(readingAttribute,value,n);
 }reading=false;return 0;
}
int selectAttribute(uint16_t c,const ble_gatt_error *e,ble_gatt_attr*,void*){
 if(c!=connection||e->status||ble_gattc_read(c,attributeHandle,fullValue,nullptr))reading=false;return 0;
}
int gap(ble_gap_event *event,void*){
 switch(event->type){
 case BLE_GAP_EVENT_CONNECT:
  if(!event->connect.status){connection=event->connect.conn_handle;secure=false;ready=false;discovering=false;retryAt=millis()+1000;
   phase=1;lastError=0;inspectBond(connection);
   securityAt=millis()+1000;securityPending=true;report();}
  break;
 case BLE_GAP_EVENT_ENC_CHANGE:
  if(event->enc_change.conn_handle==connection){ble_gap_conn_desc desc;
   secure=!event->enc_change.status&&!ble_gap_conn_find(connection,&desc)&&desc.sec_state.encrypted;
   lastError=event->enc_change.status;securityPending=false;
   if(secure)retryAt=millis()+500;inspectBond(connection);report();}
  break;
 case BLE_GAP_EVENT_DISCONNECT:
  if(event->disconnect.conn.conn_handle==connection){connection=BLE_HS_CONN_HANDLE_NONE;secure=false;ready=false;discovering=false;reading=false;truncated=0;
   phase=0;lastError=event->disconnect.reason;securityPending=false;
   xSemaphoreTake(metadataLock,portMAX_DELAY);play=false;song[0]=performer[0]=collection[0]=0;xSemaphoreGive(metadataLock);report();}
  break;
 case BLE_GAP_EVENT_NOTIFY_RX:{
  if(event->notify_rx.conn_handle!=connection||!secure)break;
  if(event->notify_rx.attr_handle==changedHandle){ready=false;discovering=false;retryAt=millis()+1000;break;}
  if(remoteHandle&&event->notify_rx.attr_handle==remoteHandle){
   uint8_t list[32];uint16_t n=0;uint32_t mask=0;
   if(!ble_hs_mbuf_to_flat(event->notify_rx.om,list,sizeof(list),&n))for(uint16_t i=0;i<n;i++)if(list[i]<32)mask|=1u<<list[i];
   remoteSupported=mask;break;}
  if(event->notify_rx.attr_handle!=updateHandle)break;
  uint8_t value[256];uint16_t n=0;
  if(ble_hs_mbuf_to_flat(event->notify_rx.om,value,sizeof(value)-1,&n)||n<3)break;value[n]=0;
  if(value[0]==2&&value[1]<=2){setTrack(value[1],value+3,n-3);if(value[2]&1)truncated.fetch_or(1<<value[1]);}
  if(value[0]==0&&value[1]==1){xSemaphoreTake(metadataLock,portMAX_DELAY);play=n>3&&value[3]=='1';xSemaphoreGive(metadataLock);}
  break;}
 }return 0;
}
}
void phoneDiagnostics(char *out,size_t capacity){
 snprintf(out,capacity,"link=%d encrypted=%d subscribed=%d phase=%d error=%d busy=%d bonds=%d/%d keys=%d/%d store=%d addr=%d/%d rpa=%d resolved=%d remote=%d/%lx cmd=%d",
  connection!=BLE_HS_CONN_HANDLE_NONE,bool(secure),bool(ready),phase.load(),lastError.load(),bool(discovering),
  ourBonds.load(),peerBonds.load(),ourKey.load(),peerKey.load(),storeError.load(),
  peerOtaType.load(),peerIdType.load(),peerRpa.load(),peerResolved.load(),
  remoteState.load(),(unsigned long)remoteSupported.load(),remoteResult.load());
}
bool phoneCommand(uint8_t command){
 uint16_t c=connection;
 if(c==BLE_HS_CONN_HANDLE_NONE||!ready||!remoteHandle)return false;
 uint32_t supported=remoteSupported.load();
 if(supported&&!(supported&(1u<<command)))return false;
 remoteResult=-1;
 return ble_gattc_write_flat(c,remoteHandle,&command,1,remoteWritten,nullptr)==0;
}
void phoneBegin(){
 metadataLock=xSemaphoreCreateMutex();BLEDevice::init("Akbar Matrix");
 originalStoreWrite=ble_hs_cfg.store_write_cb;ble_hs_cfg.store_write_cb=storeWrite;countBonds();
 ble_hs_cfg.sm_bonding=1;ble_hs_cfg.sm_sc=1;ble_hs_cfg.sm_mitm=0;
 ble_hs_cfg.sm_our_key_dist=BLE_SM_PAIR_KEY_DIST_ENC|BLE_SM_PAIR_KEY_DIST_ID;
 ble_hs_cfg.sm_their_key_dist=BLE_SM_PAIR_KEY_DIST_ENC|BLE_SM_PAIR_KEY_DIST_ID;
 BLEDevice::setCustomGapHandler(gap);BLEDevice::createServer()->advertiseOnDisconnect(true);
 BLEAdvertisementData advertising;advertising.setFlags(6);
 char solicitation[18]={17,0x15};memcpy(solicitation+2,AMS.value,16);advertising.addData(solicitation,sizeof(solicitation));
 BLEAdvertisementData response;response.setName("Akbar Matrix");
 auto *adv=BLEDevice::getAdvertising();adv->setAdvertisementData(advertising);adv->setScanResponseData(response);adv->setScanResponse(true);adv->start();
 Serial.println("PHONE advertising as Akbar Matrix");
}
void phoneTick(){
 static uint32_t reported=0;
 if(securityPending&&connection!=BLE_HS_CONN_HANDLE_NONE&&(int32_t)(millis()-securityAt.load())>=0){
  securityPending=false;
  // Give iOS time to restore a bonded link before asking it to start security.
  ble_gap_conn_desc desc;
  if(!ble_gap_conn_find(connection,&desc)&&desc.sec_state.encrypted){secure=true;retryAt=millis()+500;}
  else lastError=ble_gap_security_initiate(connection);
 }
 if(connection!=BLE_HS_CONN_HANDLE_NONE&&secure&&!ready&&!discovering&&(int32_t)(millis()-retryAt.load())>=0)discover();
 if(ready&&attributeHandle&&!reading&&truncated&&remoteState.load()!=1){
  uint8_t bits=truncated.load(),id=(bits&1)?0:((bits&2)?1:2);truncated.fetch_and(~(1<<id));reading=true;readingAttribute=id;
  const uint8_t request[]={2,id};if(ble_gattc_write_flat(connection,attributeHandle,request,2,selectAttribute,nullptr))reading=false;
 }
 // Subscribe to Remote Command after the main subscription settles, one GATT procedure at a time.
 if(ready&&remoteHandle&&remoteState.load()==0&&!reading&&millis()-readyAt.load()>1500){
  remoteState=1;remoteCCCD=0;
  if(ble_gattc_disc_all_dscs(connection,remoteHandle,remoteEnd,remoteDescriptor,nullptr))remoteState=-6;
 }
 if(millis()-reported<100)return;reported=millis();
 char t[241],a[241],al[241];bool p;uint32_t changed;
 xSemaphoreTake(metadataLock,portMAX_DELAY);strlcpy(t,song,sizeof(t));strlcpy(a,performer,sizeof(a));strlcpy(al,collection,sizeof(al));p=play;changed=metadataChanged;xSemaphoreGive(metadataLock);
 // Group separate title/artist/album notifications before changing the record.
 if(millis()-changed>=800)wirelessPhoneState(ready,p,t,a,al);
}
