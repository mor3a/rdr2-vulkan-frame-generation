#pragma once
namespace fgvk {
struct MenuStartup {
  bool ready=false;
  void Reset(){ready=false;}
  // UI does not depend on DLSS, camera registration or provider readiness.
  void Presented(bool /*evaluated*/,bool success){if(success)ready=true;}
};
}
