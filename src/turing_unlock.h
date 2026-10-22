#pragma once
#include <cstddef>
namespace fgvk {
bool VerifyTuringUnlock040(const wchar_t* directory,char* error,size_t capacity);
bool LoadTuringUnlock040(const wchar_t* directory,char* error,size_t capacity,unsigned preset=0);
int TuringUnlock040LoadedPreset();
}
