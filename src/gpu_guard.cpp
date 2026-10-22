#include "gpu_guard.h"
#include "log.h"
#include <windows.h>
#include <atomic>

namespace {
std::atomic<bool> blocked{false};
}

namespace fgvk {
bool IsGpuBlocked() noexcept {
    return blocked.load(std::memory_order_acquire);
}

[[noreturn]] void BlockUnsupportedGpu(bool simulated) {
    if (blocked.exchange(true, std::memory_order_acq_rel)) {
        // A concurrent device attempt must not resume startup while the first
        // caller owns the warning. That caller terminates this process.
        for(;;) Sleep(INFINITE);
    }
    Log("%sAMD unsupported: graphics startup blocked; remove this mod; game exits after warning is dismissed",
        simulated ? "TEST MODE: simulated AMD; " : "");
    const wchar_t* text = simulated
        ? L"TEST MODE: Simulated unsupported AMD GPU.\n\n"
          L"AMD GPUs are not supported by this mod build. Graphics startup is blocked.\n\n"
          L"Remove these mod files from the folder beside RDR2.exe:\n"
          L"RDR2VulkanFG.asi\nRDR2VulkanFGCamera.asi\nRDR2VulkanFG.ini\nRDR2VulkanFG (folder)\n\n"
          L"Closing this message will close Red Dead Redemption 2.\n"
          L"For this test, remove SimulateUnsupportedAMD=1 from the INI before restarting."
        : L"AMD GPUs are not supported by this mod build. Graphics startup is blocked.\n\n"
          L"Remove these mod files from the folder beside RDR2.exe:\n"
          L"RDR2VulkanFG.asi\nRDR2VulkanFGCamera.asi\nRDR2VulkanFG.ini\nRDR2VulkanFG (folder)\n\n"
          L"Closing this message will close Red Dead Redemption 2.\n"
          L"This warning will appear again on the next launch while this mod remains installed.";
    // The owning graphics initialization call waits here; no device or FG
    // context is created. Never suspend arbitrary game/driver threads or show
    // this dialog under the Windows DLL loader lock.
    const int result=MessageBoxW(nullptr,text,L"RDR2 Vulkan FG - Remove unsupported mod",
        MB_OK | MB_ICONERROR | MB_TASKMODAL | MB_SETFOREGROUND | MB_TOPMOST);
    Log("Unsupported GPU warning dismissed (result=%d); deliberately closing this RDR2 process",result);
    // The user explicitly requests a forced close. Avoid DLL teardown on a
    // partially initialized graphics thread and affect only the current game.
    TerminateProcess(GetCurrentProcess(),ERROR_NOT_SUPPORTED);
    ExitProcess(ERROR_NOT_SUPPORTED); // Fallback if TerminateProcess fails.
}
}
