# RDR2 Vulkan Frame Generation

Version 0.82 source for Red Dead Redemption 2. DLSS-G uses Native Vulkan by default; XeSS FG uses a Vulkan/D3D12 bridge.

[Downloads and installation package](https://www.nexusmods.com/reddeadredemption2/mods/10997)

## Requirements

Windows, Vulkan, in-game DLSS, a compatible NVIDIA RTX GPU, an ASI loader, and Hardware-accelerated GPU scheduling enabled. ScriptHookRDR2 is not required. FSR3 is currently disabled. AMD GPUs are not supported.

Extract the complete runtime package beside `RDR2.exe` with the game closed. Preserve your ASI loader and other mods. Remove an old `RDR2VulkanFGCamera.asi` if present. Source builds need the runtime files from the matching package.

## Controls

Insert opens the menu. Numpad `*` toggles frame generation. The menu key is configurable. Existing INI files retain their saved key.

DLSS-G uses Native Vulkan by default. `DLSSGBackend=Bridge` selects the D3D12 presentation bridge on the next game launch. XeSS FG remains on its bridge path. Multipliers depend on the provider and runtime-reported capacity; XeSS 3x/4x are experimental.

`Restart FG / Apply changes` rebuilds generation resources. Switching between Native and bridge presentation requires restarting the game. RTX20 preset changes also require a game restart.

RTX20 DLSS-G is experimental and opt-in: add `EnableExperimentalRTX20=1` under `[fgvk]` with the game closed. This selects the private 0.4.0 unlock and D3D12 bridge. Loading the unlock does not establish gameplay support on every RTX20 card.

## Source and validation

The current `src/`, `tests/` and `vendor/` files match the 0.82 build source. The Nexus update checker is included. Runtime DLLs and compiled packages are distributed separately; building the ASI does not assemble a runtime installation.

[Build instructions](docs/BUILD.md) · [Architecture and current status](docs/MAINTAINER-NOTES.md) · [0.82 changes](CHANGELOG-0.82.txt)

0.82 includes a Streamline queue-allocation correction for Native Vulkan startup. Allocation regressions and isolated device creation passed on the development RTX3050 Laptop. Validation on the reported RTX5070Ti driver and RTX20 gameplay is still required. The cutscene suspension report is under investigation.

The FPS counter combines game-present and provider telemetry. An increased count alone does not prove uniform display cadence. Frame generation has GPU, memory and presentation overhead; performance varies by hardware, settings and provider.

## License

Original project code is MIT licensed. Third-party components retain their own terms; see `LICENSE.txt`, `ACKNOWLEDGEMENTS.txt`, `licenses/` and vendored notices.
