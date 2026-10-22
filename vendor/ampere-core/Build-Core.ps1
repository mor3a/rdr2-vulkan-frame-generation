$ErrorActionPreference='Stop'
$work=Split-Path (Split-Path (Split-Path $PSScriptRoot))
$src=@('patcher.cpp','preset_runtime.cpp','midpoint_fix.cpp','ampere_policy.cpp','ampere_bundle.cpp','sm75_ptx.cpp','turing_runtime.cpp','ampere_experiment.cpp','flip_metering.cpp','dlssg_provider_policy.cpp','entry_detour.cpp','nvidia_mfg_policy.cpp','temporal_interval_trace.cpp') | ForEach-Object { Join-Path $PSScriptRoot $_ }
$src+=Join-Path $PSScriptRoot 'exports.def'
& "$work\toolchain\ziglang\zig.exe" c++ -std=c++20 -O2 -fms-extensions -shared -Wno-deprecated-declarations -DUNICODE -D_UNICODE -DNOMINMAX -DWIN32_LEAN_AND_MEAN -DMFG_UNLOCK_UNIVERSAL_CONFIG=1 "-I$work\Streamline-2.14.1\include" "-I$work\Streamline\external\ngx-sdk\include" "-I$PSScriptRoot\third_party/minhook/include" @src "$PSScriptRoot\entry_detour.S" "$PSScriptRoot\buffer.obj" "$PSScriptRoot\hook.obj" "$PSScriptRoot\trampoline.obj" "$PSScriptRoot\hde64.obj" -lbcrypt -lversion -ldxgi -o "$PSScriptRoot\RTX40MFGCore.dll"
if($LASTEXITCODE){throw 'Core build failed'}
