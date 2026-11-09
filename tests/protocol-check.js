const fs=require('fs'),vm=require('vm'),assert=require('assert');
const path=require('path');
const projectRoot=path.resolve(__dirname,'..');
const buildDir=path.join(projectRoot,'.test-build');
fs.mkdirSync(buildDir,{recursive:true});
const ino=fs.readFileSync(path.join(projectRoot,'MK5_RCD330_Bridge','MK5_RCD330_Bridge.ino'),'utf8');
function part(a,b){const i=ino.indexOf(a),j=ino.indexOf(b,i);assert(i>=0&&j>i);return ino.slice(i,j);}
new vm.Script(part('<script>','</script>').slice(8));
assert(ino.includes('WiFi.softAP(AP_SSID,AP_PASS,6,true,2)'));
assert(ino.includes('cfg.swap?5:4'));
assert(ino.includes('CS_PIN=13,INT_PIN=14'));
const base=String.raw`
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <cassert>
#include <algorithm>
#include <cctype>
#include <vector>
#include <strings.h>
using std::min;
#define constrain(x,a,b) ((x)<(a)?(a):((x)>(b)?(b):(x)))
size_t strlcpy(char* d,const char* s,size_t n){size_t k=strlen(s);if(n){size_t c=std::min(k,n-1);memcpy(d,s,c);d[c]=0;}return k;}
constexpr size_t BAP_MAX=512,TRACE_COUNT=192;
constexpr uint16_t DDP_TX=0x680,DDP_RX=0x681;
constexpr uint32_t BOOT_DDP_MS=20000;
constexpr uint8_t SCK_PIN=18,MISO_PIN=19,MOSI_PIN=23,CS_PIN=13,INT_PIN=14;
constexpr int VSPI=2,OUTPUT=1,INPUT_PULLUP=2,HIGH=1,LOW=0,MSBFIRST=1,SPI_MODE0=0;
static uint8_t chip[256],operation=0,address=0,phase=0,mask=0;
void pinMode(int,int){}
void delay(int){}
void digitalWrite(int,int value){
  if(!value){phase=0;operation=0;}
  else{if(operation==0x90)chip[0x2c]&=~1;if(operation==0x94)chip[0x2c]&=~2;}
}
struct SPISettings{SPISettings(int,int,int){}};
struct SPIClass{
  explicit SPIClass(int){}
  void begin(int,int,int,int){}
  void beginTransaction(SPISettings){}
  void endTransaction(){}
  uint8_t transfer(uint8_t v){
    if(!phase){
      operation=v;phase=1;
      if(v==0xc0){memset(chip,0,sizeof(chip));chip[0x0e]=chip[0x0f]=0x80;}
      if(v==0x81)chip[0x30]|=8;
      if(v==0x40)address=0x31;
      if(v==0x90)address=0x61;
      if(v==0x94)address=0x71;
      return 0;
    }
    if(operation==3||operation==2){
      if(phase==1){address=v;++phase;return 0;}
      if(operation==3)return chip[address++];
      chip[address]=v;if(address==0x0f)chip[0x0e]=v&0xe0;++address;return 0;
    }
    if(operation==5){
      if(phase==1){address=v;phase=2;return 0;}
      if(phase==2){mask=v;phase=3;return 0;}
      chip[address]=(chip[address]&~mask)|(v&mask);
      if(address==0x0f)chip[0x0e]=chip[address]&0xe0;return 0;
    }
    if(operation==0x40){chip[address++]=v;return 0;}
    if(operation==0x90||operation==0x94)return chip[address++];
    return 0;
  }
};
`;
const globals=String.raw`
static Settings cfg;static Audio audio;static Counters count;
static bool active=true,mcpOk=true;static uint32_t activeSince=0;
static FrameQueue<16> radioLocal;
static bool elapsed(uint32_t n,uint32_t t,uint32_t p){return uint32_t(n-t)>=p;}
static bool due(uint32_t n,uint32_t t){return int32_t(n-t)>=0;}
`;
const tests=String.raw`
Frame frame(std::initializer_list<uint8_t> b,uint32_t id=0x66c){
  Frame f;f.id=id;f.len=b.size();std::copy(b.begin(),b.end(),f.data);return f;
}
std::vector<Frame> fragmented(const std::vector<uint8_t>& payload,uint8_t channel=0){
  std::vector<Frame> v;Frame f;f.id=0x66c;f.len=8;
  f.data[0]=0x80|(channel<<4)|(payload.size()>>8);f.data[1]=payload.size();f.data[2]=0x4c;f.data[3]=0x55;
  size_t n=min(size_t(4),payload.size());memcpy(f.data+4,payload.data(),n);f.len=4+n;v.push_back(f);
  uint8_t seq=0;for(size_t off=n;off<payload.size();){
    Frame c;c.id=0x66c;c.data[0]=0xc0|(channel<<4)|(seq++&15);size_t len=min(size_t(7),payload.size()-off);
    c.len=len+1;memcpy(c.data+1,payload.data()+off,len);off+=len;v.push_back(c);
  }return v;
}
int main(){
  {
    Mcp2515 driver;assert(driver.begin(8));
    assert(chip[0x2a]==1&&chip[0x29]==0xb4&&chip[0x28]==0x86&&chip[0x2b]==0x27);
    Frame f=frame({0x22},0x5c1);assert(driver.send(f,100));assert(!driver.send(f,100));
    assert(chip[0x31]==0xb8&&chip[0x32]==0x20&&chip[0x35]==1&&chip[0x36]==0x22);
    chip[0x30]&=~8;chip[0x2c]|=4;assert(driver.completion(101)==1&&!(chip[0x2c]&4));
    f=frame({1,2,3,4,5,6,7,8},0x1fffffff);f.flags=1;assert(driver.send(f,102));
    assert(chip[0x31]==0xff&&chip[0x32]==0xeb&&chip[0x33]==0xff&&chip[0x34]==0xff);
    memcpy(chip+0x61,chip+0x31,13);chip[0x2c]|=1;Frame received;
    assert(driver.read(received)&&received.id==0x1fffffff&&received.flags==1&&received.len==8);
    assert(!memcmp(received.data,f.data,8)&&!(chip[0x2c]&1));
    chip[0x30]&=~8;assert(driver.completion(103)==1);
    f=Frame{};f.id=0x123;f.flags=2;f.len=4;assert(driver.send(f,104));
    assert(chip[0x31]==0x24&&chip[0x32]==0x60&&chip[0x35]==0x44);
    memcpy(chip+0x61,chip+0x31,13);chip[0x60]|=8;chip[0x2c]|=1;
    assert(driver.read(received)&&received.id==0x123&&received.flags==2&&received.len==4);
    assert(driver.completion(355)==-1&&!(chip[0x30]&8));
    assert(driver.begin(16)&&chip[0x2a]==3);
  }
  {
    FrameQueue<2> q;Frame a=frame({1}),b=frame({2});
    assert(q.push(a,100));assert(q.push(b,101));assert(!q.push(a,102));
    assert(q.front()->f.data[0]==1);q.pop();assert(q.front()->f.data[0]==2);q.pop();assert(!q.front());
  }
  {
    BapAssembler parser;BapPacket p;
    assert(parser.feed(frame({0x4c,0x55,2,'F','M'}),100,p));
    assert(p.opcode==4&&p.function==21&&p.len==3&&p.data[2]=='M');
    const auto data=std::vector<uint8_t>{8,'B','T',' ','A','u','d','i','o',4,'S','o','n','g',6,'A','r','t','i','s','t'};
    auto packets=fragmented(data);assert(packets.size()==4);
    for(size_t i=0;i<packets.size();++i)assert(parser.feed(packets[i],200+i,p)==(i+1==packets.size()));
    assert(p.len==data.size()&&!memcmp(p.data,data.data(),data.size()));
    parseAudio(p,300);assert(!strcmp(audio.top,"Song")&&!strcmp(audio.bottom,"Artist")&&audio.metadata);
    parser.reset();assert(!parser.feed(packets[0],500,p));
    assert(!parser.feed(packets[2],501,p));assert(!parser.feed(packets[3],502,p));
    parser.reset();auto other=fragmented(data,1);
    assert(!parser.feed(packets[0],600,p));assert(!parser.feed(other[0],601,p));
    for(size_t i=1;i<packets.size();++i){
      assert(parser.feed(packets[i],602+i*2,p)==(i+1==packets.size()));
      assert(parser.feed(other[i],603+i*2,p)==(i+1==other.size()));
    }
    parser.reset();assert(!parser.feed(packets[0],1000,p));parser.expire(2001);
    assert(!parser.feed(packets[1],2002,p));assert(!parser.feed(frame({0x82,0x01,0x4c,0x55}),3000,p));
  }
  {
    BapPacket p;p.opcode=4;p.function=21;
    const uint8_t raw[]={9,'1','0','3','.','5',' ','M','H','z',11,'R','A','D','I','O',' ','T','E','S','T','E'};
    p.len=sizeof(raw);memcpy(p.data,raw,sizeof(raw));parseAudio(p,4000);
    assert(!strcmp(audio.top,"RADIO TESTE")&&!strcmp(audio.bottom,"103.5 MHz"));
    const uint8_t source[]={9,'B','l','u','e','t','o','o','t','h',1,0,0};
    p.len=sizeof(source);memcpy(p.data,source,sizeof(source));parseAudio(p,4001);
    assert(!strcmp(audio.top,"Bluetooth")&&!audio.metadata);
    const uint8_t arbitrary[]={'U','n','k','n','o','w','n',' ','t','e','x','t'};
    p.len=sizeof(arbitrary);memcpy(p.data,arbitrary,sizeof(arbitrary));parseAudio(p,4002);
    assert(!audio.metadata&&!strcmp(audio.source,"CAN (layout a confirmar)"));
    char text[40];const uint8_t utf8[]={0xc3,0x81,'u','d','i','o',' ',0xc3,0xa7,0xc3,0xa3,'o'};
    cleanText(utf8,sizeof(utf8),text,sizeof(text));assert(!strcmp(text,"Audio cao"));
  }
  {
    Ddp d;uint32_t now=20000;Frame out;uint8_t tag;
    active=true;activeSince=0;audio.valid=true;audio.generation=5;audio.changed=now;
    strlcpy(audio.top,"ABCDEFGHIJKLMNOP",65);strlcpy(audio.bottom,"1234567890ABCDEF",65);
    d.tick(now);assert(d.nextFrame(out,tag,now)&&out.data[0]==0xa0);d.wireComplete(tag,now);
    d.receive(frame({0xa1,4,0x8a,0xff,0x32,0xff},DDP_RX),++now);
    assert(d.nextFrame(out,tag,now)&&out.data[0]==0x10&&out.data[2]==0x52);d.wireComplete(tag,now);
    d.receive(frame({0xb2},DDP_RX),++now);assert(d.state()==Ddp::Identify);
    d.receive(frame({0xb1},DDP_RX),++now);assert(d.state()==Ddp::DisplayParams);
    d.receive(frame({0x10,0x20,1,0x11},DDP_RX),++now);
    assert(d.nextFrame(out,tag,now)&&out.data[0]==0xb1);d.wireComplete(tag,now);
    now+=5;assert(d.nextFrame(out,tag,now)&&out.data[0]==0x11&&out.data[1]==1);d.wireComplete(tag,now);
    d.receive(frame({0xb2},DDP_RX),++now);assert(d.state()==Ddp::AreaParams);
    d.receive(frame({0x11,0x21,0,4,0,0x6e,0,0x5b},DDP_RX),++now);
    assert(d.nextFrame(out,tag,now)&&out.data[0]==0xb2);d.wireComplete(tag,now);
    now+=5;assert(d.nextFrame(out,tag,now)&&out.data[0]==0x22);d.wireComplete(tag,now);
    assert(!d.nextFrame(out,tag,now+4));now+=5;
    assert(d.nextFrame(out,tag,now)&&out.data[0]==0x13);d.wireComplete(tag,now);
    d.receive(frame({0xb4},DDP_RX),++now);assert(d.state()==Ddp::Bound);
    d.receive(frame({0x12,0x23,0,1,0},DDP_RX),++now);
    assert(d.nextFrame(out,tag,now)&&out.data[0]==0xb3);d.wireComplete(tag,now);
    now+=5;d.tick(now);assert(d.nextFrame(out,tag,now)&&out.data[0]==0x14);d.wireComplete(tag,now);
    d.receive(frame({0xb5},DDP_RX),++now);assert(d.state()==Ddp::Ready&&d.areaAvailable());
    now+=201;d.tick(now);unsigned frames=0,windowAcks=0;uint32_t draws=count.draw;
    while(frames<9){
      assert(d.nextFrame(out,tag,now));
      if(tag==0){d.wireComplete(tag,now);continue;}
      ++frames;uint8_t header=out.data[0];d.wireComplete(tag,now);
      if((header&0xf0)==0||(header&0xf0)==0x10){
        ++windowAcks;
        if(frames==4){d.receive(frame({uint8_t(0x90|((header+1)&15))},DDP_RX),now);assert(count.draw==draws);assert(!d.nextFrame(out,tag,now+5));}
        d.receive(frame({uint8_t(0xb0|((header+1)&15))},DDP_RX),now);
      }
      now+=5;
    }
    assert(windowAcks==3&&count.draw==draws+1);
    d.receive(frame({0x13,0x23,0,0,0},DDP_RX),++now);
    assert(!d.areaAvailable());assert(d.nextFrame(out,tag,now)&&tag==0);d.wireComplete(tag,now);
    ++audio.generation;now+=300;d.tick(now);assert(!d.nextFrame(out,tag,now));
    now+=6000;d.tick(now);assert(d.state()==Ddp::Backoff);
  }
  puts("PASS: queue bounds; BAP assembly, sequence, interleaving, timeout, overflow; audio fields and UTF-8; DDP handshake, ACK, window 4+4+1, busy, area and timeout.");
}
`;
fs.writeFileSync(path.join(buildDir,'protocol_test.cpp'),base+part('struct Frame {','static Settings cfg;')+globals+part('class Mcp2515 {','static Mcp2515 mcp;')+part('struct BapPacket {','static BapAssembler bap;')+part('static void cleanText','// Transporte DDP.')+part('class Ddp {','static Ddp ddp;')+tests);
console.log('PASS: hidden AP configuration and embedded JavaScript syntax. Protocol tests extracted from delivered .ino.');
