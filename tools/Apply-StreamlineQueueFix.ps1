param([string]$SDKRoot=(Join-Path $PSScriptRoot '..\.deps\Streamline-2.14.1'))
$ErrorActionPreference='Stop'
$patch=(Resolve-Path (Join-Path $PSScriptRoot '..\patches\streamline-2.14.1-queue-allocation.patch')).Path
$head=(& git -C $SDKRoot rev-parse HEAD).Trim()
if($head -ne '2122257e0fce486f91b385aa63b9a09b0a34b363'){throw 'Expected Streamline 2.14.1 pinned source revision.'}
& git -C $SDKRoot apply --reverse --check $patch 2>$null
if($LASTEXITCODE -eq 0){Write-Host 'Streamline queue allocation patch is already applied.';exit 0}
& git -C $SDKRoot apply --check $patch
if($LASTEXITCODE -ne 0){throw 'Queue patch does not apply cleanly; existing SDK changes were preserved.'}
& git -C $SDKRoot apply $patch
if($LASTEXITCODE -ne 0){throw 'Queue patch failed.'}
Write-Host 'Applied Streamline queue allocation fix.'
