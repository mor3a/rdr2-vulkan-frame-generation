#include "experiment_metrics.h"
#include <tlhelp32.h>
#include <atomic>
#include <cwchar>

namespace fgvk::experiment {
namespace {
std::atomic<bool> enabled{false}, busy{false};
std::atomic<unsigned> generation{0};
SRWLOCK dataLock=SRWLOCK_INIT;
std::array<double,512> gaps{};
size_t gapCount=0,gapNext=0;
LONGLONG previousPresent=0;
std::atomic<LONGLONG> frequency{0};
Snapshot cached;
ULONGLONG lastLaunch=0;
struct Baseline {DWORD id{};uint64_t creation{},cpu{},cycles{};bool cyclesValid{};};
std::array<Baseline,512> previousThreads{};
size_t previousCount=0;
ULONGLONG previousTime=0;
struct Handle {
  HANDLE value{};
  ~Handle(){if(value && value!=INVALID_HANDLE_VALUE)CloseHandle(value);}
};
uint64_t Bits(FILETIME t){return (uint64_t(t.dwHighDateTime)<<32)|t.dwLowDateTime;}
using DescriptionFn=HRESULT(WINAPI*)(HANDLE,PWSTR*);

DWORD WINAPI Sample(void* argument){
  SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_BELOW_NORMAL);
  const unsigned epoch=static_cast<unsigned>(reinterpret_cast<uintptr_t>(argument));
  std::array<double,512> copy{};size_t count;
  std::array<Baseline,512> oldThreads{};size_t oldCount;ULONGLONG oldTime;
  AcquireSRWLockShared(&dataLock);
  count=gapCount;for(size_t i=0;i<count;++i)copy[i]=gaps[(gapCount==gaps.size()?gapNext+i:i)%gaps.size()];oldThreads=previousThreads;oldCount=previousCount;oldTime=previousTime;
  ReleaseSRWLockShared(&dataLock);
  Snapshot result;result.initialized=true;result.clientPresent=detail::Summarize(copy.data(),count);
  const ULONGLONG now=GetTickCount64();
  std::array<Baseline,512> current{};size_t currentCount=0;
  Handle list{CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,0)};
  THREADENTRY32 entry{};entry.dwSize=sizeof(entry);
  const DWORD pid=GetCurrentProcessId();
  static const auto description=reinterpret_cast<DescriptionFn>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"),"GetThreadDescription"));
  bool enumerated=list.value!=INVALID_HANDLE_VALUE && Thread32First(list.value,&entry);
  result.unavailable=!enumerated;
  if(enumerated)do{
    if(entry.th32OwnerProcessID!=pid)continue;
    ++result.threadCount;
    if(currentCount==current.size())continue;
    Handle thread{OpenThread(THREAD_QUERY_LIMITED_INFORMATION,FALSE,entry.th32ThreadID)};
    FILETIME creation{},exit{},kernel{},user{};
    if(!thread.value || !GetThreadTimes(thread.value,&creation,&exit,&kernel,&user))continue;
    Baseline item;item.id=entry.th32ThreadID;item.creation=Bits(creation);item.cpu=Bits(kernel)+Bits(user);
    ULONG64 cycles=0;item.cyclesValid=QueryThreadCycleTime(thread.value,&cycles)!=FALSE;item.cycles=cycles;
    current[currentCount++]=item;
    if(!oldTime || now<=oldTime)continue;
    const Baseline* old=nullptr;
    for(size_t i=0;i<oldCount;++i)if(oldThreads[i].id==item.id && oldThreads[i].creation==item.creation){old=&oldThreads[i];break;}
    if(!old || item.cpu<old->cpu)continue;
    ThreadSample sample;sample.id=item.id;
    const double elapsed=double(now-oldTime);
    sample.cpuMsPerSecond=double(item.cpu-old->cpu)/10./elapsed;
    if(item.cyclesValid && old->cyclesValid && item.cycles>=old->cycles)
      sample.cyclesPerSecond=static_cast<uint64_t>(double(item.cycles-old->cycles)*1000./elapsed);
    if(description){PWSTR name=nullptr;if(SUCCEEDED(description(thread.value,&name)) && name){wcsncpy_s(sample.name,name,_TRUNCATE);}if(name)LocalFree(name);}
    for(size_t i=0;i<result.threads.size();++i)
      if(!result.threads[i].id || sample.cpuMsPerSecond>result.threads[i].cpuMsPerSecond){
        for(size_t j=result.threads.size()-1;j>i;--j)result.threads[j]=result.threads[j-1];
        result.threads[i]=sample;break;
      }
  }while(Thread32Next(list.value,&entry));
  if(!currentCount)result.unavailable=true;
  AcquireSRWLockExclusive(&dataLock);
  if(enabled.load(std::memory_order_relaxed) && generation.load(std::memory_order_relaxed)==epoch){
    previousThreads=current;previousCount=currentCount;previousTime=now;cached=result;
  }
  ReleaseSRWLockExclusive(&dataLock);
  busy.store(false,std::memory_order_release);return 0;
}
}

bool Enabled(){return enabled.load(std::memory_order_relaxed);}
void SetEnabled(bool value){
  AcquireSRWLockExclusive(&dataLock);
  if(enabled.load(std::memory_order_relaxed)!=value){
    enabled.store(false,std::memory_order_release);
    generation.fetch_add(1,std::memory_order_relaxed);
    previousPresent=0;gapCount=gapNext=0;cached={};lastLaunch=0;previousCount=0;previousTime=0;
    LARGE_INTEGER hz{};if(value && QueryPerformanceFrequency(&hz))frequency.store(hz.QuadPart,std::memory_order_relaxed);
    enabled.store(value,std::memory_order_release);
  }
  ReleaseSRWLockExclusive(&dataLock);
}
void RecordPresent(){
  if(!enabled.load(std::memory_order_relaxed))return;
  const unsigned epoch=generation.load(std::memory_order_relaxed);
  LARGE_INTEGER now{};if(!QueryPerformanceCounter(&now))return;
  if(!TryAcquireSRWLockExclusive(&dataLock))return;
  const auto hz=frequency.load(std::memory_order_relaxed);
  if(enabled.load(std::memory_order_relaxed) && generation.load(std::memory_order_relaxed)==epoch && hz>0){
    if(previousPresent && now.QuadPart>previousPresent){
      const double delta=double(now.QuadPart-previousPresent)*1000./double(hz);
      if(detail::ValidGap(delta)){gaps[gapNext]=delta;gapNext=(gapNext+1)%gaps.size();if(gapCount<gaps.size())++gapCount;}
    }
    previousPresent=now.QuadPart;
  }
  const bool sampleDue=!lastLaunch || GetTickCount64()-lastLaunch>=1000;
  ReleaseSRWLockExclusive(&dataLock);
  if(sampleDue)(void)Read();
}
Snapshot Read(){
  if(!Enabled())return {};
  Snapshot result;unsigned epoch=0;bool launch=false;
  const ULONGLONG now=GetTickCount64();
  AcquireSRWLockExclusive(&dataLock);result=cached;
  if(enabled.load(std::memory_order_relaxed) && (!lastLaunch || now-lastLaunch>=1000) && !busy.exchange(true,std::memory_order_acq_rel)){
    lastLaunch=now?now:1;epoch=generation.load(std::memory_order_relaxed);launch=true;
  }
  ReleaseSRWLockExclusive(&dataLock);
  if(launch){
    HMODULE self{};
    if(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,reinterpret_cast<LPCWSTR>(&Sample),&self)){
      Handle worker{CreateThread(nullptr,0,Sample,reinterpret_cast<void*>(static_cast<uintptr_t>(epoch)),0,nullptr)};
      if(worker.value)return result;
    }
    AcquireSRWLockExclusive(&dataLock);
    if(enabled.load(std::memory_order_relaxed) && generation.load(std::memory_order_relaxed)==epoch){cached.initialized=true;cached.unavailable=true;result=cached;}
    ReleaseSRWLockExclusive(&dataLock);
    busy.store(false,std::memory_order_release);
  }
  return result;
}
}
