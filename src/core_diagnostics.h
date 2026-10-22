#pragma once
#include "experiment_metrics.h"
namespace fgvk {
bool SetCoreIntervalTracing(bool enabled);
bool CoreIntervalTracing();
experiment::Distribution ReadCoreIntervals();
}
