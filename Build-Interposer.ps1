param(
  [string]$DependencyRoot=(Join-Path $PSScriptRoot '.deps'),
  [string]$SDKRoot=(Join-Path $DependencyRoot 'Streamline-2.14.1'),
  [string]$PortableMsvcRoot=''
)
$ErrorActionPreference='Stop'
$DependencyRoot=(Resolve-Path $DependencyRoot).Path
$sdk=(Resolve-Path $SDKRoot).Path
& (Join-Path $PSScriptRoot 'tools\Apply-StreamlineQueueFix.ps1') -SDKRoot $sdk
if($LASTEXITCODE -ne 0){throw 'Queue source patch failed'}
if($PortableMsvcRoot){
  $PortableMsvcRoot=(Resolve-Path $PortableMsvcRoot).Path
  $vc=Get-ChildItem (Join-Path $PortableMsvcRoot 'VC\Tools\MSVC') -Directory | Sort-Object Name -Descending | Select-Object -First 1
  $win=Join-Path $PortableMsvcRoot 'Windows Kits\10'
  $sdkVersion=(Get-ChildItem (Join-Path $win 'Include') -Directory | Sort-Object Name -Descending | Select-Object -First 1).Name
  if(!$vc -or !$sdkVersion){throw 'Portable MSVC or Windows SDK directory is incomplete.'}
  $env:PATH="$($vc.FullName)\bin\Hostx64\x64;$win\bin\$sdkVersion\x64;$env:PATH"
  $env:INCLUDE="$($vc.FullName)\include;$win\Include\$sdkVersion\ucrt;$win\Include\$sdkVersion\shared;$win\Include\$sdkVersion\um;$win\Include\$sdkVersion\winrt"
  $env:LIB="$($vc.FullName)\lib\x64;$win\Lib\$sdkVersion\ucrt\x64;$win\Lib\$sdkVersion\um\x64"
}
if(!(Get-Command cl.exe -ErrorAction SilentlyContinue) -or !(Get-Command rc.exe -ErrorAction SilentlyContinue)){throw 'Run from an x64 MSVC developer shell, or pass -PortableMsvcRoot.'}
$headers=Join-Path $DependencyRoot 'Vulkan-Headers\include'
New-Item -ItemType Directory -Path "$sdk\external\vulkan\include" -Force | Out-Null
Copy-Item -Path "$headers\*" -Destination "$sdk\external\vulkan\include" -Recurse -Force
$dispatch="$sdk\external\vulkan\include\vulkan\vk_layer_dispatch_table.h"
if(!(Test-Path -LiteralPath $dispatch)){
  Invoke-WebRequest -Uri 'https://raw.githubusercontent.com/KhronosGroup/Vulkan-Loader/v1.4.365/loader/generated/vk_layer_dispatch_table.h' -OutFile $dispatch
}
$build=Join-Path $sdk '_artifacts\queuefix-build'
New-Item -ItemType Directory -Path $build -Force | Out-Null
@'
#define GIT_LAST_COMMIT_SHORT "queuefix1"
#define GIT_LAST_COMMIT "RDR2VulkanFG queuefix1"
#define GIT_LAST_COMMIT_TAG "v2.14.1"
'@ | Set-Content -LiteralPath "$sdk\_artifacts\gitVersion.h"
$dirs=@('sl.interposer','sl.api','sl.param','sl.log','sl.exception','sl.ota','sl.security','sl.plugin-manager')
$src=foreach($dir in $dirs){Get-ChildItem "$sdk\source\core\$dir" -Recurse -Filter '*.cpp' | Where-Object {$_.Name -ne 'pluginManagerEntry.cpp'} | ForEach-Object {$_.FullName}}
$flags=@('/nologo','/std:c++20','/O2','/MD','/EHsc','/MP4','/DWIN32','/DWIN64','/DNOMINMAX','/DSL_SDK','/DSL_INTERPOSER','/DNDEBUG','/DSL_PRODUCTION','/DSL_ENABLE_TIMING=0','/DSL_ENABLE_PROFILING=0','/DVK_USE_PLATFORM_WIN32_KHR','/DSL_ENABLE_EXCEPTION_HANDLING',"/I$sdk","/I$sdk\external\json\include","/I$sdk\external\nvapi","/I$sdk\external\vulkan\include")
Push-Location $build
try {
  & cl.exe @flags /c @src *> compile.log
  if($LASTEXITCODE -ne 0){Get-Content compile.log -Tail 40;throw 'Interposer compile failed'}
  & rc.exe /nologo /DSL_PRODUCTION "/I$sdk" /fointerposer.res "$sdk\source\core\sl.interposer\interposer.rc" *> resource.log
  if($LASTEXITCODE -ne 0){Get-Content resource.log;throw 'Interposer resource compile failed'}
  $objects=Get-ChildItem -Filter '*.obj' | ForEach-Object {$_.FullName}
  & link.exe /nologo /DLL /OPT:REF /OPT:ICF /DYNAMICBASE /NXCOMPAT /OUT:sl.interposer.dll "/DEF:$sdk\source\core\sl.interposer\exports.def" @objects interposer.res "$sdk\external\nvapi\amd64\nvapi64.lib" dbghelp.lib user32.lib advapi32.lib ole32.lib shell32.lib version.lib wintrust.lib crypt32.lib *> link.log
  if($LASTEXITCODE -ne 0){Get-Content link.log -Tail 40;throw 'Interposer link failed'}
  Get-FileHash sl.interposer.dll
} finally {Pop-Location}
