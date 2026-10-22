#include "dlss_activity.h"
#include <cassert>
int main(){fgvk::DlssActivity s;assert(!s.Recent(0,100));s.Completed(100,true);assert(s.Recent(100,100));assert(s.Recent(200,100));assert(!s.Recent(201,100));s.Completed(201,false);assert(!s.Recent(201,100));s.Completed(202,true);assert(s.Recent(202,0));assert(!s.Recent(203,0));assert(!s.Recent(201,100));s.Reset();assert(!s.Recent(202,100));for(unsigned i=0;i<1000;++i){s.Completed(i,true);assert(s.Recent(i,100));assert(!s.Recent(i+101,100));}}
