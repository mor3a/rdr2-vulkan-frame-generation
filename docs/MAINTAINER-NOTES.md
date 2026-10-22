# 0.82 implementation and current status

## Routes

`vkhooks.cpp` owns Vulkan interception, device/swapchain ownership and present routing. Native DLSS-G uses the interposer-owned Vulkan device and queues; do not also register it with `slSetVulkanInfo` or perform a second queue rewrite. `slboot.cpp` initializes private, matched Streamline plugins with manual hooking and frame-based tags.

`inputs.cpp` observes successful game DLSS-SR creation/evaluation, captures depth and motion vectors, and associates recording/submission/presentation with the exact frame token. `frame_association.h`, frame history and capture helpers protect against reordered or stale inputs. Do not substitute the newest available frame when exact association fails.

`xess_provider.cpp` and the `xess_bridge_*` files contain shared Vulkan/D3D12 bridge machinery for both XeSS and DLSSGBridge. Their names do not imply XeSS-only behavior. `dlssg_bridge_runtime.cpp` and `xess_bridge_runtime.cpp` implement provider-specific SDK calls. Native changes must not unnecessarily disturb this route.

Native HUD protection captures matching pre-HUD color into GPU snapshots and tags only associated submissions. This is a renderpass/dynamic-rendering heuristic; absent captures do not establish that the HUD is protected. Do not re-enable tagging the raw pre-tonemapped SR output as final HUD-less color without validating its color space and stage.

CameraSource defaults to Neutral. Legacy Sampled is normalized to Neutral. The active mod makes no ScriptHook camera calls or registration. Motion is provided by the game's NGX inputs. Derived camera support remains optional and is not the default.

## Defaults and lifecycle

Native DLSS-G, 2x, ReflexSleep=0, native HUD protection on, extra present pacing off, 100ms DLSS freshness grace, three idle presents, 60 initial valid frames, and one valid frame to re-enable after prior initialization. The public menu defaults to Insert; an existing INI retains its saved key. Update checks are mandatory; any different valid Nexus version label is a notice, including text suffixes.

Native restart disables generation, retires an Off presentation and rebuilds generation input/resources without rebuilding the Vulkan device or swapchain. Recording in progress returns Busy; a five-second timeout becomes a reported error. Native/bridge presentation switches require restarting the game. RTX20 preset changes require restarting the game.

FSR3 is disabled in the current configuration. AMD is unsupported. The AMD guard currently displays a modal warning and terminates the current game process after acknowledgement; it is not a passive warning. Keep any simulation keys out of public INIs.

## Native queue defect and correction

The stock 2.14.1 wrapper can skip adding compute queues when the host exhausts its preferred family, then incorrectly append a duplicate family entry. For the observed host requests (family0/count1, family2/count8, family1/count2), this produces family2/count8 plus family2/count2, although the family has capacity8. This violates queue-create uniqueness/capacity requirements and is a concrete startup defect. A short failing log does not independently prove the exact final crash instruction.

The patch accounts for the complete host request, reserves separate SDK queue indices, and chooses an alternative compute-capable family when the dedicated one is full. It preserves original game indices, priorities, flags and pNext. The tested plan becomes family0/count4 (game index0, SDK graphics1, SDK compute2–3), family2/count8 unchanged, family1/count2 unchanged and optical-flow family5/count1. It rejects unavailable capacity rather than oversubscribing a family or silently sharing the game's queues.

The same problematic upstream allocation logic was found in the newer 2.14.16 wrapper during investigation. Merely replacing DLLs with that release was not established as a correction. The shipped plugins remain at the matched 2.14.1 set.

Allocation tests, isolated plugin load and real Vulkan instance/device creation passed on an RTX3050 Laptop. All exported interposer symbols match the original DLL. This does not validate final presentation, RTX5070Ti driver behavior or RTX20 gameplay.

## Open reports

Cutscenes: there is no explicit cutscene-disable branch. Native generation suspends when successful valid DLSS-SR input becomes stale or when present-associated inputs remain unavailable. It resumes on a fresh valid matched frame. A prerecorded scene or one that stops DLSS has no valid input for this integration. Current evidence does not distinguish that expected case from a transition defect in the reported scene. Suspension records now distinguish those paths and include input age; no verified cutscene fix is claimed.

RTX20: private dlssg_for_sm86 0.4.0 integration is opt-in, with the D3D12 FG bridge while the game stays on Vulkan. CPU loader/config checks are not gameplay validation. Avoid mixing this proxy with a second unlock stack.

Presentation: historical high-FPS-but-poor-cadence, restart, HDR and graphics-setting reports must be evaluated against their exact version/backend. Native/Neutral/ReflexSleep=0 was reported as substantially better by the development user, but universal performance, perfect HUD isolation and crash freedom are not established.

External window-procedure recursion: an installed ScriptHook interface was observed in a repeating USER32/ScriptHook chain. This was excluded from the current change. Do not add a blanket WndProc rewrite, disable unrelated mods or blame an exported symbol offset as if it were a proven source function.

Next validation: affected RTX5070Ti startup; RTX20 opt-in gameplay; cutscene entry/exit with fresh-input logs; Native/bridge cadence and restart separately; DLSS quality/resolution/VSync/HDR changes; multi-monitor refresh selection. In-game testing is separate from build and fixture results.
