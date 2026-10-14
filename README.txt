RDR2 Vulkan Frame Generation 0.30
made by mor3a

Main downloads and updates: https://www.nexusmods.com/reddeadredemption2/mods/10997
This repository contains public source code and issue tracking.

DLSS-G and native AMD FSR3 frame generation using RDR2's DLSS depth and motion
vectors. This is a beta RDR2 integration, not a universal Vulkan game mod.
Use Vulkan, DLSS enabled, SDR and Story Mode.

MANUAL INSTALLATION
Close RDR2. For a clean update, remove the previous RDR2VulkanFG ASI files,
INI and runtime folder, then copy both ASI files, RDR2VulkanFG runtime folder
and RDR2VulkanFG.ini from the Nexus download beside RDR2.exe.
Keep your existing ASI loader and compatible ScriptHookRDR2 installation.
Remove version.dll or winmm.dll only if they came from an older release of
this mod. Preserve files required by other mods.
Obtain ScriptHookRDR2 from its original author:
http://www.dev-c.com/rdr2/scripthookrdr2/

MENU AND CONTROLS
Insert opens/closes the draggable menu inside the game's Vulkan image.
Drag the title bar to move it. The GPU model and unlock details are displayed.
The footer reads made by mor3a

Main downloads and updates: https://www.nexusmods.com/reddeadredemption2/mods/10997
This repository contains public source code and issue tracking.. No separate desktop menu window is created.
FG temporarily pauses while the menu is open and resumes after closing it if
FG is enabled. Numpad * toggles FG. End cycles DLSS-G multipliers.
The provider selector saves DLSSG or FSR3 for the next launch; changing backend
requires a restart. Off and the frame generation checkbox apply immediately.
DLSSGFrames=1/2 requests 2x/3x, subject to runtime/driver support.
FSR3 is native 2x only. FSR3 3x and XeSS FG are not included.

FPS COUNTER
The menu enables an FPS counter in the top-right corner.
With FG off it shows rendered FPS; with working FG it shows rendered /
displayed FPS. It samples game presents and backend counters rather than
ImGui's refresh rate. Brief telemetry gaps retain the last measured value. Sustained loss is shown as --. A requested multiplier
alone is not proof of actual generated frames.

OPTIMIZATIONS
The extra GPU wait before DLSS-G present is disabled by default. The native
backend still owns presentation synchronization. Optional PresentPacing=1
retains the older wait if it suits another configuration better.
Short cross-thread DLSS input gaps no longer immediately trigger suspension.
FG resources remain allocated during off/on transitions. FSR skips new depth/MV
preparation when disabled and resets its interpolation history when resumed.
The optional UI skips busy command buffers rather than waiting on the CPU.
Hotkeys only change settings while the game has focus. Settings are written
when changed. BenchmarkFG=1 before launch enables an optional fixed-scene
126-second comparison; leave 0 for normal use. See VALIDATION.txt for results.

GPU SUPPORT AND LIMITATIONS
Tested hardware: RTX 3050 Laptop, 4 GB VRAM. RTX 30 uses a private unlock core;
RTX 40/50 skip it. RTX 20 uses an experimental Turing route and is hardware
untested and disabled by default; EnableExperimentalRTX20=1 explicitly opts in. Loading an unlock does not
prove support. FSR3 does not require the unlock. DLSS-G 4x is not offered in this release.
FSRHUDCapture=1 and FSRHUDStrict=1 reduce HUD artifacts, but slight HUD ghosting
can remain. DLSS-G did not show that artifact in the tested configuration.
FSRHUDProtection=0 keeps the older rectangular fallback disabled.
HDR, live resolution changes and other GPUs are not runtime validated.
This release cannot eliminate the GPU cost of generating frames. More displayed
frames can come with lower rendered FPS and increased input latency. Choose 2x
for a lower render cost than 3x; a higher multiplier is not automatically better.

TROUBLESHOOTING AND REMOVAL
Read RDR2VulkanFG.log when reporting problems. The old SafeStartup marker no
longer disables the mod; legacy SafeStartup INI entries are ignored.
FG suspends without
valid DLSS inputs during loading/menu transitions.
To remove the mod, close the game and remove the two RDR2VulkanFG ASIs,
RDR2VulkanFG.ini and the RDR2VulkanFG runtime folder. Restore backed-up
version.dll/version.ini if replaced, or remove them only if no other mod uses
that loader. Preserve other mods, ScriptHook files and the game executable.

SOURCE AND LICENSES
The separate source ZIP contains src, tests, vendor and Build.ps1.
Build using Zig 0.16.0:
Build.ps1 -ZigPath <path-to-zig.exe> -DependencyRoot <dependency-folder> -RunChecks
Git fetches pinned Detours, Vulkan-Headers and Streamline revisions. Runtime
DLLs are distributed in the runtime ZIP. Preserve all third-party copyrights
and licenses under licenses and the runtime folder.
Upstream unlock: https://github.com/mcsoderh/RTX30MFG-Unlock/releases/tag/v1.2.2

Known UI limitation: transient text artifacts may appear during menu changes. See VALIDATION.txt for the tested scope.

PUBLIC SOURCE
The Nexus runtime update checker is excluded. This source builds without update checking.
Menu settings include an FPS limit and a configurable menu hotkey.
