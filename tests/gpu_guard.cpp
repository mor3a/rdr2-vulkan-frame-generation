#include "../src/gpu_guard.h"

static_assert(fgvk::IsUnsupportedGpuVendor(0x1002u), "AMD must be blocked");
static_assert(!fgvk::IsUnsupportedGpuVendor(0x10DEu), "NVIDIA must not be blocked");
static_assert(!fgvk::IsUnsupportedGpuVendor(0x8086u), "Intel must not be blocked");
static_assert(!fgvk::IsUnsupportedGpuVendor(0u), "Unknown vendor must not be blocked");
static_assert(!fgvk::IsUnsupportedGpuVendor(0xFFFFu), "Unknown vendor must not be blocked");
