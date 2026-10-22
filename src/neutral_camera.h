#pragma once
#include <sl_consts.h>
#include <cmath>

namespace fgvk {
// Neutral metadata for an MV-driven DLSS-G experiment. These are nominal
// projection values, not measurements of RDR2's render camera. This function
// neither reads nor changes the game's camera and never calls ScriptHook.
inline bool BuildNeutralCamera(sl::Constants& c,float aspect,bool invertedDepth,bool reset){
  if(!std::isfinite(aspect) || aspect<=0.f)return false;
  constexpr float nearPlane=.05f,farPlane=10000.f;
  constexpr float fov=1.0471975511965976f; // Nominal vertical FOV: 60 degrees.
  const float focal=1.f/std::tan(fov*.5f);
  const float a=invertedDepth?nearPlane/(nearPlane-farPlane):farPlane/(farPlane-nearPlane);
  const float b=invertedDepth?nearPlane*farPlane/(farPlane-nearPlane):-nearPlane*farPlane/(farPlane-nearPlane);
  auto identity=[](){sl::float4x4 m{};m.setRow(0,{1,0,0,0});m.setRow(1,{0,1,0,0});m.setRow(2,{0,0,1,0});m.setRow(3,{0,0,0,1});return m;};
  c.cameraViewToClip.setRow(0,{focal/aspect,0,0,0});
  c.cameraViewToClip.setRow(1,{0,focal,0,0});
  c.cameraViewToClip.setRow(2,{0,0,a,1});
  c.cameraViewToClip.setRow(3,{0,0,b,0});
  c.clipToCameraView.setRow(0,{aspect/focal,0,0,0});
  c.clipToCameraView.setRow(1,{0,1.f/focal,0,0});
  c.clipToCameraView.setRow(2,{0,0,0,1.f/b});
  c.clipToCameraView.setRow(3,{0,0,1,-a/b});
  c.clipToPrevClip=identity();c.prevClipToClip=identity();c.clipToLensClip=identity();
  c.cameraPos={0,0,0};c.cameraRight={0,1,0};c.cameraUp={0,0,1};c.cameraFwd={1,0,0};
  c.cameraNear=nearPlane;c.cameraFar=farPlane;c.cameraFOV=fov;c.cameraAspectRatio=aspect;
  c.cameraPinholeOffset={0,0};c.depthInverted=invertedDepth?sl::Boolean::eTrue:sl::Boolean::eFalse;
  c.reset=reset?sl::Boolean::eTrue:sl::Boolean::eFalse;
  return true;
}
}
