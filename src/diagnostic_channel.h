#pragma once
#include <windows.h>
#include <cwchar>
namespace fgvk {
struct DiagnosticSnapshot {
  volatile LONG busy;
  DWORD threadId;
  DWORD reason; // 1 = first-chance exception, 2 = stalled presentation
  EXCEPTION_RECORD exception;
  CONTEXT context;
};
inline void DiagnosticName(wchar_t* output,size_t count,const wchar_t* kind,DWORD pid){
  swprintf(output,count,L"Local\\RDR2VulkanFG-%s-%lu",kind,pid);
}
}
