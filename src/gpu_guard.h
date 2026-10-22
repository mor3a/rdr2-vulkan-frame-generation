#pragma once
#include <cstdint>

namespace fgvk {
constexpr bool IsUnsupportedGpuVendor(std::uint32_t vendor) noexcept {
    return vendor == 0x1002u;
}

bool IsGpuBlocked() noexcept;
// Called only from the owned graphics-device initialization call, outside
// DllMain. Blocks startup on a modal warning, then terminates this game process.
[[noreturn]] void BlockUnsupportedGpu(bool simulated = false);
}
