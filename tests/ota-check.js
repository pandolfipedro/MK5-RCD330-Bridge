const fs=require('fs'),assert=require('assert');
const path=require('path');
const projectRoot=path.resolve(__dirname,'..');
const buildDir=path.join(projectRoot,'.test-build');
fs.mkdirSync(buildDir,{recursive:true});
const ino=fs.readFileSync(path.join(projectRoot,'MK5_RCD330_Bridge','MK5_RCD330_Bridge.ino'),'utf8');
const a=ino.indexOf('static void failUpload'),b=ino.indexOf('static void beginWeb',a);
assert(a>=0&&b>a);
const base=String.raw`
#include <string>
#include <cstring>
#include <cstdint>
#include <cctype>
#include <cassert>
#include <cstdio>
#include <algorithm>
using std::min;
struct String:std::string{
  using std::string::string;
  String()=default;String(const std::string& s):std::string(s){}
  void toLowerCase(){for(char& c:*this)c=tolower((unsigned char)c);}
  bool endsWith(const char* s){size_t n=strlen(s);return size()>=n&&compare(size()-n,n,s)==0;}
  int indexOf(const char* s){size_t p=find(s);return p==npos?-1:int(p);}
};
enum{UPLOAD_FILE_START,UPLOAD_FILE_WRITE,UPLOAD_FILE_END,UPLOAD_FILE_ABORTED,U_FLASH};
struct HTTPUpload{int status=0;String filename="app.ino.bin";size_t currentSize=0,totalSize=0;uint8_t buf[4096]={};};
static bool partition=true,parked=false,parkAllowed=true;
static uint32_t clockMs=100;
uint32_t millis(){return clockMs;}
void delay(int){}
void* esp_ota_get_next_update_partition(void*){return partition?(void*)1:nullptr;}
bool parkCan(){if(!parkAllowed)return false;parked=true;return true;}
void resumeCan(){parked=false;}
struct Updater{
  bool active=false,beginOk=true,writeOk=true,endOk=true;
  int begins=0,commits=0,aborts=0;size_t written=0;
  bool begin(int,int){++begins;active=beginOk;return beginOk;}
  size_t write(uint8_t*,size_t n){assert(active);if(!writeOk)return 0;written+=n;return n;}
  bool end(bool){assert(active);++commits;active=false;return endOk;}
  void abort(){active=false;++aborts;}
  const char* errorString(){return "simulated flash error";}
}Update;
struct Server{
  HTTPUpload u;bool auth=true,tokenOk=true;int status=0;String body;
  HTTPUpload& upload(){return u;}
  bool authenticate(const char*,const char*){return auth;}
  String arg(const char* key){if(!strcmp(key,"token"))return tokenOk?"csrf":"bad";return "1";}
  void send(int code,const char*,const String& text){status=code;body=text;}
  void sendHeader(const char*,const char*){}
}server;
bool authorized(){if(server.auth)return true;server.status=401;return false;}
constexpr char WEB_USER[]="admin",WEB_PASS[]="password",csrf[]="csrf";
constexpr int UPDATE_SIZE_UNKNOWN=-1;
static bool uploadAccepted=false,uploadStarted=false,uploadComplete=false,uploadFileSeen=false;
static String uploadError;
static uint32_t uploadAt=0,restartAt=0,uploadBytes=0;
static uint8_t uploadHeader[36]={};static size_t uploadHeaderUsed=0;
`;
const tests=String.raw`
void reset(){
  Update=Updater{};server=Server{};partition=parkAllowed=true;parked=false;
  uploadAccepted=uploadStarted=uploadComplete=uploadFileSeen=false;
  uploadAt=restartAt=uploadBytes=0;uploadHeaderUsed=0;uploadError="";
}
void start(){server.u.status=UPLOAD_FILE_START;uploadChunk();}
void writeChunk(uint8_t first=0xe9){server.u.status=UPLOAD_FILE_WRITE;server.u.currentSize=100;memset(server.u.buf,0,100);server.u.buf[0]=first;server.u.buf[32]=0x32;server.u.buf[33]=0x54;server.u.buf[34]=0xcd;server.u.buf[35]=0xab;uploadChunk();}
void endFile(){server.u.status=UPLOAD_FILE_END;server.u.totalSize=200;uploadChunk();}
int main(){
  reset();start();assert(parked&&uploadStarted);writeChunk();writeChunk(0x12);
  assert(uploadBytes==200&&Update.written==200);endFile();
  assert(uploadComplete&&Update.commits==0);uploadFinished();
  assert(server.status==200&&Update.commits==1&&restartAt>clockMs&&parked);
  reset();server.tokenOk=false;start();uploadFinished();assert(server.status==400&&!Update.begins&&!parked);
  reset();server.auth=false;start();uploadFinished();assert(server.status==401&&!Update.begins);
  reset();partition=false;start();uploadFinished();assert(server.status==400&&!parked&&!Update.begins);
  reset();server.u.filename="app.merged.bin";start();uploadFinished();assert(server.status==400&&!Update.begins);
  reset();start();writeChunk(0);uploadFinished();assert(server.status==400&&!parked&&Update.aborts==1&&!Update.commits);
  reset();start();writeChunk();writeChunk();endFile();start();uploadFinished();
  assert(server.status==400&&!parked&&Update.aborts==1&&!Update.commits);
  reset();start();server.u.status=UPLOAD_FILE_ABORTED;uploadChunk();uploadFinished();
  assert(server.status==400&&!parked&&Update.aborts==1&&!Update.commits);
  reset();Update.beginOk=false;start();uploadFinished();assert(server.status==400&&!parked&&!Update.commits);
  reset();start();Update.writeOk=false;writeChunk();uploadFinished();assert(server.status==400&&!parked&&Update.aborts==1);
  reset();start();writeChunk();server.u.status=UPLOAD_FILE_END;server.u.totalSize=200;uploadChunk();uploadFinished();
  assert(server.status==400&&!parked&&!Update.commits);
  reset();start();writeChunk();writeChunk();endFile();Update.endOk=false;uploadFinished();
  assert(server.status==400&&!parked&&restartAt==0);
  reset();start();server.u.status=UPLOAD_FILE_WRITE;server.u.currentSize=100;
  server.u.buf[0]=0xe9;server.u.buf[12]=9;uploadChunk();uploadFinished();
  assert(server.status==400&&!parked&&!Update.commits);
  reset();start();server.u.status=UPLOAD_FILE_WRITE;server.u.currentSize=100;
  server.u.buf[0]=0xe9;uploadChunk();uploadFinished();
  assert(server.status==400&&!parked&&!Update.commits);
  puts("PASS: OTA multipart chunks, authentication, CSRF, partition, filename, magic, one-file limit, abort, length, flash failure and commit timing.");
}
`;
fs.writeFileSync(path.join(buildDir,'ota_test.cpp'),base+ino.slice(a,b)+tests);
