#include "../src/frame_history.h"
using fgvk::FrameHistory;
constexpr bool CheckHistory() {
  FrameHistory h;
  auto first=h.Select(1,1,1,1,1);
  if(!first.valid || !first.Reset()) return false;
  h.Complete(first,true,true);
  auto normal=h.Select(2,2,2,2,2);
  if(!normal.valid || normal.Reset()) return false;
  h.Invalidate(FrameHistory::Association);
  auto corrected=h.Select(2,2,2,2,2);
  if(!(corrected.reasons&FrameHistory::Association)) return false;
  h.Complete(corrected,true,true);
  if(h.Select(3,3,3,3,3).Reset()) return false;
  h.Invalidate(FrameHistory::QueueDrop);
  auto drop=h.Select(4,4,4,4,4);
  if(!(drop.reasons&FrameHistory::Gap) || !(drop.reasons&FrameHistory::QueueDrop)) return false;
  h.Complete(drop,true,false);
  auto retry=h.Select(5,5,5,5,5);
  if(!(retry.reasons&FrameHistory::PresentFailure)) return false;
  h.Complete(retry,true,true);
  auto inFlight=h.Select(6,6,6,6,6);
  h.Invalidate(FrameHistory::Missing);
  h.Complete(inFlight,true,true);
  if(!(h.Select(7,7,7,7,7).reasons&FrameHistory::Missing)) return false;
  auto valid=h.Select(7,7,7,7,7); h.Complete(valid,true,true);
  if(h.Select(6,6,6,6,6).valid || h.Select(7,7,7,7,7).valid) return false;
  auto wrong=h.Select(8,8,9,8,8);
  if(wrong.valid) return false;
  h.Complete(wrong,true,true);
  if(h.Last()!=7 || !h.Select(9,9,9,9,9).Reset()) return false;
  if(h.Select(9,9,9,8,9).valid || h.Select(9,9,9,9,8).valid || h.Select(8,9,9,9,9).valid) return false;
  FrameHistory placeholder;
  placeholder.Complete(placeholder.Select(0,0,0,0,0),false,true);
  if(placeholder.Last()!=0 || !placeholder.Select(1,1,1,1,1).Reset()) return false;
  auto rejected=h.Select(9,9,9,9,9); h.Complete(rejected,false,true);
  return h.Last()==7 && (h.Select(10,10,10,10,10).reasons&FrameHistory::Rejected);
}
static_assert(CheckHistory(),"Frame history and NVIDIA index contract regression");
