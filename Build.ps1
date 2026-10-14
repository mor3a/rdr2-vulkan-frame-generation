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
$buildRoot=Join-Path $DependencyRoot 'rdr2-fg-public-0.30-build'
New-Item -ItemType Directory -Force $buildRoot | Out-Null
$flags=@('c++','-std=c++17','-O2',"-ffile-prefix-map=$PSScriptRoot=.","-ffile-prefix-map=$DependencyRoot=dependencies",'-fms-extensions','-Wno-macro-redefined','-Wno-nullability-completeness',
  '-DVK_NO_PROTOTYPES','-DWIN32_LEAN_AND_MEAN','-DNOMINMAX','-DDETOURS_X64',
  "-I$(Join-Path $DependencyRoot 'Vulkan-Headers/include')",
  "-I$(Join-Path $DependencyRoot 'Streamline-2.12/include')",
  "-I$(Join-Path $DependencyRoot 'Detours/src')","-I$(Join-Path $PSScriptRoot 'src')")
$flags+=@("-I$(Join-Path $PSScriptRoot 'vendor/imgui')","-I$(Join-Path $PSScriptRoot 'vendor')")
$src=@('dllmain.cpp','log.cpp','config.cpp','vkhooks.cpp','slboot.cpp','inputs.cpp','ngx_capture.cpp','ngx_capture.S','ampere_boot.cpp','rdr_camera.cpp','menu.cpp','fsr_native.cpp','hud_capture.cpp','hud_mask.cpp','performance_probe.cpp') | ForEach-Object {Join-Path $PSScriptRoot "src/$_"}
$src+=@('imgui.cpp','imgui_draw.cpp','imgui_tables.cpp','imgui_widgets.cpp','imgui_impl_win32.cpp','imgui_impl_dx11.cpp') | ForEach-Object {Join-Path $PSScriptRoot "vendor/imgui/$_"}
$src=$src | Where-Object {$_ -notlike '*imgui_impl_dx11.cpp'}
$src+=@(Join-Path $PSScriptRoot 'src/menu_vulkan.cpp'),(Join-Path $PSScriptRoot 'vendor/imgui/imgui_impl_vulkan.cpp')
$detours=@('detours.cpp','disasm.cpp','modules.cpp','image.cpp','creatwth.cpp') | ForEach-Object {Join-Path $DependencyRoot "Detours/src/$_"}
$binary=Join-Path $buildRoot 'RDR2VulkanFG.asi'
$log=Join-Path $buildRoot 'build.log'
$src+=@(Join-Path $PSScriptRoot 'src/update_check.cpp')
$buildArgs=$flags+@('-shared')+$src+$detours+@((Join-Path $PSScriptRoot 'vendor/d3dcompiler.lib'),'-o',$binary,'-luser32','-ladvapi32','-ld3d11','-ldxgi','-lgdi32','-ldwmapi','-lcomctl32','-limm32','-lwinhttp','-lshell32','-lole32','-lversion','-Wl,--dynamicbase','-Wl,--nxcompat')
& $ZigPath @buildArgs *> $log
if($LASTEXITCODE -ne 0){Get-Content -LiteralPath $log -Tail 30;throw 'ASI build failed.'}
$diagnosticBinary=Join-Path $buildRoot 'RDR2VulkanFGDiagnostics.dll'
& $ZigPath c++ -std=c++17 -O2 -shared -DFGVK_COLLECTOR_DLL -DWIN32_LEAN_AND_MEAN "-I$(Join-Path $PSScriptRoot 'src')" (Join-Path $PSScriptRoot 'src/tools/fgvk-stack.cpp') -o $diagnosticBinary -ldbghelp -lshell32 *> (Join-Path $buildRoot 'diagnostics-helper-build.log')
if($LASTEXITCODE -ne 0){Get-Content (Join-Path $buildRoot 'diagnostics-helper-build.log') -Tail 25;throw 'Diagnostic collector build failed.'}
New-Item -ItemType Directory -Path (Join-Path $PSScriptRoot 'RDR2VulkanFG') -Force | Out-Null
Copy-Item -LiteralPath $diagnosticBinary -Destination (Join-Path $PSScriptRoot 'RDR2VulkanFG/RDR2VulkanFGDiagnostics.dll') -Force
New-Item -ItemType Directory -Path (Join-Path $buildRoot 'RDR2VulkanFG') -Force | Out-Null
Copy-Item -LiteralPath $diagnosticBinary -Destination (Join-Path $buildRoot 'RDR2VulkanFG/RDR2VulkanFGDiagnostics.dll') -Force
if($RunChecks){
  foreach($test in @('contracts','capture_abi','fps_meter','swapchain_lock','menu_startup','diagnostics','present_compat','present_route','latency_imports','vulkan_owner')){
    $exe=Join-Path $buildRoot "$test.exe"
    $argsTest=$flags+@(Join-Path $PSScriptRoot "tests/$test.cpp")
    if($test -eq 'capture_abi'){$argsTest+=@(Join-Path $PSScriptRoot 'src/ngx_capture.S')}
    if($test -eq 'diagnostics'){$argsTest+=@(Join-Path $PSScriptRoot 'src/log.cpp'),'-lversion'}
    $argsTest+=@('-o',$exe)
    & $ZigPath @argsTest *> (Join-Path $buildRoot "$test-build.log")
    if($LASTEXITCODE -ne 0){throw "Check build failed: $test"}
    & $exe
    if($LASTEXITCODE -ne 0){throw "Check failed: $test"}
  }
  $fatalRoot=Join-Path $buildRoot 'fatal-exception-check'
  New-Item -ItemType Directory -Path (Join-Path $fatalRoot 'RDR2VulkanFG') -Force | Out-Null
  Copy-Item -LiteralPath $diagnosticBinary -Destination (Join-Path $fatalRoot 'RDR2VulkanFG/RDR2VulkanFGDiagnostics.dll') -Force
  $fatalExe=Join-Path $fatalRoot 'diagnostic_crash.exe'
  & $ZigPath @flags (Join-Path $PSScriptRoot 'tests/diagnostic_crash.cpp') (Join-Path $PSScriptRoot 'src/log.cpp') -lversion -o $fatalExe *> (Join-Path $buildRoot 'fatal-check-build.log')
  if($LASTEXITCODE -ne 0){throw 'Fatal diagnostic fixture build failed.'}
  $fixtureProcess=Start-Process -FilePath $fatalExe -WindowStyle Hidden -PassThru
  if(!$fixtureProcess.WaitForExit(10000)){Stop-Process -Id $fixtureProcess.Id;throw 'Fatal fixture exceeded its timeout.'}
  $fixtureLog=Join-Path $fatalRoot 'RDR2VulkanFG.log'
  $record=''
  for($attempt=0;$attempt -lt 30;$attempt++){
    $record=Get-Content -LiteralPath $fixtureLog -Raw
    if($record -match 'process exit code=0xc06d007f'){break}
    Start-Sleep -Milliseconds 100
  }
  if($record -notmatch 'DLL=fatal-fixture.dll function=FatalMissingExport WindowsError=127' -or $record -notmatch 'threads dumped' -or $record -notmatch 'process exit code=0xc06d007f'){throw 'Fatal delay-load failure did not reach the unified log.'}
  $unexpected=Get-ChildItem -LiteralPath $fatalRoot -File | Where-Object {$_.Name -match '\.dmp$|\.stacks\.log$|\.exceptions\.log$'}
  if($unexpected){throw 'Diagnostic fixture generated separate diagnostic files.'}
  Write-Host 'Unhandled delay-load crash: missing DLL/export, external stacks and actual process exit captured in one log: PASS'
  $fixtureProcess=Start-Process -FilePath $fatalExe -ArgumentList '--fastfail' -WindowStyle Hidden -PassThru
  if(!$fixtureProcess.WaitForExit(10000)){Stop-Process -Id $fixtureProcess.Id;throw 'Fail-fast fixture exceeded its timeout.'}
  for($attempt=0;$attempt -lt 30;$attempt++){$record=Get-Content -LiteralPath $fixtureLog -Raw;if($record -match 'process exit code=0xc0000409'){break};Start-Sleep -Milliseconds 100}
  if($record -notmatch 'DEBUG EXCEPTION code=0xc0000409' -or $record -notmatch 'subcode=7' -or $record -notmatch 'threads dumped' -or $record -notmatch 'process exit code=0xc0000409'){throw 'Actual fail-fast context was not captured.'}
  Write-Host 'Actual __fastfail(7): final fault context, thread stacks and 0xC0000409 exit captured externally: PASS'
}
Copy-Item -LiteralPath $binary -Destination (Join-Path $PSScriptRoot 'RDR2VulkanFG.asi') -Force
$cameraBinary=Join-Path $PSScriptRoot 'RDR2VulkanFGCamera.asi'
& $ZigPath c++ -O2 -shared -DWIN32_LEAN_AND_MEAN (Join-Path $PSScriptRoot 'src/camera_entry.cpp') -o $cameraBinary '-Wl,--dynamicbase' '-Wl,--nxcompat' *> (Join-Path $buildRoot 'camera-build.log')
if($LASTEXITCODE -ne 0){throw 'Camera helper build failed.'}
Get-FileHash -LiteralPath $binary -Algorithm SHA256 | Select-Object Hash,Path
Write-Host 'Build complete. Compilation does not replace runtime validation.'



