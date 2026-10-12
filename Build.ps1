param(
  [string]$ZigPath=(Join-Path $PSScriptRoot '..\..\work\toolchain\ziglang\zig.exe'),
  [string]$DependencyRoot=(Join-Path $PSScriptRoot '..\..\work'),
  [switch]$RunChecks
)
$ErrorActionPreference='Stop'
if(!(Test-Path -LiteralPath $ZigPath)){throw 'Zig 0.16.0 not found. Pass -ZigPath with the path to zig.exe.'}
$deps=@(
  @{Name='Detours';Url='https://github.com/microsoft/Detours.git';Commit='1dbd4ea281d73d478aa8d09a8b83fbd265f715f7'},
  @{Name='Vulkan-Headers';Url='https://github.com/KhronosGroup/Vulkan-Headers.git';Commit='c46850864f4661461b0f6cb9922c058ffea4915e'},
  @{Name='Streamline-2.12';Url='https://github.com/NVIDIA-RTX/Streamline.git';Commit='e8aaa6eaac968711fb62473d4ae8256dde20919b'}
)
New-Item -ItemType Directory -Force $DependencyRoot | Out-Null
foreach($dep in $deps){
  $depPath=Join-Path $DependencyRoot $dep.Name
  if(!(Test-Path -LiteralPath $depPath)){
    & git clone --filter=blob:none --no-checkout $dep.Url $depPath
    if($LASTEXITCODE -ne 0){throw "Dependency download failed: $($dep.Name)"}
    & git -C $depPath checkout --detach $dep.Commit
    if($LASTEXITCODE -ne 0){throw "Pinned checkout failed: $($dep.Name)"}
  }
  $head=(& git -C $depPath rev-parse HEAD).Trim()
  if($head -ne $dep.Commit){throw "Dependency revision differs; existing checkout preserved: $depPath"}
}
$buildRoot=Join-Path $DependencyRoot 'rdr2-fg-build'
New-Item -ItemType Directory -Force $buildRoot | Out-Null
$flags=@('c++','-std=c++17','-O2',"-ffile-prefix-map=$PSScriptRoot=.","-ffile-prefix-map=$DependencyRoot=dependencies",'-fms-extensions','-Wno-macro-redefined','-Wno-nullability-completeness',
  '-DVK_NO_PROTOTYPES','-DWIN32_LEAN_AND_MEAN','-DNOMINMAX','-DDETOURS_X64',
  "-I$(Join-Path $DependencyRoot 'Vulkan-Headers/include')",
  "-I$(Join-Path $DependencyRoot 'Streamline-2.12/include')",
  "-I$(Join-Path $DependencyRoot 'Detours/src')","-I$(Join-Path $PSScriptRoot 'src')")
$flags+=@("-I$(Join-Path $PSScriptRoot 'vendor/imgui')","-I$(Join-Path $PSScriptRoot 'vendor')")
$src=@('dllmain.cpp','log.cpp','config.cpp','vkhooks.cpp','slboot.cpp','inputs.cpp','ngx_capture.cpp','ngx_capture.S','ampere_boot.cpp','rdr_camera.cpp','menu.cpp','fsr_native.cpp','startup_guard.cpp','hud_capture.cpp','hud_mask.cpp','performance_probe.cpp') | ForEach-Object {Join-Path $PSScriptRoot "src/$_"}
$src+=@('imgui.cpp','imgui_draw.cpp','imgui_tables.cpp','imgui_widgets.cpp','imgui_impl_win32.cpp','imgui_impl_dx11.cpp') | ForEach-Object {Join-Path $PSScriptRoot "vendor/imgui/$_"}
$src=$src | Where-Object {$_ -notlike '*imgui_impl_dx11.cpp'}
$src+=@(Join-Path $PSScriptRoot 'src/menu_vulkan.cpp'),(Join-Path $PSScriptRoot 'vendor/imgui/imgui_impl_vulkan.cpp')
$detours=@('detours.cpp','disasm.cpp','modules.cpp','image.cpp','creatwth.cpp') | ForEach-Object {Join-Path $DependencyRoot "Detours/src/$_"}
$binary=Join-Path $buildRoot 'RDR2VulkanFG.asi'
$log=Join-Path $buildRoot 'build.log'
$buildArgs=$flags+@('-shared')+$src+$detours+@((Join-Path $PSScriptRoot 'vendor/d3dcompiler.lib'),'-o',$binary,'-luser32','-ladvapi32','-ld3d11','-ldxgi','-lgdi32','-ldwmapi','-lcomctl32','-limm32','-Wl,--dynamicbase','-Wl,--nxcompat')
& $ZigPath @buildArgs *> $log
if($LASTEXITCODE -ne 0){Get-Content -LiteralPath $log -Tail 30;throw 'ASI build failed.'}
if($RunChecks){
  foreach($test in @('contracts','capture_abi','fps_meter')){
    $exe=Join-Path $buildRoot "$test.exe"
    $argsTest=$flags+@(Join-Path $PSScriptRoot "tests/$test.cpp")
    if($test -eq 'capture_abi'){$argsTest+=@(Join-Path $PSScriptRoot 'src/ngx_capture.S')}
    $argsTest+=@('-o',$exe)
    & $ZigPath @argsTest *> (Join-Path $buildRoot "$test-build.log")
    if($LASTEXITCODE -ne 0){throw "Check build failed: $test"}
    & $exe
    if($LASTEXITCODE -ne 0){throw "Check failed: $test"}
  }
}
Copy-Item -LiteralPath $binary -Destination (Join-Path $PSScriptRoot 'RDR2VulkanFG.asi') -Force
$cameraBinary=Join-Path $PSScriptRoot 'RDR2VulkanFGCamera.asi'
& $ZigPath c++ -O2 -shared -DWIN32_LEAN_AND_MEAN (Join-Path $PSScriptRoot 'src/camera_entry.cpp') -o $cameraBinary '-Wl,--dynamicbase' '-Wl,--nxcompat' *> (Join-Path $buildRoot 'camera-build.log')
if($LASTEXITCODE -ne 0){throw 'Camera helper build failed.'}
Get-FileHash -LiteralPath $binary -Algorithm SHA256 | Select-Object Hash,Path
Write-Host 'Build complete. Compilation does not replace runtime validation.'



