#pragma once
namespace fgvk {
struct MenuStartup {
  bool ready=false;
  void Reset(){ready=false;}
  void Presented(bool evaluated,bool success){if(evaluated && success)ready=true;}
};
}
