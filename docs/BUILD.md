# Building 0.82

Use Windows PowerShell, Git and Zig 0.16.0. The main ASI uses the GNU Windows target through Zig. The custom Streamline interposer must use MSVC and its Microsoft C++ runtime, matching the production plugins.

## Main ASI

From the repository root:

```powershell
.\Build.ps1 -ZigPath 'C:\tools\zig\zig.exe'
```

The repository wrapper defaults dependencies to `.deps`; pass `-DependencyRoot` to use another directory. Missing dependency checkouts are fetched and checked out at:

| Dependency | Revision |
| --- | --- |
| Microsoft Detours | `1dbd4ea281d73d478aa8d09a8b83fbd265f715f7` |
| Khronos Vulkan-Headers | `c46850864f4661461b0f6cb9922c058ffea4915e` (1.4.365) |
| NVIDIA Streamline | `2122257e0fce486f91b385aa63b9a09b0a34b363` (2.14.1) |

Intel headers are vendored from `intel/xess` revision `de0fb9c1c510661c571164e1418ceca8101dab69`. XeSS host/pacing reference attribution records `dashdogy/MFG-Unlocker` revision `2b52823e3769c31cf52d3b368c6963cff8fc5344`. Keep the vendored source and notices together.

The script writes `.deps/rdr2-fg-0.82-build/RDR2VulkanFG.asi`, copies it to the repository root, and builds `RDR2VulkanFGDiagnostics.dll`. The ASI build includes compile-time regressions. Do not use `-RunChecks` casually: that option also runs deliberately crashing diagnostic fixtures. Ordinary compilation does not launch RDR2.

The original 0.82 build had the same application sources, with dependency defaults pointing to its surrounding workspace. This repository changes only dependency setup to support a clean checkout; it does not guarantee byte-identical compiler output.

## Modified Streamline interposer

The bundled 0.82 interposer includes `patches/streamline-2.14.1-queue-allocation.patch`. A stock SDK interposer does not contain this correction. Updating only the ASI will not apply it.

Use an x64 MSVC developer shell with Windows SDK and run:

```powershell
.\Build-Interposer.ps1
```

Alternatively pass `-PortableMsvcRoot` for a portable toolchain containing `VC/Tools/MSVC` and `Windows Kits/10`. The original build used MSVC 14.44.35207 and Windows SDK 10.0.26100.0. The helper applies the patch only to the pinned source and preserves unrelated SDK changes if application fails. It copies the pinned Vulkan headers and obtains the matching generated dispatch header from the Khronos Vulkan-Loader 1.4.365 tag.

Output: `.deps/Streamline-2.14.1/_artifacts/queuefix-build/sl.interposer.dll`. Place it at `RDR2VulkanFG/Streamline/sl.interposer.dll` in the runtime package. Preserve the matching production plugin DLLs, NGX runtime and low-latency DLL. The rebuilt interposer is a locally modified binary, not an NVIDIA-signed release.

## Queue regression

After applying the SDK patch:

```powershell
& 'C:\tools\zig\zig.exe' c++ -std=c++17 -O2 -I'.deps/Vulkan-Headers/include' -I'.deps/Streamline-2.14.1' tests/native_queue_allocation.cpp -o '.deps/native-queue-check.exe'
& '.deps/native-queue-check.exe'
```

Other isolated test sources are under `tests/`. `interposer_load.cpp` can create a real Vulkan device when its optional device argument is supplied; it is not a gameplay test. Never run the intentional crash fixtures in RDR2.

## Runtime packaging

Use the matching Nexus package as the runtime template; its private layout includes Streamline, Core/RTX20 and XeSS dependencies. Preserve third-party licenses. The public INI defaults to Native DLSS-G with detailed logging/diagnostics off and RTX20 opt-in off. Exclude executables, PDBs, logs, manifests and developer/simulation INI keys from the user-facing runtime ZIP.

The proprietary runtime components are not built by `Build.ps1`. Updating their versions can invalidate binary-layout checks and offsets; review the runtime policies before substituting DLLs. Whole-program reproducible builds are not claimed.

`tools/package_runtime.py --template <matching-0.82.zip> --output <new.zip> --asi <built.asi>` replaces the main ASI while preserving the template's runtime dependencies. Add `--interposer` and `--diagnostics` only when replacing those corresponding build outputs. It refuses existing output paths and developer-enabled templates. This helper does not install or publish the package.

The optional vendored unlock-core build script retains older workspace/precompiled-MinHook-object assumptions. It is not part of the main ASI build and is not advertised as a standalone clean-checkout build. Use the matching runtime core unless deliberately working on that component.
