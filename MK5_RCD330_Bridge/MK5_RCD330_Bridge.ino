/*
  MK5 RCD330 Bridge / ESP32 WROOM / TWAI + MCP2515
  v1.0.0 - 2026-11-08

  Objetivo: informacoes REAIS de radio/midia no painel vermelho, BAP -> DDP.
  Prototipo: compilacao verificada em computador; nao validado fisicamente
  na sua RCD330/carro. GETs BAP nao garantem que toda RCD330 exporte textos.
  Nenhum titulo ficticio e apresentado como dado da radio. A interface
  distingue fonte, metadados recebidos e confirmacoes DDP.

  Arduino IDE: pacote esp32 por Espressif Systems 3.3.12.
  Placa ESP32 Dev Module, flash 4 MB, particionamento Default COM OTA.
  Somente bibliotecas incluidas no pacote ESP32. Sem biblioteca MCP externa.
  Salve este arquivo na pasta MK5_RCD330_Bridge.

  Pinos:
    TWAI D4/D5 preservados. Padrao TX=D4, RX=D5. Se seus fios estiverem
    TX=D5/RX=D4, mude a opcao na pagina web, sem trocar os fios.
    MCP2515: SCK=D18, SO/MISO=D19, SI/MOSI=D23, CS=D13, INT=D14.
    D2/D12 nao utilizados. SPI nao usa o CS padrao D5.
    TWAI no carro; MCP2515 + transceptor na radio. Ambos 100 kbit/s.
    Cristal MCP configuravel 8/16 MHz; padrao 8 MHz, nao autodetectado.
    Separe apenas CANH/CANL da radio numa extensao Quadlock; nao una
    os segmentos. A ponte encaminha os quadros nos dois sentidos.
    GND comum. GPIO ESP32 NAO tolera 5 V. Modulos MCP2515/TJA1050 de
    alimentacao unica podem precisar adaptacao eletrica de SPI/INT.
    Step-down em 5 V para VIN da placa de desenvolvimento, nunca 12 V.
    Conferir a terminacao e a camada fisica antes de conectar ao carro.

  Wi-Fi SEMPRE LIGADO, SSID OCULTO (adicionar rede manualmente, WPA2):
    Rede: MK5-RCD330           Senha: Mk5Radio!330
    Pagina: http://192.168.4.1
    Usuario: admin            Senha web: PainelMK5!330
    Altere as constantes abaixo antes da primeira gravacao, se desejar.
    Sem deep-sleep: medir consumo acrescentado pela ponte instalada.

  Atualizacao:
    Primeira gravacao por USB, com particao OTA.
    Depois: Arduino IDE -> Sketch -> Export Compiled Binary.
    Conectar a rede oculta e enviar o .ino.bin da APLICACAO pela pagina.
    Nao enviar merged/bootloader/partitions.bin. Nao atualiza a RCD330.
    A ponte e pausada no upload; a radio fica temporariamente sem CAN.
    Sucesso reinicia o ESP32; falha retoma a ponte. Sem internet/CDN.
    A entrega deste projeto e somente este .ino.

  Protocolo:
    RCD330 continua 56-Radio; nao sao feitos coding/adaptacoes/diagnostico 5F.
    Audio BAP LSG=0x31, funcao inicial 0x15, configuravel.
    RX de audio em 0x660..0x66F, normalmente 0x66C.
    GETs limitados nas rotas candidatas 0x6C9/0x6C8/0x67C, so na radio.
    A rota pode ser fixada na web. Respostas correlacionadas sao evidencia,
    nao homologacao de todos os firmwares RCD330.
    DDP: TX=0x680 RX=0x681 device=0x52; pagina Audio, duas linhas com rolagem.
    Respeita janela, sequencias, ACKs, busy e disponibilidade da area.
    Volume/volante/brilho passam intactos. Titulos ficam so em RAM.

  Referencias:
    https://github.com/tehfalcon-vw/MIB2-Can-Bridge
    https://github.com/norly/revag-docs/blob/master/notes/bap.txt
    https://github.com/unemployable/Golf-Mk5-RCD330-CAN-Filter
    https://pccar.ru/showthread.php?nojs=1&t=24102
    https://www.microchip.com/en-us/product/mcp2515
*/
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <Preferences.h>
#include <SPI.h>
#include <driver/twai.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <ctype.h>
#include <new>
#include <atomic>

namespace MK5 {
constexpr char VERSION[]="1.0.0";
constexpr char AP_SSID[]="MK5-RCD330", AP_PASS[]="Mk5Radio!330";
constexpr char WEB_USER[]="admin", WEB_PASS[]="PainelMK5!330";
constexpr uint8_t SCK_PIN=18,MISO_PIN=19,MOSI_PIN=23,CS_PIN=13,INT_PIN=14;
constexpr uint16_t DDP_TX=0x680,DDP_RX=0x681;
constexpr size_t BAP_MAX=512,TRACE_COUNT=192;
constexpr uint32_t IDLE_MS=10000,BOOT_DDP_MS=20000;
struct Frame {uint32_t id=0;uint8_t len=0,flags=0,data[8]={};};
// flags: bit 0 extended, bit 1 RTR
struct Settings {
  uint32_t magic=0x4D4B3501;
  bool swap=false,client=true,panel=true;
  uint8_t crystal=8,textFunction=0x15;
  uint16_t requestId=0;
};
struct Audio {
  char top[65]={},bottom[65]={},source[33]={};
  bool valid=false,metadata=false;
  uint32_t updated=0,changed=0,generation=0;
};
struct Counters {
  uint32_t vrx=0,rrx=0,vtx=0,rtx=0,drop=0,expired=0,txError=0,mcpOverflow=0;
  uint32_t nativeMissed=0,busError=0,recoveries=0;
  uint32_t bap=0,bapInvalid=0,bapErrors=0,requests=0,text=0;
  uint32_t ack=0,busy=0,badAck=0,draw=0,timeout=0;
};
struct Trace {uint32_t ms=0;Frame f;uint8_t bus=0;};
struct Snapshot {
  Audio audio;Counters count;
  bool twai=false,mcp=false,active=false,area=false;
  uint8_t ddp=0,native=0,mcpFlags=0,function=0,opcode=0;
  uint16_t route=0;char packet[161]={};
};
struct Pending {Frame f;uint32_t at=0;bool local=false;};
template<size_t N> class FrameQueue {
  Pending q[N];size_t head=0,tail=0,used=0;
 public:
  bool push(const Frame& f,uint32_t now,bool local=false){
    if(used==N)return false;
    q[tail].f=f;q[tail].at=now;q[tail].local=local;
    tail=(tail+1)%N;++used;return true;
  }
  Pending* front(){return used?&q[head]:nullptr;}
  void pop(){if(used){head=(head+1)%N;--used;}}
  void clear(){head=tail=used=0;}
};
static Settings cfg;
static Audio audio;
static Counters count;
static Snapshot published;
static Trace traces[TRACE_COUNT];
static size_t traceHead=0,traceUsed=0;
static portMUX_TYPE mux=portMUX_INITIALIZER_UNLOCKED;
static std::atomic<bool> parkRequest{false},parkReady{false},retryRequest{false};
static TaskHandle_t canTaskHandle=nullptr;
static bool twaiOk=false,mcpOk=false,active=false,runSeen=false;
static uint32_t lastVehicle=0,lastRun=0,activeSince=0;
static FrameQueue<256> toVehicle,toRadio;
static FrameQueue<16> radioLocal;
static uint8_t lastFunction=0,lastOpcode=0;
static char lastPacket[161]={};
static bool elapsed(uint32_t n,uint32_t t,uint32_t p){return uint32_t(n-t)>=p;}
static bool due(uint32_t n,uint32_t t){return int32_t(n-t)>=0;}
static void record(const Frame& f,uint8_t bus,uint32_t now){
  if(!((f.id>=0x660&&f.id<=0x66F)||f.id==DDP_TX||f.id==DDP_RX||f.id==0x6C8||f.id==0x6C9||f.id==0x67C))return;
  portENTER_CRITICAL(&mux);
  traces[traceHead].ms=now;traces[traceHead].f=f;traces[traceHead].bus=bus;
  traceHead=(traceHead+1)%TRACE_COUNT;if(traceUsed<TRACE_COUNT)++traceUsed;
  portEXIT_CRITICAL(&mux);
}

// Driver MCP2515 autocontido. Somente canTask acessa SPI.
class Mcp2515 {
  SPIClass spi{VSPI};bool pending=false;uint32_t started=0;
  void start(){spi.beginTransaction(SPISettings(4000000,MSBFIRST,SPI_MODE0));digitalWrite(CS_PIN,LOW);}
  void stop(){digitalWrite(CS_PIN,HIGH);spi.endTransaction();}
 public:
  uint8_t reg(uint8_t a){start();spi.transfer(3);spi.transfer(a);uint8_t v=spi.transfer(0);stop();return v;}
  void write(uint8_t a,uint8_t v){start();spi.transfer(2);spi.transfer(a);spi.transfer(v);stop();}
  void bits(uint8_t a,uint8_t mask,uint8_t v){start();spi.transfer(5);spi.transfer(a);spi.transfer(mask);spi.transfer(v);stop();}
  bool mode(uint8_t m){
    bits(0x0F,0xE0,m);
    for(uint8_t i=0;i<20;++i){if((reg(0x0E)&0xE0)==m)return true;delay(1);}
    return false;
  }
  bool begin(uint8_t mhz){
    pinMode(CS_PIN,OUTPUT);digitalWrite(CS_PIN,HIGH);pinMode(INT_PIN,INPUT_PULLUP);
    spi.begin(SCK_PIN,MISO_PIN,MOSI_PIN,CS_PIN);
    start();spi.transfer(0xC0);stop();delay(10);pending=false;
    if(!mode(0x80))return false;
    write(0x2A,mhz==16?3:1);write(0x29,0xB4);write(0x28,0x86);
    for(uint8_t a=0;a<=0x0B;++a)write(a,0);
    for(uint8_t a=0x10;a<=0x1B;++a)write(a,0);
    for(uint8_t a=0x20;a<=0x27;++a)write(a,0);
    write(0x60,0x64);write(0x70,0x60); // receive all + rollover
    write(0x2C,0);write(0x2B,0x27); // RX0/RX1/TX0/error IRQ
    return mode(0) && reg(0x2A)==(mhz==16?3:1);
  }
  bool read(Frame& f){
    uint8_t flags=reg(0x2C);if(!(flags&3))return false;
    bool second=!(flags&1);uint8_t ctrl=reg(second?0x70:0x60),b[13];
    start();spi.transfer(second?0x94:0x90);
    for(uint8_t& v:b)v=spi.transfer(0);stop();
    // READ RX BUFFER limpa apenas RXnIF ao desativar CS; nao limpar novamente.
    f=Frame{};f.len=b[4]&15;
    if(f.len>8){++count.mcpOverflow;return false;}
    f.id=(uint32_t(b[0])<<3)|(b[1]>>5);
    if(b[1]&8){f.flags|=1;f.id=(f.id<<18)|(uint32_t(b[1]&3)<<16)|(uint32_t(b[2])<<8)|b[3];}
    if(ctrl&8)f.flags|=2;
    if(!(f.flags&2))memcpy(f.data,b+5,f.len);return true;
  }
  bool send(const Frame& f,uint32_t now){
    if(pending||(reg(0x30)&8)||(reg(0x2D)&0x20))return false;
    uint8_t b[13]={};
    if(f.flags&1){b[0]=f.id>>21;b[1]=((f.id>>13)&0xE0)|8|((f.id>>16)&3);b[2]=f.id>>8;b[3]=f.id;}
    else{b[0]=f.id>>3;b[1]=(f.id&7)<<5;}
    b[4]=f.len|((f.flags&2)?0x40:0);if(!(f.flags&2))memcpy(b+5,f.data,f.len);
    bits(0x30,0x70,0);bits(0x2C,4,0);
    start();spi.transfer(0x40);for(uint8_t v:b)spi.transfer(v);stop();
    start();spi.transfer(0x81);stop();pending=true;started=now;return true;
  }
  int completion(uint32_t now){
    if(!pending)return 0;uint8_t v=reg(0x30);
    if(v&8){if(elapsed(now,started,250)){bits(0x30,8,0);pending=false;return -1;}return 0;}
    pending=false;bits(0x2C,4,0);return(v&0x50)?-1:1;
  }
  uint8_t errors(){return reg(0x2D);}
  void clearErrors(){if(errors()&0xC0){++count.mcpOverflow;bits(0x2D,0xC0,0);}bits(0x2C,0xA0,0);}
  void pause(){bits(0x30,8,0);pending=false;mode(0x60);}
};
static Mcp2515 mcp;

struct BapPacket {uint32_t id=0;uint8_t opcode=0,function=0;uint16_t len=0;uint8_t data[BAP_MAX]={};};
class BapAssembler {
  struct Channel {
    bool active=false;uint32_t id=0,at=0;uint16_t len=0,used=0;
    uint8_t channel=0,next=0,opcode=0,function=0,data[BAP_MAX]={};
  } ch[4];
 public:
  void reset(){for(auto& c:ch)c.active=false;}
  void expire(uint32_t now){for(auto& c:ch)if(c.active&&elapsed(now,c.at,1000)){c.active=false;++count.bapInvalid;}}
  bool feed(const Frame& f,uint32_t now,BapPacket& out){
    if((f.flags&3)||f.len<2||f.id<0x660||f.id>0x66F)return false;
    uint8_t first=f.data[0];
    if(!(first&0x80)){
      uint16_t h=(uint16_t(first)<<8)|f.data[1];if(((h>>6)&63)!=49)return false;
      out.id=f.id;out.opcode=(h>>12)&7;out.function=h&63;out.len=f.len-2;
      memcpy(out.data,f.data+2,out.len);return true;
    }
    uint8_t channel=(first>>4)&3;Channel* c=nullptr;
    for(auto& x:ch)if(x.active&&x.id==f.id&&x.channel==channel){c=&x;break;}
    if(!(first&0x40)){
      if(c)c->active=false;if(f.len<4){++count.bapInvalid;return false;}
      uint16_t h=(uint16_t(f.data[2])<<8)|f.data[3];if(((h>>6)&63)!=49)return false;
      uint16_t len=(uint16_t(first&15)<<8)|f.data[1];
      if(len>BAP_MAX||f.len-4>len){++count.bapInvalid;return false;}
      if(!c)for(auto& x:ch)if(!x.active){c=&x;break;}
      if(!c){++count.bapInvalid;return false;}
      c->active=true;c->id=f.id;c->channel=channel;c->opcode=(h>>12)&7;c->function=h&63;
      c->len=len;c->used=f.len-4;c->next=0;c->at=now;memcpy(c->data,f.data+4,c->used);
    }else{
      if(!c){++count.bapInvalid;return false;}
      if((first&15)!=c->next||elapsed(now,c->at,1000)||c->used+f.len-1>c->len){c->active=false;++count.bapInvalid;return false;}
      memcpy(c->data+c->used,f.data+1,f.len-1);c->used+=f.len-1;c->next=(c->next+1)&15;c->at=now;
    }
    if(c->used!=c->len)return false;
    out.id=c->id;out.opcode=c->opcode;out.function=c->function;out.len=c->len;
    memcpy(out.data,c->data,c->len);c->active=false;return true;
  }
};
static BapAssembler bap;
static BapPacket bapPacket;

static void cleanText(const uint8_t* s,size_t n,char* out,size_t cap){
  size_t w=0;
  for(size_t i=0;i<n&&w+1<cap;++i){
    uint8_t c=s[i];if(c>=32&&c<=126){out[w++]=c;continue;}
    uint16_t cp=c;
    if(c==0xC3&&i+1<n&&(s[i+1]&0xC0)==0x80)cp=0xC0+(s[++i]&63);
    else if(c>=0xC0&&c<=0xF7&&i+1<n&&(s[i+1]&0xC0)==0x80){while(i+1<n&&(s[i+1]&0xC0)==0x80)++i;continue;}
    char a=0;
    if(cp>=0xC0&&cp<=0xC5)a='A';else if(cp>=0xE0&&cp<=0xE5)a='a';
    else if(cp>=0xC8&&cp<=0xCB)a='E';else if(cp>=0xE8&&cp<=0xEB)a='e';
    else if(cp>=0xCC&&cp<=0xCF)a='I';else if(cp>=0xEC&&cp<=0xEF)a='i';
    else if(cp>=0xD2&&cp<=0xD6)a='O';else if(cp>=0xF2&&cp<=0xF6)a='o';
    else if(cp>=0xD9&&cp<=0xDC)a='U';else if(cp>=0xF9&&cp<=0xFC)a='u';
    else if(cp==0xC7)a='C';else if(cp==0xE7)a='c';else if(cp==0xD1)a='N';else if(cp==0xF1)a='n';
    if(a)out[w++]=a;
  }
  out[w]=0;while(w&&out[w-1]==' ')out[--w]=0;
  size_t start=0;while(out[start]==' ')++start;if(start)memmove(out,out+start,w-start+1);
}
static bool containsNoCase(const char* s,const char* needle){
  for(size_t i=0;s[i];++i){size_t j=0;while(needle[j]&&s[i+j]&&tolower((unsigned char)s[i+j])==tolower((unsigned char)needle[j]))++j;if(!needle[j])return true;}return false;
}
static bool sourceText(const char* s){
  return containsNoCase(s,"carplay")||containsNoCase(s,"android auto")||containsNoCase(s,"bluetooth")||containsNoCase(s,"bt audio")||
    !strcasecmp(s,"USB")||!strcasecmp(s,"SD")||!strcasecmp(s,"AUX")||!strcasecmp(s,"MEDIA")||!strcasecmp(s,"IPOD");
}
static bool frequencyText(const char* s){
  bool digit=false;size_t len=strlen(s);if(!len||len>20)return false;
  for(size_t i=0;i<len;++i){unsigned char c=s[i];if(isdigit(c)){digit=true;continue;}
    if(c!='.'&&c!=','&&c!=' '&&c!='M'&&c!='m'&&c!='H'&&c!='h'&&c!='z'&&c!='Z'&&c!='F'&&c!='f'&&c!='A'&&c!='a'&&c!='k'&&c!='K')return false;}
  return digit;
}
static void setAudio(const char* top,const char* bottom,const char* source,bool metadata,uint32_t now){
  if(!top[0]&&!bottom[0])return;
  bool changed=strcmp(audio.top,top)||strcmp(audio.bottom,bottom)||strcmp(audio.source,source)||audio.metadata!=metadata;
  strlcpy(audio.top,top,sizeof(audio.top));strlcpy(audio.bottom,bottom,sizeof(audio.bottom));strlcpy(audio.source,source,sizeof(audio.source));
  audio.valid=true;audio.metadata=metadata;audio.updated=now;
  if(changed){audio.changed=now;++audio.generation;++count.text;}
}
static void parseAudio(const BapPacket& p,uint32_t now){
  if(p.opcode!=4||p.function!=cfg.textFunction||!p.len)return;
  char field[3][65]={};uint16_t offset=0;bool schema=true;
  for(uint8_t i=0;i<3&&offset<p.len;++i){
    uint8_t n=p.data[offset++];if(offset+n>p.len){schema=false;break;}
    cleanText(p.data+offset,n,field[i],sizeof(field[i]));offset+=n;
    if(offset+3==p.len&&p.data[offset]==1&&p.data[offset+1]==0&&p.data[offset+2]==0)break;
  }
  if(schema&&(field[0][0]||field[1][0]||field[2][0])){
    if(frequencyText(field[0]))setAudio(field[1][0]?field[1]:"RADIO",field[0],"FM/AM",true,now);
    else if(field[1][0])setAudio(field[1],field[2][0]?field[2]:field[0],sourceText(field[0])?field[0]:"MIDIA",true,now);
    else setAudio(field[0],"",sourceText(field[0])?field[0]:"RADIO",!sourceText(field[0]),now);
    return;
  }
  // Fallback restrito a funcao de texto, nunca a Config/FunctionList.
  size_t bestStart=0,bestLen=0,start=0,len=0;
  for(uint16_t i=0;i<p.len;++i){uint8_t c=p.data[i];if(c>=32&&c<=126){if(!len)start=i;++len;if(len>bestLen){bestLen=len;bestStart=start;}}else len=0;}
  if(bestLen<2)return;char text[65];cleanText(p.data+bestStart,bestLen,text,sizeof(text));
  if(frequencyText(text))setAudio("RADIO",text,"FM/AM",true,now);
  else if(sourceText(text))setAudio(text,"",text,false,now);
  else setAudio(text,"","CAN (layout a confirmar)",false,now);
}

class BapClient {
  const uint16_t candidates[3]={0x6C9,0x6C8,0x67C};
  uint8_t candidate=0,step=0,attempts=0,lastRequested=0;
  uint16_t selected=0,learned=0;bool locked=false,wasActive=false;
  uint32_t next=0,routeStarted=0,lastRequest=0,lastExternal=0;
 public:
  uint16_t route()const{return learned?learned:(selected?selected:(cfg.requestId?cfg.requestId:candidates[candidate]));}
  void reset(uint32_t now){candidate=step=attempts=0;selected=learned=0;locked=false;next=now+2000;routeStarted=next;lastRequest=lastExternal=0;}
  void observeVehicle(const Frame& f,uint32_t now){
    if((f.flags&3)||f.len<2||(f.id!=0x6C8&&f.id!=0x6C9&&f.id!=0x67C))return;
    uint16_t h;
    if(!(f.data[0]&0x80))h=(uint16_t(f.data[0])<<8)|f.data[1];
    else if(!(f.data[0]&0x40)&&f.len>=4)h=(uint16_t(f.data[2])<<8)|f.data[3];else return;
    if(((h>>6)&63)==49){learned=selected=f.id;locked=true;lastExternal=now;}
  }
  void response(const BapPacket& p,uint32_t now){
    if(p.opcode==0){step=0;next=now+500;return;}
    if(lastRequest&&!elapsed(now,lastRequest,1800)&&p.function==lastRequested&&p.opcode==4){selected=route();locked=true;}
  }
  void tick(uint32_t now){
    if(!active||!cfg.client){wasActive=false;return;}
    if(!wasActive){reset(now);wasActive=true;}
    if(!mcpOk||!due(now,next)||(lastExternal&&!elapsed(now,lastExternal,5000)))return;
    if(!locked&&!cfg.requestId&&elapsed(now,routeStarted,8000)){
      candidate=(candidate+1)%3;step=0;routeStarted=now;
      if(++attempts>=3){attempts=0;next=now+60000;routeStarted=next;return;}
    }
    uint8_t function=step==0?2:(step==1?3:cfg.textFunction);
    Frame f;f.id=route();f.len=2;uint16_t h=0x1000|(49<<6)|function;f.data[0]=h>>8;f.data[1]=h;
    if(radioLocal.push(f,now,true)){++count.requests;lastRequest=now;lastRequested=function;if(step<2)++step;next=now+(step<2?500:2000);}else next=now+100;
  }
};
static BapClient client;

// Transporte DDP. Apenas uma mensagem logica pendente; texto novo e coalescido.
class Ddp {
 public:
  enum Phase:uint8_t {Off,Connect,Identify,DisplayParams,Area,AreaParams,Bind,Bound,Show,Ready,Backoff};
 private:
  enum Purpose:uint8_t {None,IdentifyPayload,AreaPayload,BindPayload,ShowPayload,DrawPayload};
  Frame control[8];uint8_t cHead=0,cTail=0,cUsed=0;
  uint8_t tx[96]={},rx[96]={};size_t txLen=0,txOffset=0,rxLen=0;
  uint8_t txSeq=0,rxSeq=0,expectedAck=0,blockCount=0,window=4;
  bool transport=false,waitingAck=false,inFlight=false,pendingAck=false;
  uint8_t pendingChunk=0;
  uint32_t nextFragment=0,ackDeadline=0,phaseAt=0,retryAt=0,lastRx=0,lastControl=0,lastRxData=0;
  uint32_t interFrame=5,ackTimeout=1000,drawGeneration=0,lastScroll=0;
  bool connected=false,available=false,haveDisplay=false,haveArea=false,initialShow=false;
  Purpose purpose=None;Phase phase=Off;uint32_t revision=0;
  char renderedTop[17]={},renderedBottom[17]={};
  void change(Phase p,uint32_t now){phase=p;phaseAt=now;}
  static uint32_t timeValue(uint8_t b){
    static const uint32_t unit[4]={100,1000,10000,100000};
    return ((b&63)*unit[b>>6]+999)/1000;
  }
  void controlFrame(const uint8_t* b,uint8_t len){
    if(cUsed==8){++count.drop;return;}
    Frame& f=control[cTail];f=Frame{};f.id=DDP_TX;f.len=len;memcpy(f.data,b,len);
    cTail=(cTail+1)%8;++cUsed;
  }
  void ack(uint8_t n){uint8_t b=0xB0|(n&15);controlFrame(&b,1);}
  void beginPayload(const uint8_t* b,size_t len,Purpose p,uint32_t now){
    if(transport||len>sizeof(tx))return;
    memcpy(tx,b,len);txLen=len;txOffset=0;blockCount=0;purpose=p;
    transport=true;waitingAck=false;nextFragment=now;
  }
  void identify(uint32_t now){const uint8_t b[]={0,0x52,0,0xFF};change(Identify,now);beginPayload(b,sizeof(b),IdentifyPayload,now);}
  void areaRequest(uint32_t now){const uint8_t b[]={1,0x12};change(Area,now);beginPayload(b,sizeof(b),AreaPayload,now);}
  void bind(uint32_t now){const uint8_t b[]={2,0x70,0x52,0x12,'A','u','d','i','o'};change(Bind,now);beginPayload(b,sizeof(b),BindPayload,now);}
  void show(uint32_t now){const uint8_t b[]={0x0C,0};change(Show,now);beginPayload(b,sizeof(b),ShowPayload,now);}
  void done(uint32_t now){
    Purpose p=purpose;purpose=None;transport=false;waitingAck=false;
    if(p==IdentifyPayload){change(DisplayParams,now);if(haveDisplay)areaRequest(now);}
    else if(p==AreaPayload){change(AreaParams,now);if(haveArea)bind(now);}
    else if(p==BindPayload){change(Bound,now);initialShow=true;}
    else if(p==ShowPayload)change(Ready,now);
    else if(p==DrawPayload){++count.draw;change(Ready,now);}
  }
  void payload(uint32_t now){
    if(!rxLen)return;uint8_t command=rx[0];
    if(command==0x20){haveDisplay=true;if(phase==DisplayParams&&!transport)areaRequest(now);}
    else if(command==0x21){haveArea=true;if(phase==AreaParams&&!transport)bind(now);}
    else if(command==0x23&&rxLen>=4&&rx[1]==0){
      bool before=available;available=rx[2]==1;
      if(available&&!before){renderedTop[0]=renderedBottom[0]=0;drawGeneration=0;}
    }
  }
  static void visible(const char* text,char* out,uint32_t now){
    size_t n=strlen(text);if(n<=16){strlcpy(out,text,17);return;}
    uint32_t age=uint32_t(now-audio.changed);
    size_t start=age<1800?0:((age-1800)/450)%(n+3);
    for(size_t i=0;i<16;++i){size_t p=(start+i)%(n+3);out[i]=p<n?text[p]:' ';}out[16]=0;
  }
  static size_t textCommand(uint8_t* b,size_t pos,uint8_t y,const char* s){
    size_t n=strlen(s);
    b[pos++]=0x61;b[pos++]=6+n;b[pos++]=0;b[pos++]=0;b[pos++]=3;b[pos++]=0;b[pos++]=y;b[pos++]=0;
    memcpy(b+pos,s,n);return pos+n;
  }
  void draw(uint32_t now){
    char top[17],bottom[17];visible(audio.top,top,now);visible(audio.bottom,bottom,now);
    if(drawGeneration==audio.generation&&!strcmp(top,renderedTop)&&!strcmp(bottom,renderedBottom))return;
    uint8_t b[80]={9,0,0x60,9,0,0,0,0,0,0x6E,0,0x5B,0};size_t n=13;
    n=textCommand(b,n,0x18,top);n=textCommand(b,n,0x34,bottom);b[n++]=8;
    strlcpy(renderedTop,top,17);strlcpy(renderedBottom,bottom,17);drawGeneration=audio.generation;
    beginPayload(b,n,DrawPayload,now);
  }
 public:
  Phase state()const{return phase;}
  uint32_t epoch()const{return revision;}
  bool areaAvailable()const{return available;}
  void reset(uint32_t now,uint32_t backoff=2000){
    ++revision;
    cHead=cTail=cUsed=0;transport=waitingAck=inFlight=pendingAck=false;purpose=None;
    txLen=txOffset=rxLen=0;txSeq=rxSeq=0;
    connected=available=haveDisplay=haveArea=initialShow=false;
    renderedTop[0]=renderedBottom[0]=0;drawGeneration=0;lastRx=now;
    retryAt=now+backoff;change(Backoff,now);
  }
  void stop(uint32_t now){reset(now,0);change(Off,now);}
  void wireComplete(uint8_t tag,uint32_t now){
    inFlight=false;lastControl=now;
    if(tag!=1)return;
    txOffset+=pendingChunk;txSeq=(txSeq+1)&15;++blockCount;
    nextFragment=now+interFrame;
    if(pendingAck){waitingAck=true;expectedAck=txSeq;ackDeadline=now+ackTimeout;}
  }
  bool nextFrame(Frame& f,uint8_t& tag,uint32_t now){
    if(inFlight)return false;
    if(cUsed){f=control[cHead];cHead=(cHead+1)%8;--cUsed;tag=0;inFlight=true;return true;}
    if(!transport||waitingAck||!due(now,nextFragment))return false;
    pendingChunk=min(size_t(7),txLen-txOffset);
    bool last=txOffset+pendingChunk==txLen;pendingAck=last||blockCount+1>=window;
    uint8_t type=last?0x10:(pendingAck?0x00:0x20);
    f=Frame{};f.id=DDP_TX;f.len=pendingChunk+1;f.data[0]=type|txSeq;
    memcpy(f.data+1,tx+txOffset,pendingChunk);tag=1;inFlight=true;return true;
  }
  void receive(const Frame& f,uint32_t now){
    if((f.flags&3)||!f.len||phase==Off||phase==Backoff)return;
    lastRx=now;uint8_t c=f.data[0],hi=c&0xF0,seq=c&15;
    if(c==0xA1){
      if(phase==Connect){
        window=f.len>=2?constrain(f.data[1],1,15):4;
        interFrame=f.len>=5?constrain(timeValue(f.data[4]),uint32_t(5),uint32_t(6300)):5;
        ackTimeout=f.len>=3?constrain(timeValue(f.data[2])*4,uint32_t(400),uint32_t(25200)):1000;
        connected=true;txSeq=rxSeq=0;lastControl=now;identify(now);
      }return;
    }
    if(c==0xA3){const uint8_t b[]={0xA1,4,0x8A,0xFF,0x32,0xFF};controlFrame(b,sizeof(b));return;}
    if(c==0xA8||c==0xA4){reset(now);return;}
    if(hi==0xB0){
      ++count.ack;
      if(!waitingAck||seq!=expectedAck){++count.badAck;return;}
      waitingAck=false;blockCount=0;nextFragment=now+interFrame;
      if(txOffset==txLen)done(now);return;
    }
    // Busy nao e conclusao de desenho: aguarda ACK pronto; timeout reconecta.
    if(hi==0x90){++count.busy;if(waitingAck)ackDeadline=now+ackTimeout;return;}
    if(hi==0||hi==0x10||hi==0x20||hi==0x30){
      bool ackRequired=hi==0||hi==0x10,last=hi==0x10||hi==0x30;
      if(seq!=rxSeq){if(ackRequired)ack(rxSeq);return;}
      if(rxLen+f.len-1>sizeof(rx)){++count.timeout;reset(now);return;}
      memcpy(rx+rxLen,f.data+1,f.len-1);rxLen+=f.len-1;
      rxSeq=(rxSeq+1)&15;lastRxData=now;if(ackRequired)ack(rxSeq);
      if(last){payload(now);rxLen=0;}
    }
  }
  void tick(uint32_t now){
    if(!active||!cfg.panel||!audio.valid){if(phase!=Off)stop(now);return;}
    if(phase==Off)reset(now,0);
    if(phase==Backoff){
      if(!due(now,retryAt)||!elapsed(now,activeSince,BOOT_DDP_MS))return;
      const uint8_t b[]={0xA0,4,0x59,0xFF,0x32,0xFF};controlFrame(b,sizeof(b));change(Connect,now);return;
    }
    if(waitingAck&&due(now,ackDeadline)){++count.timeout;reset(now);return;}
    if(rxLen&&elapsed(now,lastRxData,1000)){++count.timeout;reset(now);return;}
    if(connected&&elapsed(now,lastRx,5000)){++count.timeout;reset(now);return;}
    if(phase!=Ready&&phase!=Bound&&!transport&&elapsed(now,phaseAt,2000)){++count.timeout;reset(now);return;}
    if(connected&&elapsed(now,lastControl,1000)&&!cUsed){uint8_t b=0xA3;controlFrame(&b,1);lastControl=now;}
    if(phase==Bound&&initialShow&&!transport&&!inFlight){initialShow=false;show(now);return;}
    // Depois de sair da pagina Audio, nunca forca retorno periodico.
    if(phase==Ready&&available&&!transport&&!inFlight&&elapsed(now,lastScroll,200)){lastScroll=now;draw(now);}
  }
};
static Ddp ddp;
static Frame reserved;
static bool reservedPending=false,reservedSent=false;
static uint8_t reservedTag=0;
static uint32_t reservedAt=0;
static uint32_t reservedEpoch=0;
static void IRAM_ATTR mcpInterrupt(){
  BaseType_t wake=pdFALSE;
  if(canTaskHandle)vTaskNotifyGiveFromISR(canTaskHandle,&wake);
  if(wake)portYIELD_FROM_ISR();
}
static bool startTwai(){
  twai_general_config_t g=TWAI_GENERAL_CONFIG_DEFAULT(gpio_num_t(cfg.swap?5:4),gpio_num_t(cfg.swap?4:5),TWAI_MODE_NORMAL);
  g.tx_queue_len=16;g.rx_queue_len=256;
  g.alerts_enabled=TWAI_ALERT_BUS_OFF|TWAI_ALERT_BUS_RECOVERED|TWAI_ALERT_TX_FAILED|TWAI_ALERT_RX_QUEUE_FULL|TWAI_ALERT_BUS_ERROR;
  twai_timing_config_t timing=TWAI_TIMING_CONFIG_100KBITS();
  twai_filter_config_t filter=TWAI_FILTER_CONFIG_ACCEPT_ALL();
  if(twai_driver_install(&g,&timing,&filter)!=ESP_OK)return false;
  if(twai_start()!=ESP_OK){twai_driver_uninstall();return false;}return true;
}
static bool sendVehicle(const Frame& f){
  twai_message_t t={};t.identifier=f.id;t.extd=bool(f.flags&1);t.rtr=bool(f.flags&2);t.data_length_code=f.len;
  memcpy(t.data,f.data,f.len);return twaiOk&&twai_transmit(&t,0)==ESP_OK;
}
static void finishProtocol(uint32_t now){
  if(!reservedSent||!twaiOk)return;
  twai_status_info_t status;
  if(twai_get_status_info(&status)==ESP_OK&&status.state==TWAI_STATE_RUNNING&&status.msgs_to_tx==0){
    reservedSent=false;if(reservedEpoch==ddp.epoch())ddp.wireComplete(reservedTag,now);
  }
}
static void packetReceived(const BapPacket& p,uint32_t now){
  ++count.bap;lastFunction=p.function;lastOpcode=p.opcode;size_t at=0,n=min(size_t(p.len),size_t(48));
  for(size_t i=0;i<n&&at+4<sizeof(lastPacket);++i)at+=snprintf(lastPacket+at,sizeof(lastPacket)-at,"%02X ",p.data[i]);
  lastPacket[at]=0;if(p.opcode==7)++count.bapErrors;
  client.response(p,now);parseAudio(p,now);
}
static void vehicleReceived(const Frame& f,uint32_t now){
  ++count.vrx;record(f,0,now);
  if(!(f.flags&3)&&(f.id==0x2C3||f.id==0x575)){lastRun=now;runSeen=true;}
  if(f.id!=DDP_TX&&f.id!=DDP_RX&&!(f.id>=0x660&&f.id<=0x68F))lastVehicle=now;
  client.observeVehicle(f,now);
  if(f.id==DDP_RX&&ddp.state()!=Ddp::Off&&ddp.state()!=Ddp::Backoff){
    // Um ACK recebido pode estar na fila antes de processarmos o TX completo.
    finishProtocol(now);ddp.receive(f,now);return;
  }
  if(!toRadio.push(f,now))++count.drop;
}
static void radioReceived(const Frame& f,uint32_t now){
  ++count.rrx;record(f,1,now);
  if(bap.feed(f,now,bapPacket))packetReceived(bapPacket,now);
  if(!toVehicle.push(f,now))++count.drop;
}
static void prune(FrameQueue<256>& q,uint32_t now){
  while(q.front()&&elapsed(now,q.front()->at,250)){q.pop();++count.expired;}
}
static void publish(const twai_status_info_t& native){
  Snapshot s;s.audio=audio;s.count=count;s.twai=twaiOk;s.mcp=mcpOk;s.active=active;
  s.area=ddp.areaAvailable();s.ddp=ddp.state();s.native=native.state;s.mcpFlags=mcpOk?mcp.errors():0;
  s.route=client.route();s.function=lastFunction;s.opcode=lastOpcode;strlcpy(s.packet,lastPacket,sizeof(s.packet));
  portENTER_CRITICAL(&mux);published=s;portEXIT_CRITICAL(&mux);
}
static void canTask(void*){
  canTaskHandle=xTaskGetCurrentTaskHandle();twaiOk=startTwai();mcpOk=mcp.begin(cfg.crystal);
  attachInterrupt(digitalPinToInterrupt(INT_PIN),mcpInterrupt,FALLING);
  uint32_t publishedAt=0,healthAt=0,recoverAt=0,initAt=millis();
  bool parked=false;twai_status_info_t native={};
  for(;;){
    uint32_t now=millis();
    if(parkRequest){
      if(!parked){
        ddp.stop(now);reservedPending=reservedSent=false;toVehicle.clear();toRadio.clear();radioLocal.clear();bap.reset();
        if(twaiOk){twai_stop();twai_driver_uninstall();twaiOk=false;}if(mcpOk)mcp.pause();
        parked=true;parkReady=true;
      }
      publish(native);vTaskDelay(pdMS_TO_TICKS(10));continue;
    }
    if(parked){
      parkReady=false;twaiOk=startTwai();mcpOk=mcp.begin(cfg.crystal);client.reset(now);ddp.reset(now);
      parked=false;lastVehicle=lastRun=0;runSeen=active=false;initAt=now;
    }
    if(retryRequest){retryRequest=false;client.reset(now);ddp.reset(now,0);reservedPending=reservedSent=false;}
    finishProtocol(now);
    if(twaiOk){
      twai_message_t t;
      for(uint8_t n=0;n<48&&twai_receive(&t,0)==ESP_OK;++n){
        if(t.data_length_code>8){++count.txError;continue;}
        Frame f;f.id=t.identifier;f.len=t.data_length_code;f.flags=(t.extd?1:0)|(t.rtr?2:0);
        memcpy(f.data,t.data,f.len);vehicleReceived(f,now);
      }
    }
    if(mcpOk){Frame f;for(uint8_t n=0;n<48&&mcp.read(f);++n)radioReceived(f,now);}
    bool was=active;
    active=runSeen?(lastRun&&!elapsed(now,lastRun,IDLE_MS)):(lastVehicle&&!elapsed(now,lastVehicle,IDLE_MS));
    if(active&&!was){
      activeSince=now;client.reset(now);ddp.reset(now,0);
      // Nao apresentar cache da viagem anterior como dado atual.
      if(audio.valid&&audio.updated!=now)audio.valid=false;
    }
    if(!active&&was){ddp.stop(now);reservedPending=reservedSent=false;radioLocal.clear();}
    bap.expire(now);client.tick(now);ddp.tick(now);
    if(twaiOk&&twai_get_status_info(&native)==ESP_OK&&native.state==TWAI_STATE_RUNNING){
      if(reservedSent&&elapsed(now,reservedAt,300)){
        ddp.reset(now);reservedSent=false;twai_clear_transmit_queue();++count.txError;
      }
      if(reservedPending&&reservedEpoch!=ddp.epoch())reservedPending=false;
      if(!reservedPending&&!reservedSent){reservedPending=ddp.nextFrame(reserved,reservedTag,now);reservedEpoch=ddp.epoch();}
      if(reservedPending){
        // Esvaziar fila antes de TX DDP permite medir intervalo apos TX fisico.
        if(native.msgs_to_tx==0&&sendVehicle(reserved)){
          record(reserved,2,now);++count.vtx;reservedPending=false;reservedSent=true;reservedAt=now;
        }
      }else if(!reservedSent){
        prune(toVehicle,now);
        for(uint8_t n=native.msgs_to_tx;n<4&&toVehicle.front();++n){
          if(!sendVehicle(toVehicle.front()->f))break;++count.vtx;toVehicle.pop();
        }
      }
    }
    if(mcpOk){
      if(mcp.completion(now)<0)++count.txError;prune(toRadio,now);
      while(radioLocal.front()&&elapsed(now,radioLocal.front()->at,500)){radioLocal.pop();++count.expired;}
      Pending* p=toRadio.front();bool local=false;if(!p){p=radioLocal.front();local=true;}
      if(p&&mcp.send(p->f,now)){++count.rtx;if(local){record(p->f,3,now);radioLocal.pop();}else toRadio.pop();}
    }
    if(elapsed(now,healthAt,100)){
      healthAt=now;
      if(twaiOk){
        uint32_t alerts=0;twai_read_alerts(&alerts,0);
        if(alerts&TWAI_ALERT_TX_FAILED){++count.txError;ddp.reset(now);reservedPending=reservedSent=false;}
        twai_get_status_info(&native);count.nativeMissed=native.rx_missed_count;count.busError=native.bus_error_count;
        if(native.state==TWAI_STATE_BUS_OFF){
          ddp.reset(now);reservedPending=reservedSent=false;toVehicle.clear();
          if(twai_initiate_recovery()==ESP_OK)++count.recoveries;
        }else if(native.state==TWAI_STATE_STOPPED)twai_start();
      }
      if(mcpOk){
        mcp.clearErrors();
        if((mcp.errors()&0x20)&&elapsed(now,recoverAt,3000)){
          recoverAt=now;mcpOk=mcp.begin(cfg.crystal);++count.recoveries;radioLocal.clear();toRadio.clear();
        }
      }
      if((!twaiOk||!mcpOk)&&elapsed(now,initAt,5000)){
        initAt=now;if(!twaiOk)twaiOk=startTwai();if(!mcpOk)mcpOk=mcp.begin(cfg.crystal);
      }
    }
    if(elapsed(now,publishedAt,250)){publish(native);publishedAt=now;}
    ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS(1));
  }
}

static WebServer server(80);
static char csrf[33]={};
static bool uploadAccepted=false,uploadStarted=false,uploadComplete=false,uploadFileSeen=false;
static String uploadError;
static uint32_t uploadAt=0,restartAt=0,uploadBytes=0;
static uint8_t uploadHeader[36]={};
static size_t uploadHeaderUsed=0;
static void loadSettings(){
  Preferences p;if(!p.begin("mk5rcd330",true))return;
  Settings saved;
  if(p.getBytesLength("config")==sizeof(saved)&&p.getBytes("config",&saved,sizeof(saved))==sizeof(saved)&&
    saved.magic==cfg.magic&&(saved.crystal==8||saved.crystal==16)&&saved.textFunction>=0x10&&saved.textFunction<=0x3F&&
    (saved.requestId==0||saved.requestId==0x6C9||saved.requestId==0x6C8||saved.requestId==0x67C))cfg=saved;
  p.end();
}
static bool saveSettings(const Settings& s){
  Preferences p;if(!p.begin("mk5rcd330",false))return false;
  bool ok=p.putBytes("config",&s,sizeof(s))==sizeof(s);p.end();return ok;
}
static Snapshot snapshot(){Snapshot s;portENTER_CRITICAL(&mux);s=published;portEXIT_CRITICAL(&mux);return s;}
static String jsonString(const char* s){
  String out="\"";
  for(size_t i=0;s[i];++i){unsigned char c=s[i];if(c=='"'||c=='\\'){out+='\\';out+=char(c);}else if(c>=32&&c<127)out+=char(c);else out+=' ';}
  out+='"';return out;
}
static bool authorized(){
  if(server.authenticate(WEB_USER,WEB_PASS))return true;
  server.requestAuthentication();return false;
}
static bool mutationAllowed(){
  if(!authorized())return false;
  if(!server.hasArg("token")||server.arg("token")!=csrf){server.send(403,"text/plain; charset=utf-8","Token inválido. Reabra a página.");return false;}
  if(uploadStarted||restartAt){server.send(409,"text/plain; charset=utf-8","Atualização ou reinício em andamento.");return false;}return true;
}
static bool parkCan(){
  if(!canTaskHandle){parkRequest=true;parkReady=true;return true;}
  parkRequest=true;if(canTaskHandle)xTaskNotifyGive(canTaskHandle);
  uint32_t started=millis();while(!parkReady&&!elapsed(millis(),started,2000))delay(2);
  if(!parkReady){parkRequest=false;return false;}return true;
}
static void resumeCan(){parkRequest=false;if(canTaskHandle)xTaskNotifyGive(canTaskHandle);}
static void statusPage(){
  if(!authorized())return;Snapshot s=snapshot();
  static const char* states[]={"Desligado","Conectando","Identificando","Parametros","Solicitando area","Parametros da area","Registrando Audio","Audio registrado","Apresentando Audio","Sessao pronta","Nova tentativa"};
  String j;j.reserve(1500);
  j="{\"version\":"+jsonString(VERSION)+",\"top\":"+jsonString(s.audio.top)+",\"bottom\":"+jsonString(s.audio.bottom);
  j+=",\"source\":"+jsonString(s.audio.source)+",\"valid\":"+String(s.audio.valid?"true":"false")+",\"metadata\":"+String(s.audio.metadata?"true":"false");
  j+=",\"age\":"+String(s.audio.valid?uint32_t(millis()-s.audio.updated)/1000:0);
  j+=",\"twai\":"+String(s.twai?"true":"false")+",\"mcp\":"+String(s.mcp?"true":"false");
  j+=",\"active\":"+String(s.active?"true":"false")+",\"area\":"+String(s.area?"true":"false")+",\"ddp\":"+jsonString(states[min(s.ddp,uint8_t(10))]);
  char hex[12];snprintf(hex,sizeof(hex),"0x%03X",s.route);j+=",\"route\":"+jsonString(hex);
  snprintf(hex,sizeof(hex),"0x%02X",s.function);
  j+=",\"function\":"+jsonString(hex)+",\"opcode\":"+String(s.opcode)+",\"packet\":"+jsonString(s.packet);
  j+=",\"vrx\":"+String(s.count.vrx)+",\"rrx\":"+String(s.count.rrx)+",\"vtx\":"+String(s.count.vtx)+",\"rtx\":"+String(s.count.rtx);
  j+=",\"bap\":"+String(s.count.bap)+",\"requests\":"+String(s.count.requests)+",\"updates\":"+String(s.count.text)+",\"draws\":"+String(s.count.draw);
  j+=",\"drops\":"+String(s.count.drop+s.count.expired+s.count.nativeMissed+s.count.mcpOverflow);
  j+=",\"errors\":"+String(s.count.txError+s.count.busError)+",\"bapInvalid\":"+String(s.count.bapInvalid);
  j+=",\"bapErrors\":"+String(s.count.bapErrors)+",\"busy\":"+String(s.count.busy)+",\"timeouts\":"+String(s.count.timeout)+",\"uptime\":"+String(millis()/1000)+"}";
  server.sendHeader("Cache-Control","no-store");server.send(200,"application/json",j);
}
static const char HTML[] PROGMEM=R"HTML(<!doctype html>
<html lang="pt-BR"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>MK5 · RCD330</title>
<style>*{box-sizing:border-box}body{font:16px system-ui,sans-serif;background:#11151b;color:#e7ecf3;margin:0;padding:22px}main{max-width:760px;margin:auto}h1{font-size:25px}h2{font-size:19px;margin-top:0}.card{background:#1c2330;border:1px solid #334057;border-radius:12px;padding:19px;margin:16px 0}small,.muted{color:#b4bfd0}#top,#bottom{font:22px monospace;color:#ffb477;min-height:29px;overflow-wrap:anywhere}label{display:block;margin:12px 0 5px}input,select,button{font:inherit;border:1px solid #55657c;border-radius:7px;padding:10px;max-width:100%;background:#111924;color:#fff}button{background:#274e78;cursor:pointer;margin-top:12px}a{color:#9dc9ff}pre{white-space:pre-wrap;overflow-wrap:anywhere;font-size:13px}summary{cursor:pointer}#result{white-space:pre-wrap}input[type=checkbox]{width:18px;height:18px;vertical-align:middle}</style>
<main><h1>MK5 · RCD330</h1><p class="muted">Informações de rádio e mídia no painel</p>
<section class="card"><h2>Áudio recebido</h2><div id="top">Aguardando dados da rádio</div><div id="bottom"></div><p id="audioStatus" class="muted"></p><p id="connection"></p><small id="panelStatus"></small></section>
<section class="card"><h2>Atualizar o ESP32</h2><p>Envie o <b>.ino.bin da aplicação</b> exportado pelo Arduino IDE. A ponte será pausada e o ESP32 reiniciará ao concluir.</p>
<form id="ota"><input id="firmware" type="file" accept=".bin" required><label><input id="confirm" type="checkbox" required> Pausar a comunicação da rádio durante a atualização</label><button>Enviar atualização</button></form><p id="result" role="status"></p></section>
<details class="card"><summary>Configuração</summary><form id="config">
<label for="swap">Ligação TWAI já instalada</label><select id="swap" name="swap"><option value="0">D4 = TX · D5 = RX</option><option value="1">D5 = TX · D4 = RX</option></select>
<label for="xtal">Cristal do MCP2515</label><select id="xtal" name="xtal"><option value="8">8 MHz</option><option value="16">16 MHz</option></select>
<label for="route">Rota BAP para a rádio</label><select id="route" name="route"><option value="0">Automática · tentativas de leitura limitadas</option><option value="6C9">0x6C9</option><option value="6C8">0x6C8</option><option value="67C">0x67C</option></select>
<label for="func">Função BAP de texto, em hexadecimal</label><input id="func" name="func" value="15" maxlength="2" pattern="[0-9a-fA-F]{2}">
<label><input id="bap" name="bap" type="checkbox" checked> Solicitar informações de áudio à rádio</label>
<label><input id="panel" name="panel" type="checkbox" checked> Traduzir áudio para o painel vermelho</label>
<button>Salvar e reiniciar</button></form></details>
<details class="card"><summary>Diagnóstico</summary><pre id="diagnostic"></pre><button id="retry" type="button">Reiniciar negociação</button><p><a href="/capture.csv">Baixar captura CAN recente</a></p><small>Uma página registrada ou um ACK não comprovam a presença de metadados. O estado de áudio acima depende das mensagens recebidas.</small></details>
<small>Rede oculta · endereço local 192.168.4.1 · Wi-Fi permanece ligado</small></main>
<script>
const token='__TOKEN__';let timer;
async function refresh(){try{
  const r=await fetch('/api/status',{cache:'no-store'});if(!r.ok)throw Error(r.status);const s=await r.json();
  document.getElementById('top').textContent=s.valid?s.top:'Aguardando dados reais da rádio';
  document.getElementById('bottom').textContent=s.bottom;
  document.getElementById('audioStatus').textContent=s.valid?(s.metadata?'Texto de estação/mídia recebido':'Fonte ou texto CAN; interpretação a confirmar')+' · '+s.source+' · há '+s.age+' s':'Ainda não foram recebidos textos de áudio. Verifique CAN e a negociação BAP.';
  document.getElementById('connection').textContent='Carro/TWAI: '+(s.twai?'iniciado':'falha')+' · Rádio/MCP2515: '+(s.mcp?'iniciado':'falha')+' · '+(s.active?'Carro ativo':'Sem atividade de uso');
  document.getElementById('panelStatus').textContent=s.ddp+' · área '+(s.area?'disponível':'não selecionada/indisponível');
  document.getElementById('diagnostic').textContent='Versão '+s.version+' · ligado há '+s.uptime+' s\nRX carro/rádio: '+s.vrx+' / '+s.rrx+'\nTX carro/rádio: '+s.vtx+' / '+s.rtx+'\nBAP: '+s.bap+' · GETs: '+s.requests+' · mudanças de texto: '+s.updates+'\nTransferências DDP confirmadas: '+s.draws+' · busy: '+s.busy+' · timeouts DDP: '+s.timeouts+'\nPerdas/expirados: '+s.drops+' · erros CAN: '+s.errors+'\nBAP inválido: '+s.bapInvalid+' · respostas de erro: '+s.bapErrors+'\nRota candidata: '+s.route+' · função: '+s.function+' · opcode: '+s.opcode+'\nÚltimo payload: '+s.packet;
}catch(e){document.getElementById('connection').textContent='Sem conexão com o ESP32.';}}
timer=setInterval(refresh,2000);refresh();
fetch('/api/config').then(r=>r.json()).then(s=>{for(const k of ['swap','xtal','route','func'])document.getElementById(k).value=s[k];for(const k of ['bap','panel'])document.getElementById(k).checked=s[k];});
document.getElementById('config').onsubmit=async e=>{e.preventDefault();const f=new URLSearchParams(new FormData(e.target));f.set('token',token);const r=await fetch('/config',{method:'POST',body:f});document.getElementById('result').textContent=await r.text();};
document.getElementById('retry').onclick=async()=>{const r=await fetch('/retry',{method:'POST',body:new URLSearchParams({token})});document.getElementById('result').textContent=await r.text();};
document.getElementById('ota').onsubmit=e=>{
  e.preventDefault();const file=document.getElementById('firmware').files[0];if(!file||!document.getElementById('confirm').checked)return;
  clearInterval(timer);const form=new FormData();form.append('firmware',file);const x=new XMLHttpRequest();x.open('POST','/update?token='+token+'&pause=1');
  x.upload.onprogress=p=>{if(p.lengthComputable)document.getElementById('result').textContent='Enviando: '+Math.round(100*p.loaded/p.total)+'%';};
  x.onload=()=>{document.getElementById('result').textContent=x.responseText;if(x.status!==200)timer=setInterval(refresh,2000);};
  x.onerror=()=>{document.getElementById('result').textContent='Conexão interrompida. Reconecte à rede e confira a versão.';timer=setInterval(refresh,2000);};x.send(form);
};
</script></html>)HTML";
static void rootPage(){
  if(!authorized())return;String page=FPSTR(HTML);page.replace("__TOKEN__",csrf);
  server.sendHeader("Cache-Control","no-store");server.sendHeader("X-Content-Type-Options","nosniff");
  server.send(200,"text/html; charset=utf-8",page);
}
static void configPage(){
  if(!authorized())return;
  char r[8],f[4];snprintf(r,sizeof(r),"%X",cfg.requestId);snprintf(f,sizeof(f),"%02X",cfg.textFunction);
  String j="{\"swap\":\""+String(cfg.swap?1:0)+"\",\"xtal\":\""+String(cfg.crystal)+"\",\"route\":\""+r+"\",\"func\":\""+f+"\",\"bap\":"+(cfg.client?"true":"false")+",\"panel\":"+(cfg.panel?"true":"false")+"}";
  server.sendHeader("Cache-Control","no-store");server.send(200,"application/json",j);
}
static void configure(){
  if(!mutationAllowed())return;Settings next=cfg;
  if(!server.hasArg("swap")||!server.hasArg("xtal")||!server.hasArg("route")||!server.hasArg("func")){server.send(400,"text/plain","Configuracao incompleta.");return;}
  next.swap=server.arg("swap")=="1";int xtal=server.arg("xtal").toInt();
  char* end=nullptr;String route=server.arg("route"),func=server.arg("func");
  unsigned long routeN=strtoul(route.c_str(),&end,16);
  bool routeValid=route.length()&&end&&*end==0&&(routeN==0||routeN==0x6C9||routeN==0x6C8||routeN==0x67C);
  unsigned long funcN=strtoul(func.c_str(),&end,16);
  if((xtal!=8&&xtal!=16)||!routeValid||!func.length()||!end||*end||funcN<0x10||funcN>0x3F){server.send(400,"text/plain","Parametros invalidos.");return;}
  next.crystal=xtal;next.requestId=routeN;next.textFunction=funcN;
  next.client=server.hasArg("bap");next.panel=server.hasArg("panel");
  if(!parkCan()){server.send(503,"text/plain","Nao foi possivel pausar a ponte.");return;}
  if(!saveSettings(next)){resumeCan();server.send(500,"text/plain","Falha ao salvar configuracao.");return;}
  server.send(200,"text/plain; charset=utf-8","Configuração salva. Reiniciando; reabra esta página em alguns segundos.");
  restartAt=millis()+1200;
}
static void capturePage(){
  if(!authorized())return;
  Trace* copy=new(std::nothrow) Trace[TRACE_COUNT];if(!copy){server.send(503,"text/plain","Memoria insuficiente.");return;}
  size_t n;
  portENTER_CRITICAL(&mux);n=traceUsed;
  for(size_t i=0;i<n;++i)copy[i]=traces[(traceHead+TRACE_COUNT-n+i)%TRACE_COUNT];
  portEXIT_CRITICAL(&mux);
  server.sendHeader("Content-Disposition","attachment; filename=mk5-can.csv");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);server.send(200,"text/csv","ms,bus,id,dlc,flags,data\n");
  static const char* bus[]={"carro_rx","radio_rx","carro_tx_local","radio_tx_local"};
  char line[140];
  for(size_t i=0;i<n;++i){
    const Trace& t=copy[i];int at=snprintf(line,sizeof(line),"%lu,%s,%03lX,%u,%u,",(unsigned long)t.ms,bus[min(t.bus,uint8_t(3))],(unsigned long)t.f.id,t.f.len,t.f.flags);
    for(uint8_t k=0;k<t.f.len;++k)at+=snprintf(line+at,sizeof(line)-at,"%02X%s",t.f.data[k],k+1<t.f.len?" ":"");
    snprintf(line+at,sizeof(line)-at,"\n");server.sendContent(line);
  }
  server.sendContent("");delete[] copy;
}
static void failUpload(const String& message){
  uploadError=message;if(uploadStarted)Update.abort();
  uploadStarted=uploadAccepted=uploadComplete=false;resumeCan();
}
static void uploadChunk(){
  HTTPUpload& u=server.upload();
  if(u.status==UPLOAD_FILE_START){
    if(uploadFileSeen){failUpload("Envie apenas um arquivo por atualizacao.");return;}
    uploadFileSeen=true;uploadComplete=false;uploadError="";uploadAccepted=false;uploadBytes=0;uploadHeaderUsed=0;
    if(!server.authenticate(WEB_USER,WEB_PASS)){uploadError="Nao autorizado.";return;}
    if(server.arg("token")!=csrf||server.arg("pause")!="1"){uploadError="Token invalido ou pausa nao confirmada.";return;}
    if(restartAt||uploadStarted){uploadError="Outra operacao esta em andamento.";return;}
    String name=u.filename;name.toLowerCase();
    if(!name.endsWith(".bin")||name.indexOf("merged")>=0||name.indexOf("bootloader")>=0||name.indexOf("partitions")>=0){uploadError="Selecione apenas o .ino.bin da aplicacao.";return;}
    if(!esp_ota_get_next_update_partition(nullptr)){uploadError="Particao OTA ausente. Grave por USB com particionamento OTA.";return;}
    if(!parkCan()){uploadError="Falha ao pausar CAN.";return;}
    if(!Update.begin(UPDATE_SIZE_UNKNOWN,U_FLASH)){failUpload(Update.errorString());return;}
    uploadAccepted=uploadStarted=true;uploadAt=millis();
  }else if(u.status==UPLOAD_FILE_WRITE&&uploadAccepted){
    uploadAt=millis();
    if(u.currentSize&&uploadBytes==0&&u.buf[0]!=0xE9){failUpload("Arquivo nao e uma imagem ESP32 de aplicacao.");return;}
    size_t take=min(u.currentSize,sizeof(uploadHeader)-uploadHeaderUsed);
    if(take){memcpy(uploadHeader+uploadHeaderUsed,u.buf,take);uploadHeaderUsed+=take;}
    // Cabecalho ESP32: chip_id em 12..13; primeiro app descriptor em 32..35.
    // Impede aceitar bootloader, merged image ou aplicacao para ESP32-S3/C3.
    if(uploadHeaderUsed>=14&&(uploadHeader[12]!=0||uploadHeader[13]!=0)){
      failUpload("O firmware nao e para ESP32 classico/WROOM.");return;
    }
    if(uploadHeaderUsed==36&&(uploadHeader[32]!=0x32||uploadHeader[33]!=0x54||uploadHeader[34]!=0xCD||uploadHeader[35]!=0xAB)){
      failUpload("Descritor de aplicacao ausente. Envie o .ino.bin exportado.");return;
    }
    if(Update.write(u.buf,u.currentSize)!=u.currentSize)failUpload(Update.errorString());
    else uploadBytes+=u.currentSize;
    delay(1);
  }else if(u.status==UPLOAD_FILE_END&&uploadAccepted){
    if(u.totalSize<36||u.totalSize!=uploadBytes||uploadHeaderUsed!=36){failUpload("Arquivo vazio ou incompleto.");return;}
    // Commit somente ao terminar TODA a requisicao, rejeitando arquivos extras.
    uploadAccepted=false;uploadComplete=true;
  }else if(u.status==UPLOAD_FILE_ABORTED){failUpload("Upload interrompido. Ponte retomada.");}
}
static void uploadFinished(){
  uploadFileSeen=false;
  if(!authorized()){if(uploadStarted)failUpload("Nao autorizado.");return;}
  if(!uploadComplete||!uploadStarted){
    if(uploadStarted)failUpload("Upload incompleto.");
    server.send(400,"text/plain; charset=utf-8","Atualização recusada: "+uploadError);return;
  }
  if(!Update.end(true)){
    failUpload(Update.errorString());
    server.send(400,"text/plain; charset=utf-8","Atualização recusada: "+uploadError);return;
  }
  uploadStarted=uploadAccepted=uploadComplete=false;
  server.sendHeader("Connection","close");
  server.send(200,"text/plain; charset=utf-8","Atualização recebida e validada. Reiniciando. Reconecte à rede oculta e confira a versão.");
  restartAt=millis()+1500;
}
static void beginWeb(){
  WiFi.persistent(false);WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(IPAddress(192,168,4,1),IPAddress(192,168,4,1),IPAddress(255,255,255,0));
  bool ok=WiFi.softAP(AP_SSID,AP_PASS,6,true,2);esp_wifi_set_ps(WIFI_PS_NONE);
  for(uint8_t i=0;i<4;++i)snprintf(csrf+i*8,9,"%08lX",(unsigned long)esp_random());
  server.on("/",HTTP_GET,rootPage);server.on("/api/status",HTTP_GET,statusPage);server.on("/api/config",HTTP_GET,configPage);
  server.on("/capture.csv",HTTP_GET,capturePage);server.on("/config",HTTP_POST,configure);
  server.on("/retry",HTTP_POST,[]{if(!mutationAllowed())return;retryRequest=true;server.send(200,"text/plain; charset=utf-8","Negociação reiniciada. Confira as respostas no diagnóstico.");});
  server.on("/update",HTTP_POST,uploadFinished,uploadChunk);
  server.onNotFound([]{if(authorized())server.send(404,"text/plain","Pagina nao encontrada.");});
  server.begin();Serial.printf("Wi-Fi oculto %s: %s. http://192.168.4.1\n",AP_SSID,ok?"ativo":"falha");
}
} // namespace MK5

void setup(){
  using namespace MK5;
  Serial.begin(115200);loadSettings();
  BaseType_t result=xTaskCreatePinnedToCore(canTask,"mk5-can",12288,nullptr,5,&canTaskHandle,1);
  if(result!=pdPASS){canTaskHandle=nullptr;Serial.println("Falha no task CAN. Interface web continuara disponivel.");}
  beginWeb();Serial.println("MK5 RCD330 Bridge 1.0.0 - ainda requer validacao fisica da unidade.");
}
void loop(){
  using namespace MK5;
  server.handleClient();
  if(uploadStarted&&elapsed(millis(),uploadAt,30000))failUpload("Tempo de upload excedido. Ponte retomada.");
  if(restartAt&&due(millis(),restartAt))ESP.restart();
  delay(2);
}
