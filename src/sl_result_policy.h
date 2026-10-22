#pragma once
#include <sl_result.h>
namespace fgvk {
// The SDK can return usable options/state together with a budget warning.
// Do not retry an accepted SetOptions or discard valid GetState telemetry.
constexpr bool AcceptedSLResult(sl::Result r){return r==sl::Result::eOk || r==sl::Result::eWarnOutOfVRAM;}
}
