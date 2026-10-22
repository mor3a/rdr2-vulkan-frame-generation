#include "update_manifest.h"
int main(){using namespace fgvk;UpdateManifest m;int failed=0,checks=0;auto check=[&](bool b){++checks;if(!b)++failed;};
const char* good="{\"data\":{\"legacyModsByDomain\":{\"nodes\":[{\"version\":\"0.24 Beta\",\"modId\":123,\"status\":\"published\"}]}}}";
check(ParseNexusResponse(good,123,m));check(NexusURL(m.url));check(!ParseNexusResponse(good,124,m));check(!ParseNexusResponse(good,0,m));
check(VersionsDiffer("0.24","0.23"));check(VersionsDiffer("0.22","0.23"));check(!VersionsDiffer("v0.23 Beta","0.23"));check(NewerVersion("0.100","0.99"));
check(!ParseNexusResponse("{\"errors\":[{}],\"data\":null}",123,m));
check(!ParseNexusResponse("{\"data\":{\"legacyModsByDomain\":{\"nodes\":[]}}}",123,m));
check(ParseNexusResponse("{\"legacyModsByDomain\":{\"nodes\":[{\"version\":\"abc\",\"modId\":123}]}}",123,m));
check(!ParseNexusResponse("{\"legacyModsByDomain\":{\"nodes\":[{\"version\":\"0.24\",\"version\":\"0.25\",\"modId\":123}]}}",123,m));
check(ParseNexusResponse("{\"data\":{\"legacyModsByDomain\":{\"nodes\":[{\"version\":\"0.24\",\"modId\":123,\"status\":\"not_published\"}]}}}",123,m));
check(VersionsDiffer("0.30","0.32beta"));check(!VersionsDiffer("0.32","0.32beta"));check(!NewerVersion("0.32invalid","0.30"));
check(!NewerVersion("0.50","0.60beta"));check(NewerVersion("0.61","0.60beta"));check(!NewerVersion("0.60beta","0.60beta"));
check(!NewerVersion("0.50","0.6beta"));check(NewerVersion("0.61","0.6beta"));check(NewerVersion("0.6beta","0.60beta"));check(!NewerVersion("0.60.1beta","0.6beta"));check(!VersionsDiffer("0.6beta","0.60.2beta"));
check(NewerVersion("0.7.0beta","0.6beta"));check(!NewerVersion("0.60.1beta","0.7.0beta"));check(NewerVersion("0.7.1beta","0.7.0beta"));check(!NewerVersion("0.50","0.7.0beta"));
check(UpdateVersionDiffers("0.50","0.7.0beta"));check(UpdateVersionDiffers("0.8.0beta","0.7.0beta"));check(!UpdateVersionDiffers("0.7.0beta","0.7.0beta"));
check(UpdateVersionDiffers("0.7.0","0.7.0beta"));check(UpdateVersionDiffers("0.7.0beta Hotfix 1","0.7.0beta"));
check(!UpdateVersionDiffers("","0.7.0beta"));check(!UpdateVersionDiffers(nullptr,"0.7.0beta"));check(!ValidUpdateLabel("   "));check(!ValidUpdateLabel("0.7\n"));
check(UpdateVersionDiffers("0.7test","0.7"));check(UpdateVersionDiffers("0.7 test","0.7"));check(!UpdateVersionDiffers("0.7","0.7"));
check(ParseNexusResponse("{\"data\":{\"legacyModsByDomain\":{\"nodes\":[{\"version\":\"0.7test\",\"modId\":10997,\"status\":\"published\"}]}}}",10997,m) && UpdateVersionDiffers(m.version,"0.7"));
std::printf("%d Nexus response/version checks: %d failures\n",checks,failed);return failed?1:0;}
