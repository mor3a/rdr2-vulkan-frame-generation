#pragma once
#include <cstdint>
namespace fgvk {
bool PerformanceProbeBegin(bool world);
void PerformanceProbeDisplayed(uint32_t count);
void PerformanceProbeFrame(double pacingMs,double presentMs,double markerMs);
}
