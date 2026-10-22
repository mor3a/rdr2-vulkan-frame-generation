#pragma once
#include <windows.h>
#include <d3d12.h>
namespace fgvk::xess_pacing {
// Verified software-pacer deadline compatibility adaptation, derived from the
// pinned MIT MFG-Unlocker reference. No second presenter or CPU wait is added.
bool Install(HMODULE provider) noexcept;
bool Bind(void* context,ID3D12Device* nativeDevice) noexcept;
void Reset(void* context) noexcept;
void Unbind(void* context) noexcept;
}
