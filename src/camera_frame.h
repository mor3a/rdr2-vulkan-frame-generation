#pragma once
#include <sl_consts.h>
#include <cstdint>
#include "experiment_policy.h"
namespace fgvk {
struct FrameCameraInfo {uint64_t token{},targetUs{},poseUs{};double ageMs{};CameraSource requested{},actual{};bool fresh{};};
bool PrepareFrameCamera(uint64_t token,sl::Constants&,float aspect,bool inverted,bool reset);
bool ResolveFrameCamera(uint64_t token,sl::Constants&);
void SetDerivedFrameResult(uint64_t token,bool accepted);
void CommitFrameCamera(uint64_t token,bool presented);
void ResetFrameCameraHistory();
FrameCameraInfo ReadFrameCameraInfo();
}
