#include <nvsdk_ngx.h>
#include "../vendor/ampere-core/temporal_interval_trace.h"
#include <cassert>
#include <cwchar>
#include <cstdio>
struct Parameters:NVSDK_NGX_Parameter {
 int count=2,index=1;
#define SET(T) void Set(const char*,T)override{}
 SET(unsigned long long) SET(float) SET(double) SET(unsigned int) SET(int) SET(ID3D11Resource*) SET(ID3D12Resource*) SET(void*)
#undef SET
#define GET(T) NVSDK_NGX_Result Get(const char*,T* value)const override{*value={};return NVSDK_NGX_Result_Success;}
 GET(unsigned long long) GET(float) GET(double) GET(unsigned int) GET(ID3D11Resource*) GET(ID3D12Resource*) GET(void*)
#undef GET
 NVSDK_NGX_Result Get(const char* key,int* value)const override{*value=std::strcmp(key,"DLSSG.MultiFrameCount")==0?count:std::strcmp(key,"DLSSG.MultiFrameIndex")==0?index:5;return NVSDK_NGX_Result_Success;}
 void Reset()override{}
};
int main(){
 using namespace temporal_interval_trace;
 wchar_t temp[MAX_PATH]{},directory[MAX_PATH]{};assert(GetTempPathW(MAX_PATH,temp));
 swprintf(directory,MAX_PATH,L"%sCodexFG080-contract-%lu-%llu",temp,GetCurrentProcessId(),GetTickCount64());assert(CreateDirectoryW(directory,nullptr));
 Initialize(directory,GetCurrentProcessId());Parameters p;
 SetEnabled(false);
 assert(RecordIfValidTemporalSample(nullptr,&p,true));Flush();
 assert(!ReadSnapshot().logReady && GetFileAttributesW(FilePath())==INVALID_FILE_ATTRIBUTES);
 p.index=3;assert(!RecordIfValidTemporalSample(nullptr,&p,true));p.index=1;
 SetEnabled(true);assert(RecordIfValidTemporalSample(nullptr,&p,true));Flush();
 assert(ReadSnapshot().logReady && ReadSnapshot().validSamples==1 && GetFileAttributesW(FilePath())!=INVALID_FILE_ATTRIBUTES);
 SetEnabled(false);const auto samples=ReadSnapshot().validSamples;
 assert(RecordIfValidTemporalSample(nullptr,&p,true));Flush();assert(ReadSnapshot().validSamples==samples);
 puts("Core interval logging: disabled creates no CSV, route validation remains active; enable records; disable stops collection: PASS");
 // Trace stream is process-lifetime owned; it lives only in this fixture's unique directory.
}
