#pragma once
#include <cstdio>
#include <cstring>
#include <cstdint>
namespace fgvk {
struct UpdateManifest {char version[32]{};char url[256]{};};
inline bool ValidUpdateLabel(const char* value){
  if(!value || !*value)return false;
  size_t count=0;bool visible=false;
  for(;*value;++value){const auto c=static_cast<unsigned char>(*value);
    if(++count>=32 || c<32 || c>126 || c=='\\' || c=='"')return false;
    visible|=c!=' ';
  }return visible;
}
// Nexus version labels are publisher text, not necessarily semantic versions.
// Any different valid label is a notice, including older releases/beta suffixes.
inline bool UpdateVersionDiffers(const char* latest,const char* installed){
  return ValidUpdateLabel(latest) && ValidUpdateLabel(installed) && strcmp(latest,installed)!=0;
}
inline bool ParseVersion(const char* s,uint32_t (&v)[4]){
  if(!s || !*s)return false;
  if(*s=='v')++s;
  unsigned part=0,digits=0;for(;*s;++s){
    if(strcmp(s," Beta")==0 || strcmp(s," beta")==0 || strcmp(s,"-beta")==0 || strcmp(s,"beta")==0)break;
    if(*s=='.'){if(!digits || part==3)return false;++part;digits=0;continue;}
    if(*s<'0'||*s>'9'||++digits>6)return false;
    v[part]=v[part]*10+unsigned(*s-'0');
  }
  // This release's public label is 0.6beta, following 0.60.1beta. Keep that
  // explicit project alias symmetric for local and Nexus values; other version
  // components retain integer ordering (e.g. 0.100 > 0.99).
  if(digits && part==1 && v[0]==0 && v[1]==6){v[1]=60;v[2]=2;}
  // Public 0.7.x continues the former 0.60.x series (legacy 0.70.x ordering).
  if(digits && v[0]==0 && v[1]==7)v[1]=70;
  return digits && part>=1;
}
inline bool NewerVersion(const char* latest,const char* current){uint32_t a[4]{},b[4]{};if(!ParseVersion(latest,a)||!ParseVersion(current,b))return false;for(int i=0;i<4;++i){if(a[i]!=b[i])return a[i]>b[i];}return false;}
inline bool VersionsDiffer(const char* a,const char* b){return NewerVersion(a,b)||NewerVersion(b,a);}
inline bool NexusURL(const char* s){
  const char* prefix="https://www.nexusmods.com/reddeadredemption2/mods/";
  if(strncmp(s,prefix,strlen(prefix))!=0)return false;
  s+=strlen(prefix);if(!*s)return false;
  for(;*s;++s)if(*s<'0'||*s>'9')return false;
  return true;
}
inline const char* SkipJSONSpace(const char* p){while(*p==' '||*p=='\r'||*p=='\n'||*p=='\t')++p;return p;}
inline const char* JSONValue(const char* data,const char* key){
  const char* p=strstr(data,key);if(!p || strstr(p+strlen(key),key))return nullptr;
  p=SkipJSONSpace(p+strlen(key));if(*p++!=':')return nullptr;return SkipJSONSpace(p);
}
inline bool ParseNexusResponse(const char* data,uint32_t modId,UpdateManifest& out){
  if(!data || !modId || strlen(data)>4096 || strstr(data,"\"errors\"") || !strstr(data,"\"legacyModsByDomain\"") || !strstr(data,"\"nodes\""))return false;
  auto p=JSONValue(data,"\"version\"");auto id=JSONValue(data,"\"modId\"");if(!p || !id || *p++!='\"')return false;
  auto end=strchr(p,'\"');if(!end || end==p || size_t(end-p)>=sizeof(out.version))return false;
  UpdateManifest m;memcpy(m.version,p,end-p);if(!ValidUpdateLabel(m.version))return false;
  uint64_t actual=0;unsigned digits=0;while(*id>='0' && *id<='9'){if(++digits>10)return false;actual=actual*10+unsigned(*id++-'0');}
  id=SkipJSONSpace(id);if(!digits || actual!=modId || (*id!=',' && *id!='}'))return false;
  snprintf(m.url,sizeof(m.url),"https://www.nexusmods.com/reddeadredemption2/mods/%u",modId);
  out=m;return true;
}
}
