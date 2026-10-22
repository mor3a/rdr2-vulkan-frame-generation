RDR2 Vulkan Frame Generation 0.82

Requirements
Vulkan, in-game DLSS, a compatible NVIDIA RTX GPU, and an ASI loader.
Enable Hardware-accelerated GPU scheduling in Windows.
ScriptHookRDR2 is not required with the default settings.

Installation
Close the game and extract the files beside RDR2.exe.
When upgrading, remove the old RDR2VulkanFGCamera.asi if present.
Use the ASI, INI and RDR2VulkanFG folder from the same package.

Controls
Insert opens the menu. Numpad * toggles frame generation. End cycles bridge multipliers.
DLSS-G uses Native Vulkan by default. XeSS FG remains available as an alternative.
Available multipliers depend on the GPU and provider.

Restart FG / Apply changes resets generation and applies supported preset changes.
A brief pause is expected. Provider changes involving Native DLSS-G require a game restart.
Backend, camera source and ReflexSleep can be changed in the INI with the game closed.
ReflexSleep=0 is the default for Native DLSS-G and does not affect XeSS pacing.

Preset feedback shows what the provider or driver reports; availability may vary.
Detailed logging and diagnostics are disabled by default. Update checks run automatically.
