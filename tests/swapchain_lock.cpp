#include "swapchain_lock.h"
#include <thread>
#include <cstdio>
int main(){
  int failed=0;
  HANDLE waiting=CreateEventW(nullptr,TRUE,FALSE,nullptr);
  HANDLE entered=CreateEventW(nullptr,TRUE,FALSE,nullptr);
  std::thread worker;
  {
    fgvk::SwapchainScope present;
    {fgvk::SwapchainScope callback;}
    worker=std::thread([&]{
      SetEvent(waiting);
      fgvk::SwapchainScope recreate;
      SetEvent(entered);
    });
    if(WaitForSingleObject(waiting,3000)!=WAIT_OBJECT_0)++failed;
    if(WaitForSingleObject(entered,100)!=WAIT_TIMEOUT)++failed;
  }
  if(WaitForSingleObject(entered,3000)!=WAIT_OBJECT_0)++failed;
  worker.join();CloseHandle(waiting);CloseHandle(entered);
  std::printf("Swapchain lifecycle: recursive callback, blocked teardown, resumed teardown: %d failures\n",failed);
  return failed?1:0;
}
