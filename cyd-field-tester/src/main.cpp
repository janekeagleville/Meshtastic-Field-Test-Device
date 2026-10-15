#include <Arduino.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <SD.h>
#include <Preferences.h>
#include <math.h>
#include "config.h"
#include "protocol.h"
#include "splash.h"

TFT_eSPI tft; HardwareSerial radio(2); mesh::Framer framing; int uiScreen=-1;bool dirty=false;
void sendFrame(const uint8_t* p,size_t len);
// Aktywny test lacza: pakiet bezposredni do wybranego wezla z zadaniem ACK; RSSI/SNR to parametry ACK odebranego przez Trackera.
struct Probe{bool on=true,pending=false;uint32_t node=0,id=0,sentAt=0,nextAt=0,ok=0,lost=0,seq=0,rttSum=0;int32_t rtt=-1;};
Probe probe;
struct Node {uint32_t id=0,last=0,count=0;char name[40]={};int rssi=0;float snr=0;int lo=0,hi=0;double sum=0;int16_t hist[HISTORY]={};unsigned used=0,head=0;bool mqtt=false;int hops=-1;};
Node nodes[MAX_NODES];uint32_t rxBytes=0,frames=0,baudNow=RADIO_BAUD,baudSince=0;uint32_t myId=0,configId=0,lastFrame=0,lastRequest=0,lastHeartbeat=0,lastDraw=0,lastFlush=0;
bool configured=false,everFrame=false,logging=false,sdReady=false,detail=false;unsigned page=0;int selected=-1;
File csv;char logPath[64]={};uint32_t logNode=0,clkEpoch=0,clkAt=0,uiMsgUntil=0;const char* uiMsg="";uint32_t written=0,parseErrors=0,logErrors=0;mesh::Position ownPosition;bool ownPositionLive=false;uint32_t positionAt=0;
bool invertScr=false;Preferences prefs;uint16_t calX0=300,calX1=3800,calY0=300,calY1=3800;bool calSwap=true;
Node* node(uint32_t id){if(!id)return nullptr;for(auto &n:nodes)if(n.id==id)return &n;for(auto &n:nodes)if(!n.id){n=Node{};n.id=id;return &n;}Node* oldest=&nodes[0];for(auto &n:nodes)if(uint32_t(millis()-n.last)>uint32_t(millis()-oldest->last))oldest=&n;*oldest=Node{};oldest->id=id;return oldest;}
void user(Node* n,const uint8_t* p,size_t len){if(!n)return;mesh::Reader r(p,len);mesh::Field f;char name[40]={},shortName[12]={};while(r.next(f)){if(f.tag==2&&f.wire==2)mesh::copyText(name,sizeof(name),f);if(f.tag==3&&f.wire==2)mesh::copyText(shortName,sizeof(shortName),f);}if(r.ok()){if(name[0])strlcpy(n->name,name,sizeof(n->name));else if(shortName[0])strlcpy(n->name,shortName,sizeof(n->name));}}
void updatePosition(const uint8_t* p,size_t len,bool live){mesh::Position pos;if(mesh::position(p,len,pos)&&pos.valid()){ownPosition=pos;positionAt=millis();ownPositionLive=live;}}
void nodeInfo(const uint8_t* p,size_t len){mesh::Reader r(p,len);mesh::Field f;uint32_t id=0;const uint8_t *u=nullptr,*pos=nullptr;size_t ul=0,pl=0;while(r.next(f)){if(f.tag==1&&f.wire==0)id=f.value;if(f.tag==2&&f.wire==2){u=f.bytes;ul=f.size;}if(f.tag==3&&f.wire==2){pos=f.bytes;pl=f.size;}}if(!r.ok()){++parseErrors;return;}Node* n=node(id);if(u)user(n,u,ul);if(id==myId&&pos)updatePosition(pos,pl,configured);}
void clockSeen(uint32_t e){if(e>1700000000UL&&e<4000000000UL){clkEpoch=e;clkAt=millis();}}
bool clockNow(uint32_t& e){if(!clkEpoch)return false;e=clkEpoch+(millis()-clkAt)/1000;return true;}
void flash(const char* m,uint32_t ms=3500){uiMsg=m;uiMsgUntil=millis()+ms;}
void logPacket(const mesh::Packet& p,const Node& n,int32_t rtt){if(!logging||!csv)return;if(logNode&&p.from!=logNode)return;char lat[24]="",lon[24]="",age[24]="",rttText[16]="";if(rtt>=0)snprintf(rttText,sizeof(rttText),"%ld",(long)rtt);bool valid=ownPosition.valid();if(valid){snprintf(lat,sizeof(lat),"%.7f",ownPosition.lat*1e-7);snprintf(lon,sizeof(lon),"%.7f",ownPosition.lon*1e-7);snprintf(age,sizeof(age),"%lu",(unsigned long)((millis()-positionAt)/1000));}
 // GPS coordinates belong to the local Tracker, never the remote sender.
 char gpsAge[24]="";if(valid&&ownPosition.epoch&&p.epoch>=ownPosition.epoch)snprintf(gpsAge,sizeof(gpsAge),"%lu",(unsigned long)(p.epoch-ownPosition.epoch));
 size_t bytes=csv.printf("%lu,%lu,%08lx,%08lx,%d,%.2f,%d,%u,%u,%u,%s,%s,%s,%u,%u,%lu,%s,%s\n",(unsigned long)millis(),(unsigned long)p.epoch,(unsigned long)p.from,(unsigned long)p.id,p.rssi,p.snr,n.hops,p.relay,p.mqtt,p.transport,lat,lon,age,valid&&ownPositionLive,valid?ownPosition.source:0,(unsigned long)(valid?ownPosition.epoch:0),gpsAge,rttText);
 if(!bytes||csv.getWriteError()){++logErrors;logging=false;csv.close();flash("BLAD SD");}else ++written;}
void probeLost(){probe.pending=false;++probe.lost;probe.nextAt=max<uint32_t>(probe.sentAt+PROBE_INTERVAL_MS,uint32_t(millis())+2000);dirty=true;}
void packet(const uint8_t* p,size_t len){mesh::Packet m;if(!mesh::packet(p,len,m)){++parseErrors;return;}clockSeen(m.epoch);int32_t rtt=-1;
 // Lokalny Tracker zglasza bledy kolejki/retransmisji; potwierdzenie "przyjeto do wyslania" (NONE) jest pomijane.
 if(m.from==myId){if(m.port==3&&m.payload)updatePosition(m.payload,m.payloadSize,true);
  if(m.port==5&&m.payload&&probe.pending&&m.requestId==probe.id){unsigned e;bool ack;if(mesh::routing(m.payload,m.payloadSize,e,ack)&&ack&&e!=0)probeLost();}return;}
 // ACK z wezla docelowego: wynik testu (RSSI/SNR liczy sie jak kazdy odebrany pakiet nizej).
 if(m.port==5&&m.payload&&probe.pending&&m.from==probe.node&&m.requestId==probe.id){unsigned e;bool ack;if(mesh::routing(m.payload,m.payloadSize,e,ack)&&ack){
   if(e==0){rtt=int32_t(millis()-probe.sentAt);probe.rtt=rtt;probe.rttSum+=rtt;++probe.ok;probe.pending=false;probe.nextAt=max<uint32_t>(probe.sentAt+PROBE_INTERVAL_MS,uint32_t(millis())+2000);dirty=true;}else probeLost();}}
 if(m.port==4&&m.payload)user(node(m.from),m.payload,m.payloadSize);
 // Exclude local transmissions, replay without RSSI and MQTT ingress.
 if(!m.hasRssi||m.mqtt||m.transport>4||!m.from||!isfinite(m.snr)||m.rssi < -200||m.rssi>20)return;
 Node* n=node(m.from);if(!n)return;n->last=millis();n->rssi=m.rssi;n->snr=m.snr;n->mqtt=m.mqtt;
 // Nonzero initial hop count can be interpreted; zero is ambiguous on old firmware.
 n->hops=m.hasHopStart&&m.hopStart>0&&m.hopStart>=m.hopLimit?int(m.hopStart-m.hopLimit):-1;
 if(!n->count)n->lo=n->hi=m.rssi;n->lo=min(n->lo,int(m.rssi));n->hi=max(n->hi,int(m.rssi));++n->count;n->sum+=m.rssi;
 n->hist[n->head]=m.rssi;n->head=(n->head+1)%HISTORY;n->used=min(n->used+1,HISTORY);logPacket(m,*n,rtt);if(detail&&selected>=0&&n==&nodes[selected])dirty=true;
}
void fromRadio(const uint8_t* p,size_t len){mesh::Reader r(p,len);mesh::Field f;
 // Validate whole envelope before interpreting any nested data.
 while(r.next(f)){}if(!r.ok()){++parseErrors;return;}
 lastFrame=millis();everFrame=true;++frames;mesh::Reader data(p,len);while(data.next(f)){
 if(f.tag==2&&f.wire==2)packet(f.bytes,f.size);
 if(f.tag==3&&f.wire==2){mesh::Reader info(f.bytes,f.size);mesh::Field a;uint32_t id=0;while(info.next(a))if(a.tag==1&&a.wire==0)id=a.value;if(info.ok()&&id){if(myId&&myId!=id){ownPosition=mesh::Position{};ownPositionLive=false;}myId=id;}}
 if(f.tag==4&&f.wire==2)nodeInfo(f.bytes,f.size);
 if(f.tag==7&&f.wire==0&&f.value==configId)configured=true;
 if(f.tag==8&&f.wire==0&&f.value){configured=false;ownPositionLive=false;ownPosition=mesh::Position{};lastRequest=0;}
}}
void sendFrame(const uint8_t* p,size_t len){uint8_t head[]={0x94,0xc3,uint8_t(len>>8),uint8_t(len)};radio.write(head,4);radio.write(p,len);}
void probeSend(uint32_t now){uint8_t buf[96];uint32_t id=esp_random();if(!id)id=1;size_t len=mesh::buildProbe(buf,sizeof buf,probe.node,id,uint16_t(probe.seq),PROBE_PORT,PROBE_CHANNEL,PROBE_HOP_LIMIT);if(!len)return;
 sendFrame(buf,len);probe.id=id;probe.sentAt=now;probe.pending=true;++probe.seq;dirty=true;}
void probeStart(uint32_t nodeId){probe=Probe{};probe.node=nodeId;probe.nextAt=millis();dirty=true;}
void probeTick(uint32_t now){if(!detail||selected<0||!probe.node)return;
 if(probe.pending&&now-probe.sentAt>PROBE_TIMEOUT_MS)probeLost();
 if(probe.on&&configured&&!probe.pending&&int32_t(now-probe.nextAt)>=0)probeSend(now);}
void requestConfig(){configId=esp_random();if(!configId)configId=1;uint8_t p[6];size_t len=0;p[len++]=0x18;uint32_t v=configId;do{p[len++]=(v&127)|(v>127?128:0);v>>=7;}while(v);sendFrame(p,len);lastRequest=millis();}
// Meshtastic serial defaults to 38400 baud when serial.baud is not set; until the first valid frame arrives, cycle through common speeds.
void nextBaud(uint32_t now){static const uint32_t b[]={115200,38400,9600,57600};unsigned i=0;for(unsigned k=0;k<4;k++)if(b[k]==baudNow)i=k;baudNow=b[(i+1)%4];radio.end();radio.setRxBufferSize(4096);radio.begin(baudNow,SERIAL_8N1,RADIO_RX,RADIO_TX);baudSince=now;requestConfig();}
// Karta SD: HSPI zajmuje ekran, SD ma wlasne VSPI; przy bledzie inicjalizacji probuje nizszych taktowan (4 MHz, 1 MHz).
bool sdMount(){static bool spiUp=false;if(!spiUp){SPI.begin(18,19,23,SD_CS);spiUp=true;}static const uint32_t hz[]={10000000,4000000,1000000};for(unsigned i=0;i<3;i++){SD.end();if(SD.begin(SD_CS,SPI,hz[i])&&SD.cardType()!=CARD_NONE)return true;}SD.end();return false;}
// Nazwa pliku: "<Nazwa noda>_DDMMYY_HH-MM-SS.csv" (FAT nie dopuszcza ':', wiec godzina ma myslniki).
void logStem(uint32_t id,char* out,size_t cap){char base[40]="SIEC";if(id){Node* n=node(id);char tmp[48]="";size_t k=n&&n->name[0]?mesh::displayName(n->name,tmp,sizeof(tmp)):0;if(!k)snprintf(tmp,sizeof(tmp),"!%08lx",(unsigned long)id);char safe[40];if(!mesh::fileSafe(tmp,safe,24))snprintf(safe,sizeof(safe),"!%08lx",(unsigned long)id);strlcpy(base,safe,sizeof(base));}
 uint32_t e;if(clockNow(e)){mesh::Civil c=mesh::civilLocal(e);snprintf(out,cap,"%s_%02u%02u%02u_%02u-%02u-%02u",base,c.d,c.mo,unsigned(c.y%100),c.h,c.mi,c.s);}
 else{uint32_t q=prefs.getULong("logseq",0)+1;prefs.putULong("logseq",q);snprintf(out,cap,"%s_BRAK-CZASU_%03lu",base,(unsigned long)(q%1000));}}
void stopLog(){if(csv){csv.flush();csv.close();}logging=false;}
void startLog(uint32_t id){if(!sdReady)sdReady=sdMount();if(!sdReady){++logErrors;flash("BRAK SD");return;}
 char stem[56];logStem(id,stem,sizeof(stem));snprintf(logPath,sizeof(logPath),"/%s.csv",stem);for(unsigned i=2;i<100&&SD.exists(logPath);i++)snprintf(logPath,sizeof(logPath),"/%s-%u.csv",stem,i);
 csv=SD.open(logPath,FILE_WRITE);
 if(!csv){for(unsigned i=1;i<10000;i++){snprintf(logPath,sizeof(logPath),"/LOG%05u.csv",i);if(!SD.exists(logPath)){csv=SD.open(logPath,FILE_WRITE);break;}}if(csv)flash("KROTKA NAZWA");}
 if(!csv){++logErrors;sdReady=false;flash("BLAD SD");return;}
 csv.clearWriteError();csv.println("uptime_ms,rx_epoch,sender_id,packet_id,rssi_dbm,snr_db,hops,relay_low_byte,via_mqtt,transport,tracker_lat,tracker_lon,position_receipt_age_s,position_live,position_source,position_epoch,gps_age_s,probe_rtt_ms");
 if(csv.getWriteError()){++logErrors;csv.close();flash("BLAD SD");return;}
 logging=true;logNode=id;written=0;lastFlush=millis();Serial.printf("LOG: %s\n",logPath);}
// ZAPIS: na ekranie pracy zapisuje wybrany node, na liscie - caly ruch (plik "SIEC_...").
void toggleLog(){if(logging){stopLog();return;}startLog(detail&&selected>=0?nodes[selected].id:0);}
// Bit-banged XPT2046 uses its own pins, leaving TFT HSPI and SD VSPI independent.
uint16_t touchADC(uint8_t cmd){digitalWrite(TOUCH_CS,LOW);for(int b=7;b>=0;b--){digitalWrite(TOUCH_MOSI,(cmd>>b)&1);digitalWrite(TOUCH_CLK,HIGH);delayMicroseconds(2);digitalWrite(TOUCH_CLK,LOW);}delayMicroseconds(8);uint16_t v=0;for(int b=0;b<16;b++){digitalWrite(TOUCH_CLK,HIGH);delayMicroseconds(2);v=(v<<1)|digitalRead(TOUCH_MISO);digitalWrite(TOUCH_CLK,LOW);}digitalWrite(TOUCH_CS,HIGH);return (v>>3)&4095;}
bool rawTouch(uint16_t& x,uint16_t& y){uint16_t z=touchADC(0xb0);if(z<150)return false;uint32_t a=0,b=0;for(int i=0;i<5;i++){a+=touchADC(0xd0);b+=touchADC(0x90);}x=a/5;y=b/5;return x>50&&x<4050&&y>50&&y<4050;}
bool touch(int& x,int& y){uint16_t a,b;if(!rawTouch(a,b))return false;int u=calSwap?b:a,v=calSwap?a:b;x=constrain(map(u,calX0,calX1,0,319),0L,319L);y=constrain(map(v,calY0,calY1,0,239),0L,239L);return true;}
void calibrate(){uiScreen=-1;uint16_t xs[4],ys[4];const int cx[]={25,295,295,25},cy[]={25,25,215,215};
 for(int i=0;i<4;i++){tft.fillScreen(TFT_BLACK);tft.setTextColor(TFT_WHITE,TFT_BLACK);tft.drawString("Dotknij krzyzyka",80,100,2);tft.drawLine(cx[i]-10,cy[i],cx[i]+10,cy[i],TFT_YELLOW);tft.drawLine(cx[i],cy[i]-10,cx[i],cy[i]+10,TFT_YELLOW);uint16_t a,b;while(rawTouch(a,b)){delay(10);}while(!rawTouch(a,b)){while(radio.available())framing.feed(radio.read(),millis(),fromRadio);delay(5);}xs[i]=a;ys[i]=b;delay(250);}
 calSwap=abs(int(ys[1])-int(ys[0]))>abs(int(xs[1])-int(xs[0]));int lx=calSwap?(ys[0]+ys[3])/2:(xs[0]+xs[3])/2;int rx=calSwap?(ys[1]+ys[2])/2:(xs[1]+xs[2])/2;int ty=calSwap?(xs[0]+xs[1])/2:(ys[0]+ys[1])/2;int by=calSwap?(xs[2]+xs[3])/2:(ys[2]+ys[3])/2;
 calX0=constrain(lx-(rx-lx)*25/270,0,4095);calX1=constrain(lx+(rx-lx)*294/270,0,4095);calY0=constrain(ty-(by-ty)*25/190,0,4095);calY1=constrain(ty+(by-ty)*214/190,0,4095);
 if(abs(int(calX1)-int(calX0))<200||abs(int(calY1)-int(calY0))<200){calX0=calY0=300;calX1=calY1=3800;return;}
 prefs.putUShort("x0",calX0);prefs.putUShort("x1",calX1);prefs.putUShort("y0",calY0);prefs.putUShort("y1",calY1);prefs.putBool("swap",calSwap);prefs.putBool("valid",true);
}
unsigned order[MAX_NODES],orderCount=0;void sortNodes(){orderCount=0;for(unsigned i=0;i<MAX_NODES;i++)if(nodes[i].count)order[orderCount++]=i;for(unsigned i=1;i<orderCount;i++){unsigned v=order[i],j=i;while(j&&uint32_t(millis()-nodes[v].last)<uint32_t(millis()-nodes[order[j-1]].last)){order[j]=order[j-1];--j;}order[j]=v;}if(page*5>=orderCount)page=0;}
// ===================== UI 320x240 (poziomo) =====================
#define C565(r,g,b) ((uint16_t)((((r)&0xF8)<<8)|(((g)&0xFC)<<3)|((b)>>3)))
const uint16_t cBG=C565(0,34,22),cHDR=C565(0,18,12),cPANEL=C565(6,56,40),cLINE=C565(30,170,120),cTRACK=C565(14,84,60),
 cTURQ=C565(0,255,224),cGREEN=C565(30,255,120),cAMBER=C565(255,190,0),cRED=C565(255,56,44),cDIM=C565(140,230,200),cWHITE=0xFFFF,
 cOKBG=C565(4,78,46),cWARNBG=C565(80,54,0),cBADBG=C565(86,14,10),cGREY=C565(170,190,182);
enum Quality{Q_NONE,Q_GOOD,Q_FAIR,Q_BAD};
// Ocena ostatniego odcinka radiowego (progi w config.h).
Quality quality(const Node& n){if(!n.count)return Q_NONE;if(n.snr>=SNR_GOOD&&n.rssi>=RSSI_GOOD)return Q_GOOD;if(n.snr>=SNR_FAIR&&n.rssi>=RSSI_FAIR)return Q_FAIR;return Q_BAD;}
uint16_t qColor(Quality q){return q==Q_GOOD?cGREEN:q==Q_FAIR?cAMBER:q==Q_BAD?cRED:cGREY;}
uint16_t qBg(Quality q){return q==Q_GOOD?cOKBG:q==Q_FAIR?cWARNBG:q==Q_BAD?cBADBG:cPANEL;}
const char* qText(Quality q){return q==Q_GOOD?"CZYTELNY":q==Q_FAIR?"GRANICZNY":q==Q_BAD?"NIECZYTELNY":"BRAK DANYCH";}
uint16_t gradient(float t){static const float stop[]={0,.45f,.8f,1};static const uint8_t c[4][3]={{255,56,44},{255,190,0},{30,255,120},{0,255,224}};
 t=constrain(t,0.f,1.f);int i=0;while(i<2&&t>stop[i+1])++i;float u=(t-stop[i])/(stop[i+1]-stop[i]);uint8_t k[3];for(int j=0;j<3;j++)k[j]=c[i][j]+(c[i+1][j]-c[i][j])*u;return C565(k[0],k[1],k[2]);}
void txt(const String& s,int x,int y,int font,uint16_t fg,uint16_t bg,uint8_t datum=TL_DATUM){tft.setTextDatum(datum);tft.setTextColor(fg,bg);tft.drawString(s,x,y,font);tft.setTextDatum(TL_DATUM);}
// Name fitted to a pixel width (font 2): transliterated to ASCII, cut with ".." so it never overlaps neighbouring columns.
String fitName(const Node& n,int maxW){char buf[48];size_t len=n.name[0]?mesh::displayName(n.name,buf,sizeof(buf)):0;if(!len)snprintf(buf,sizeof(buf),"!%08lx",(unsigned long)n.id);
 String s(buf);if(tft.textWidth(s,2)<=maxW)return s;while(s.length()>1){s.remove(s.length()-1,1);String t=s;t.trim();t+="..";if(tft.textWidth(t,2)<=maxW)return t;}return String("..");}
void panel(int x,int y,int w,int h){tft.fillRoundRect(x,y,w,h,6,cPANEL);tft.drawRoundRect(x,y,w,h,6,cLINE);}
void button(int x,int y,int w,const String& s,uint16_t fg,uint16_t bg){tft.fillRoundRect(x,y,w,18,6,bg);tft.drawRoundRect(x,y,w,18,6,fg);txt(s,x+w/2,y+9,2,fg,bg,MC_DATUM);}
void splash(){tft.setSwapBytes(true);uint16_t row[SPLASH_W];for(unsigned j=0;j<SPLASH_H;j++){for(unsigned i=0;i<SPLASH_W;i++)row[i]=pgm_read_word(splashImg+j*SPLASH_W+i);tft.pushImage(0,j,SPLASH_W,1,row);}}
void drawHeader(bool alive){
 tft.fillRect(0,0,320,22,cHDR);tft.drawFastHLine(0,22,320,cLINE);
 tft.drawLine(9,13,15,7,cTURQ);tft.drawLine(15,7,21,14,cTURQ);
 const int nx[]={9,15,21},ny[]={13,7,14};for(int i=0;i<3;i++){tft.fillCircle(nx[i],ny[i],3,cTURQ);tft.fillCircle(nx[i],ny[i],1,cHDR);}
 {const int f=tft.textWidth("LoRaWAN Field Test Kit",2)<=204?2:1,y=f==2?3:7;txt("LoRaWAN",29,y,f,cWHITE,cHDR);int w=tft.textWidth("LoRaWAN",f);txt("Field Test Kit",29+w+(f==2?6:5),y,f,cTURQ,cHDR);}
 bool ok=configured&&alive;uint16_t c=ok?cGREEN:cAMBER,bg=ok?cOKBG:cWARNBG;const char* s=configured?(alive?"API OK":"API ?"):"LACZENIE";
 tft.fillRoundRect(240,4,72,15,7,bg);tft.drawRoundRect(240,4,72,15,7,c);tft.fillCircle(249,11,3,c);txt(s,257,7,1,c,bg);}
void drawGps(){bool v=ownPosition.valid();bool live=v&&ownPositionLive;uint16_t c=live?cGREEN:cAMBER,bg=live?cOKBG:cWARNBG;
 tft.fillRoundRect(8,26,304,16,8,bg);tft.drawRoundRect(8,26,304,16,8,c);tft.fillCircle(19,34,3,c);
 String s=v?String(ownPosition.lat*1e-7,5)+", "+String(ownPosition.lon*1e-7,5)+"  "+String((millis()-positionAt)/1000)+"s"+(ownPositionLive?"":" CACHE"):String("GPS: brak pozycji Trackera");
 txt(s,28,26,2,c,bg);}
void drawGauge(const Node& n,Quality q){
 const int px=8,py=71,pw=152,ph=79,cx=84,cy=139,ro=53,ri=43;panel(px,py,pw,ph);bool mine=probe.node==n.id;uint16_t dc=!mine||!probe.on?cGREY:probe.pending?cAMBER:((millis()/500)&1?cTURQ:cTRACK);tft.fillCircle(15,80,3,dc);txt("RSSI",22,76,1,cDIM,cPANEL);
 const char* t=qText(q);uint16_t c=qColor(q),bg=qBg(q);int w=6*strlen(t)+22,bx=px+pw-6-w;
 tft.fillRoundRect(bx,74,w,13,6,bg);tft.drawRoundRect(bx,74,w,13,6,c);tft.fillCircle(bx+8,80,2,c);txt(t,bx+14,76,1,c,bg);
 tft.drawArc(cx,cy,ro,ri,90,270,cTRACK,cPANEL,true);
 float f=constrain((n.rssi-GAUGE_MIN)/float(GAUGE_MAX-GAUGE_MIN),0.f,1.f),endA=90+f*180;
 for(int k=0;k<18;k++){float a0=90+k*10,a1=a0+10;if(a0>=endA)break;if(a1>endA)a1=endA;if(a1-a0<1)break;tft.drawArc(cx,cy,ro,ri,(uint32_t)a0,(uint32_t)ceilf(a1),gradient(k/17.f),cPANEL,true);}
 float ar=endA*DEG_TO_RAD,rm=(ro+ri)/2.f;tft.fillCircle(cx-rm*sinf(ar),cy+rm*cosf(ar),3,cWHITE);
 txt(String(n.rssi),cx,125,4,cWHITE,cPANEL,MC_DATUM);txt("dBm",cx,145,1,cTURQ,cPANEL,MC_DATUM);
 txt(String(GAUGE_MIN),cx-50,145,1,cDIM,cPANEL,MC_DATUM);txt(String(GAUGE_MAX),cx+50,145,1,cDIM,cPANEL,MC_DATUM);}
void drawSnr(const Node& n){panel(168,71,144,38);txt("SNR",176,76,1,cDIM,cPANEL);uint16_t c=n.snr>=SNR_GOOD?cTURQ:n.snr>=SNR_FAIR?cAMBER:cRED;
 txt(String(n.snr,1),208,76,4,c,cPANEL);txt("dB",286,88,2,cDIM,cPANEL);tft.fillRoundRect(176,104,128,3,1,cTRACK);
 float f=constrain((n.snr-SNR_MIN)/float(SNR_MAX-SNR_MIN),0.f,1.f);if(f>0)tft.fillRoundRect(176,104,max(2,int(128*f)),3,1,c);}
void drawHops(const Node& n){panel(168,112,144,38);txt("PRZEKAZANY",176,117,1,cDIM,cPANEL);
 String hs=n.hops<0?String("?"):n.hops==0?String("BEZP."):String(min(n.hops,HOP_MAX))+"/"+String(HOP_MAX)+" hop";txt(hs,304,115,2,cWHITE,cPANEL,TR_DATUM);
 int lit=n.hops<0?0:min(n.hops,HOP_MAX);for(int i=0;i<HOP_MAX;i++){int x=184+int(i*19.5f);if(i<HOP_MAX-1)tft.drawFastHLine(x+4,140,16,i<lit-1?cTURQ:cLINE);if(i<lit)tft.fillCircle(x,140,4,cTURQ);else{tft.fillCircle(x,140,4,cTRACK);tft.drawCircle(x,140,4,cLINE);}}}
void drawStats(const Node& n){const char* L[]={"N","MIN","MAX","SR."};String V[]={String(n.count),String(n.lo),String(n.hi),String(n.sum/n.count,1)};
 for(int i=0;i<4;i++){int x=8+i*77;panel(x,153,70,26);txt(L[i],x+35,158,1,cDIM,cPANEL,MC_DATUM);txt(V[i],x+35,170,2,cWHITE,cPANEL,MC_DATUM);}}
void drawChart(const Node& n){panel(8,182,304,30);txt("RSSI OSTATNICH "+String(n.used),16,185,1,cDIM,cPANEL);if(probe.node==n.id){uint32_t tot=probe.ok+probe.lost;String t=String(probe.on?"TEST ":"PAUZA ")+String(probe.ok)+"/"+String(tot);if(probe.rtt>=0)t+=" RTT "+String(probe.rtt/1000.0,1)+"s";txt(t,304,185,1,probe.lost&&probe.lost*4>tot?cAMBER:cTURQ,cPANEL,TR_DATUM);}
 const int x1=302,y0=195,y1=208,span=282;auto ym=[&](int v){return y1-(constrain(v,GAUGE_MIN,GAUGE_MAX)-GAUGE_MIN)*(y1-y0)/(GAUGE_MAX-GAUGE_MIN);};
 tft.drawFastHLine(16,ym(RSSI_GOOD),288,cTRACK);unsigned start=(n.head+HISTORY-n.used)%HISTORY;int px=0,py=0;
 for(unsigned i=0;i<n.used;i++){int x=x1-int((n.used-1-i)*span/(HISTORY-1)),y=ym(n.hist[(start+i)%HISTORY]);if(i)tft.drawLine(px,py,x,y,cTURQ);px=x;py=y;}
 if(n.used)tft.fillCircle(px,py,2,cWHITE);}
void drawNav(bool detailView){tft.fillRect(0,214,320,26,cHDR);tft.drawFastHLine(0,214,320,cLINE);
 button(8,218,94,detailView?"LISTA":"DALEJ",cTURQ,cOKBG);
 if(logging)button(113,218,94,String("STOP ")+String(written),cRED,cBADBG);else if(millis()<uiMsgUntil)button(113,218,94,uiMsg,cAMBER,cWARNBG);else button(113,218,94,"ZAPIS",cTURQ,cOKBG);
 button(218,218,94,"KAL",cTURQ,cOKBG);}
void drawDetail(const Node& n){Quality q=quality(n);char id[16];snprintf(id,sizeof(id),"!%08lx",(unsigned long)n.id);
 panel(8,45,304,22);txt("NODE",16,52,1,cDIM,cPANEL);int idW=tft.textWidth(id,2);txt(fitName(n,304-idW-8-54),54,48,2,cWHITE,cPANEL);txt(id,304,48,2,cTURQ,cPANEL,TR_DATUM);
 drawGauge(n,q);drawSnr(n);drawHops(n);drawStats(n);drawChart(n);}
void drawList(){sortNodes();unsigned pages=max(1U,(orderCount+4)/5);
 tft.fillRect(0,44,320,24,cBG);txt("NODE",24,49,1,cDIM,cBG);txt("RSSI",138,49,1,cDIM,cBG);txt("SNR",195,49,1,cDIM,cBG);txt("WIEK",257,49,1,cDIM,cBG);txt(String(page+1)+"/"+String(pages),312,49,1,cDIM,cBG,TR_DATUM);
 for(unsigned i=0;i<5;i++){int y=70+i*27;if(page*5+i>=orderCount||!orderCount){tft.fillRect(0,y,320,27,cBG);continue;}
  Node &n=nodes[order[page*5+i]];Quality q=quality(n);char id[16];snprintf(id,sizeof(id),"!%08lx",(unsigned long)n.id);panel(6,y,308,25);tft.fillCircle(17,y+12,4,qColor(q));
  txt(fitName(n,134-28-4),28,y+4,2,cWHITE,cPANEL);txt(String(n.rssi),138,y+4,2,qColor(q),cPANEL);txt(String(n.snr,1),195,y+4,2,cTURQ,cPANEL);txt(String((millis()-n.last)/1000)+"s",257,y+4,2,cDIM,cPANEL);}
 if(!orderCount){panel(6,70,308,118);txt("Czekam na dane z Trackera...",16,78,2,cWHITE,cPANEL);
  txt(String("Baud ")+String((unsigned long)baudNow)+"  RX "+String((unsigned long)rxBytes)+" B",16,100,2,cTURQ,cPANEL);
  txt(String("Ramki ")+String((unsigned long)frames)+"  Bledy "+String((unsigned long)framing.errors),16,120,2,cDIM,cPANEL);
  const char *h1,*h2;if(!rxBytes){h1="Brak bajtow z Trackera. Sprawdz: TX(GPIO6)->IO35,";h2="RX(GPIO7)<-IO27, wspolna masa; serial.mode PROTO";}else if(!frames){h1="Sa bajty, ale brak poprawnych ramek.";h2="Zly baud lub szum - skanuje predkosci...";}else{h1="Polaczono z Trackerem (API OK).";h2="Czekam na pakiety od nodow w sieci.";}
  txt(h1,16,146,1,cAMBER,cPANEL);txt(h2,16,160,1,cAMBER,cPANEL);}}
void draw(){bool alive=everFrame&&(millis()-lastFrame<90000);int screen=(detail&&selected>=0&&nodes[selected].count)?1:0;
 if(screen!=uiScreen){tft.fillScreen(cBG);uiScreen=screen;}
 drawHeader(alive);drawGps();if(screen)drawDetail(nodes[selected]);else drawList();drawNav(screen==1);
 Serial.printf("baud=%lu rx=%lu frames=%lu ",(unsigned long)baudNow,(unsigned long)rxBytes,(unsigned long)frames);Serial.printf("API=%d nodes=%u csv=%lu frame_errors=%u parse_errors=%lu log_errors=%lu\n",configured,orderCount,(unsigned long)written,framing.errors,(unsigned long)parseErrors,(unsigned long)logErrors);
}
void setup(){Serial.begin(115200);pinMode(BACKLIGHT,OUTPUT);digitalWrite(BACKLIGHT,HIGH);pinMode(TOUCH_CS,OUTPUT);digitalWrite(TOUCH_CS,HIGH);pinMode(TOUCH_CLK,OUTPUT);digitalWrite(TOUCH_CLK,LOW);pinMode(TOUCH_MOSI,OUTPUT);pinMode(TOUCH_MISO,INPUT);pinMode(0,INPUT_PULLUP);
 tft.init();tft.setRotation(1);prefs.begin("touch",false);invertScr=prefs.getBool("inv",false);tft.invertDisplay(invertScr);splash();delay(1800);baudNow=prefs.getULong("baud",RADIO_BAUD);radio.setRxBufferSize(4096);radio.begin(baudNow,SERIAL_8N1,RADIO_RX,RADIO_TX);baudSince=millis();calX0=prefs.getUShort("x0",300);calX1=prefs.getUShort("x1",3800);calY0=prefs.getUShort("y0",300);calY1=prefs.getUShort("y1",3800);calSwap=prefs.getBool("swap",true);
 sdReady=sdMount();if(!prefs.getBool("valid",false))calibrate();requestConfig();draw();}
void loop(){if(Serial.available()){char cmd=Serial.read();if(cmd=='l')toggleLog();if(cmd=='c')calibrate();if(cmd=='t'){clockSeen(uint32_t(Serial.parseInt()));}if(cmd=='r'){configured=false;requestConfig();}}while(radio.available()){framing.feed(radio.read(),millis(),fromRadio);++rxBytes;}uint32_t now=millis();
 if(!everFrame){if(now-baudSince>8000)nextBaud(now);else if(now-lastRequest>3500)requestConfig();}
 static bool baudSaved=false;if(configured&&!baudSaved){baudSaved=true;if(prefs.getULong("baud",0)!=baudNow)prefs.putULong("baud",baudNow);}if((!configured&&now-lastRequest>15000)||(everFrame&&now-lastFrame>90000&&now-lastRequest>90000)){configured=false;requestConfig();}
 if(configured&&now-lastHeartbeat>30000){const uint8_t hb[]={0x3a,0};sendFrame(hb,sizeof(hb));lastHeartbeat=now;}
 static bool pressed=false;static uint32_t pressedAt=0;int x,y;if(touch(x,y)){if(!pressed&&now-pressedAt>250){pressed=true;pressedAt=now;if(y<22){invertScr=!invertScr;prefs.putBool("inv",invertScr);tft.invertDisplay(invertScr);uiScreen=-1;}else if(y>=212){if(x<100){if(detail)detail=false;else page=(page+1)%max(1U,(orderCount+4)/5);}else if(x<247)toggleLog();else calibrate();}else if(!detail&&y>=70&&y<207){unsigned row=(y-70)/27;if(row<5&&page*5+row<orderCount){selected=order[page*5+row];detail=true;probeStart(nodes[selected].id);}}else if(detail&&x<164&&y>=70&&y<151){probe.on=!probe.on;if(probe.on)probe.nextAt=now;}lastDraw=0;}}else pressed=false;
 // BOOT short press changes page; long press starts/stops logging (useful without touch).
 static uint32_t bootAt=0;static bool bootHeld=false;if(!digitalRead(0)){if(!bootHeld){bootHeld=true;bootAt=now;}}else if(bootHeld){bootHeld=false;if(now-bootAt>1000)toggleLog();else{detail=false;page=(page+1)%max(1U,(orderCount+4)/5);}lastDraw=0;}
 if(logging&&now-lastFlush>2000){csv.flush();lastFlush=now;if(csv.getWriteError()){++logErrors;logging=false;csv.close();flash("BLAD SD");}}
 probeTick(now);
 if(!lastDraw||dirty||now-lastDraw>=(detail?500u:1000u)){dirty=false;draw();lastDraw=now;}delay(2);}
