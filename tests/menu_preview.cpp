#include "menu_error_preview.h"
#include <cstring>
#include <cstdio>
int main(){using namespace RuntimeErrors;fgvk::MenuErrorPreview preview;
 preview.Set(Code::InputState);auto s=preview.Merge(SnapshotErrors());if(s.count!=1 || SnapshotErrors().count!=0)return 1;
 ReportError(Code::InputState,"real failure",42);s=preview.Merge(SnapshotErrors());if(s.count!=1 || s.entries[0].native!=42)return 2;
 preview.Clear();s=preview.Merge(SnapshotErrors());if(s.count!=1 || strcmp(s.entries[0].detail.data(),"real failure"))return 3;
 preview.Set(Code::InputState);ClearError(Code::InputState);if(SnapshotErrors().count!=0 || preview.Merge(SnapshotErrors()).count!=1)return 4;
 preview.Clear();if(preview.Merge(SnapshotErrors()).count!=0)return 5;
 puts("Menu simulation isolation / real error precedence / independent recovery: PASS");}
