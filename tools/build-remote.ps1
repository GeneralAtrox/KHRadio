<#
.SYNOPSIS
  Builds and tests KH Radio on another Windows machine over ssh, then brings the
  Release exe and its runtime DLLs back to bin\.

.DESCRIPTION
  Packs the sources (no build output), copies them and tools\remote-run.ps1 to
  RemoteDir on the build machine, runs build.ps1 there for each configuration
  with the tests (and a short smoke run with -Smoke), copies the Release exe
  back, and deletes RemoteDir unless -KeepRemote. The build machine needs
  Windows PowerShell, OpenSSH server, Visual Studio 2022 with MFC, and NuGet
  access for the WebView2 SDK.

  If bin\KHRadio.exe is in use (KH Radio is running), the new exe is written as
  bin\KHRadio.new.exe instead; nothing running is closed.

.EXAMPLE
  .\tools\build-remote.ps1 -HostName buildbox -RemoteDir C:\build\KHRadio -Configurations Release,Debug -Smoke
#>
param(
    [Parameter(Mandatory = $true)][string]$HostName,
    [Parameter(Mandatory = $true)][string]$RemoteDir,
    [string[]]$Configurations = @('Release'),
    [switch]$Smoke,
    [switch]$KeepRemote
)
$ErrorActionPreference = 'Stop'
$Configurations = @($Configurations -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ } | Select-Object -Unique) # -File passes a list as one string
$root = Split-Path -Parent $PSScriptRoot
$stage = Join-Path $root '_build\remote'
New-Item -ItemType Directory -Force -Path $stage | Out-Null
$tar = Join-Path $stage 'khradio_src.tar'
$remote = $RemoteDir.TrimEnd('\').Replace('\', '/')
if ($Configurations -notcontains 'Release') { $Configurations = @('Release') + $Configurations }

# Windows' own tar, relative paths only (an absolute C: path reads as a host name to some tars)
Push-Location $root
try {
    & "$env:SystemRoot\System32\tar.exe" -cf $tar --exclude=./_build --exclude=./bin --exclude=./.vs --exclude=*.user .
    if ($LASTEXITCODE -ne 0) { throw 'packing the sources failed' }
} finally { Pop-Location }
"packed: $([math]::Round((Get-Item $tar).Length / 1MB, 1)) MB"

$cfgList = ($Configurations | Select-Object -Unique) -join ','

# Remote commands go as -EncodedCommand, so no quoting survives two shells.
function Invoke-Remote([string]$script) {
    $wrapped = "`$ProgressPreference = 'SilentlyContinue'; & { $script } 2>&1 | ForEach-Object { `"`$_`" }"
    $encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($wrapped))
    & ssh $HostName "powershell -NoProfile -ExecutionPolicy Bypass -EncodedCommand $encoded"
}
$rq = $RemoteDir.Replace("'", "''")
try {
    Invoke-Remote "New-Item -ItemType Directory -Force -Path '$rq' | Out-Null"
    if ($LASTEXITCODE -ne 0) { throw "could not reach $HostName" }
    & scp -q $tar (Join-Path $root 'tools\remote-run.ps1') "${HostName}:$remote/"
    if ($LASTEXITCODE -ne 0) { throw 'copying to the build machine failed' }

    $smokeArg = if ($Smoke) { ' -Smoke' } else { '' }
    $output = Invoke-Remote "& powershell.exe -NoProfile -ExecutionPolicy Bypass -File '$rq\remote-run.ps1' -Root '$rq' -Configurations $cfgList$smokeArg"
    $output | ForEach-Object { $_ }
    $passed = ($output | Select-Object -Last 1) -eq 'REMOTE VERDICT: PASS'

    if ($passed) {
        $bin = Join-Path $root 'bin'
        New-Item -ItemType Directory -Force -Path $bin | Out-Null
        $fetch = Join-Path $stage 'fetch'
        Remove-Item $fetch -Recurse -Force -ErrorAction SilentlyContinue
        New-Item -ItemType Directory -Force -Path $fetch | Out-Null
        & scp -q "${HostName}:$remote/KHRadio/_build/Release/KHRadio.exe" "${HostName}:$remote/KHRadio/_build/Release/*.dll" "$fetch/"
        if ($LASTEXITCODE -ne 0) { throw 'copying the exe back failed' }
        $target = Join-Path $bin 'KHRadio.exe'
        $locked = $false
        if (Test-Path $target) {
            try { $s = [IO.File]::Open($target, 'Open', 'ReadWrite', 'None'); $s.Close() } catch { $locked = $true }
        }
        if ($locked) { $target = Join-Path $bin 'KHRadio.new.exe' }
        Copy-Item (Join-Path $fetch 'KHRadio.exe') $target -Force
        Get-ChildItem $fetch -Filter *.dll | ForEach-Object {
            $dest = Join-Path $bin $_.Name
            try { Copy-Item $_.FullName $dest -Force } catch { "bin: $($_.Name) is in use, left as it was" }
        }
        Remove-Item $fetch -Recurse -Force
        "bin: $target ($((Get-Item $target).Length) bytes)$(if ($locked) { ' - KHRadio.exe is in use; rename this one after closing KH Radio' })"
    }
} finally {
    if (-not $KeepRemote) {
        Invoke-Remote "Remove-Item -LiteralPath '$rq' -Recurse -Force -ErrorAction SilentlyContinue; if (Test-Path -LiteralPath '$rq') { 'remote: NOT fully removed' } else { 'remote: removed' }"
    }
    Remove-Item $tar -Force -ErrorAction SilentlyContinue
}
if ($passed) { 'VERDICT: PASS' } else { 'VERDICT: FAIL'; exit 1 }
