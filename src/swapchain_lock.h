#pragma once
#include <windows.h>
namespace fgvk {
// Hold through the backend present: its wait semaphore belongs to the menu.
// Recursive entry permits same-thread backend callbacks during recreation.
class SwapchainLock {
  CRITICAL_SECTION section;
public:
  SwapchainLock(){InitializeCriticalSection(&section);}
  ~SwapchainLock(){DeleteCriticalSection(&section);}
  void Enter(){EnterCriticalSection(&section);}
  void Leave(){LeaveCriticalSection(&section);}
  SwapchainLock(const SwapchainLock&)=delete;
  SwapchainLock& operator=(const SwapchainLock&)=delete;
};
inline SwapchainLock& GameSwapchainLock(){static SwapchainLock lock;return lock;}
class SwapchainScope {
public:
  SwapchainScope(){GameSwapchainLock().Enter();}
  ~SwapchainScope(){GameSwapchainLock().Leave();}
  SwapchainScope(const SwapchainScope&)=delete;
  SwapchainScope& operator=(const SwapchainScope&)=delete;
};
}
