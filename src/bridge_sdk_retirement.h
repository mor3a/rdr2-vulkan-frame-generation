#pragma once
#include <utility>
namespace fgvk {
// The SDK remains the presentation owner while the caller drops its extra COM
// reference. XeFG Destroy refuses live external swapchain references (-19).
// Client textures are still retained until this callback has retired the SDK.
template<class Presentation,class Shutdown>
bool ReleasePresentationBeforeSDK(Presentation*& extra,Shutdown&& shutdown){
 if(extra){auto* previous=extra;extra=nullptr;previous->Release();}
 return std::forward<Shutdown>(shutdown)();
}
}
