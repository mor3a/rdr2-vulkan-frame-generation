#include "runtime_errors.h"
#include <cstdio>
#include <thread>
#include <vector>
#include <string>
int main() {
    using namespace RuntimeErrors;
    int failures=0, checks=0;
    auto check=[&](bool b){++checks; if(!b)++failures;};
    check(SnapshotErrors().count==0);
    ReportError(Code::CameraMissing,"not loaded",127);
    ReportError(Code::CameraMissing,"retry failed",126);
    auto s=SnapshotErrors();
    check(s.count==1 && s.entries[0].count==2 && s.entries[0].native==126);
    check(std::strcmp(s.entries[0].id,"FG-101")==0 && std::strcmp(s.entries[0].detail.data(),"retry failed")==0);
    ClearError(Code::CameraMissing); check(SnapshotErrors().count==0);
    std::string large(4096,'x'); ReportError(Code::InputInvalid,large.c_str());
    s=SnapshotErrors(); check(s.entries[0].detail[255]=='\0' && std::strlen(s.entries[0].detail.data())==255);
    ReportError(Code::InputInvalid,nullptr); s=SnapshotErrors(); check(s.entries[0].detail[0]=='\0');
    for(unsigned i=0;i<static_cast<unsigned>(Code::Count);++i) ReportError(static_cast<Code>(i),"failure");
    ReportError(static_cast<Code>(999),"invalid"); check(SnapshotErrors().count==Capacity);
    for(unsigned i=0;i<static_cast<unsigned>(Code::Count);++i) ClearError(static_cast<Code>(i));
    std::vector<std::thread> workers;
    for(int t=0;t<4;++t) workers.emplace_back([]{for(int i=0;i<1000;++i){ReportError(Code::ProviderState,"concurrent"); (void)SnapshotErrors();}});
    for(auto& t:workers)t.join();
    s=SnapshotErrors(); check(s.count==1 && s.entries[0].count==4000);
    SetWarning(Warning::VRAM,true); SetWarning(Warning::Telemetry,true); check(GetWarnings()==3);
    SetWarning(Warning::VRAM,false); check(GetWarnings()==2);
    SetWarning(Warning::Telemetry,false); check(GetWarnings()==0);
    ClearError(Code::ProviderState); ReportError(Code::ProviderState,"recurred");
    check(SnapshotErrors().entries[0].count==1);
    check(SnapshotErrors().entries[0].firstTime!=0 && SnapshotErrors().entries[0].firstTime<=SnapshotErrors().entries[0].time);
    ClearError(Code::ProviderState);
    ReportError(Code::PresentFailed,"present failed",-4);
    ReportError(Code::GenerationStalled,"stalled",2);
    s=SnapshotErrors();
    check(s.count==2 && std::strcmp(s.entries[0].id,"SYS-102")==0 && s.entries[0].native==-4);
    check(std::strcmp(s.entries[1].id,"FG-401")==0 && std::strcmp(Title(Code::GenerationStalled),"Frame generation stalled")==0);
    ClearError(Code::PresentFailed); ClearError(Code::GenerationStalled); check(SnapshotErrors().count==0);
    ReportError(Code::MenuInput,"message hook failed; fallback active",5);
    ReportError(Code::CursorHook,"optional detours declined",87);
    ReportError(Code::UnlockInit,"native RTX 40 fallback",126);
    s=SnapshotErrors();check(s.count==3);
    check(std::strcmp(CodeId(Code::MenuInput),"UI-103")==0 && !AffectsGeneration(Code::MenuInput));
    check(std::strcmp(CodeId(Code::CursorHook),"UI-104")==0 && !AffectsGeneration(Code::CursorHook));
    check(std::strcmp(CodeId(Code::UnlockInit),"FG-205")==0 && !AffectsGeneration(Code::UnlockInit));
    check(AffectsGeneration(Code::ProviderInit) && AffectsGeneration(Code::HookInstall));
    for(unsigned i=0;i<static_cast<unsigned>(Code::Count);++i) ClearError(static_cast<Code>(i));
    ReportError(Code::HardwareScheduling,"Streamline OS prerequisite",4);
    s=SnapshotErrors();
    check(s.count==1 && std::strcmp(s.entries[0].id,"FG-206")==0 && s.entries[0].native==4);
    check(AffectsGeneration(Code::HardwareScheduling));
    check(std::strstr(Description(Code::HardwareScheduling),"HAGS")!=nullptr);
    check(std::strstr(s.entries[0].action,"restart Windows")!=nullptr);
    ClearError(Code::HardwareScheduling);check(SnapshotErrors().count==0);
    std::printf("%d runtime diagnostic checks: %d failures\n",checks,failures);
    return failures?1:0;
}
