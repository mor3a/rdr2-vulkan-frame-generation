#include <cstdint>
#include <cstdio>
#include <initializer_list>
extern "C" {
void* rdr_original_init=nullptr;void* rdr_original_ext=nullptr;void* rdr_original_ext2=nullptr;
static unsigned long long recorded=0;
static unsigned recordedKind=0;static void* recordedCaller=nullptr;
void rdr_record_appid(unsigned long long id,void* caller,unsigned kind){recorded=id;recordedKind=kind;recordedCaller=caller;}
uint64_t rdr_capture_init(uint64_t,double,uint64_t,double,uint64_t,double);
uint64_t rdr_capture_ext(uint64_t,double,uint64_t,double,uint64_t,double);
uint64_t rdr_capture_ext2(uint64_t,double,uint64_t,double,uint64_t,double);
}
static uint64_t Original(uint64_t a,double b,uint64_t c,double d,uint64_t e,double f){
  return a==0x1234 && b==1.25 && c==0x5678 && d==2.5 && e==0x9ABC && f==3.75 ? 0xFACE : 0;
}
int main(){
  rdr_original_init=rdr_original_ext=rdr_original_ext2=reinterpret_cast<void*>(&Original);
  int failures=0;
  unsigned kind=0;
  for(auto function:{&rdr_capture_init,&rdr_capture_ext,&rdr_capture_ext2}){
    recorded=0;
    if(function(0x1234,1.25,0x5678,2.5,0x9ABC,3.75)!=0xFACE || recorded!=0x1234 || recordedKind!=++kind || !recordedCaller)failures++;
  }
  std::printf("3 ABI forwarding checks: %d failures\n",failures);return failures?1:0;
}
