<#
.SYNOPSIS
  Builds KH Radio (and optionally its tests) with the newest Visual Studio found by vswhere.

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1 -Configuration Release -Tests

  Prints a short verdict; the full build log is in _build\logs\. Exit code 0 means
  the build (and the tests, if asked for) passed.
#>
param(
    [ValidateSet('Release', 'Debug')][string]$Configuration = 'Release',
    [switch]$Tests,
    [int]$TimeoutMinutes = 20
)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$logs = Join-Path $root '_build\logs'
New-Item -ItemType Directory -Force -Path $logs | Out-Null

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) { 'FAIL: vswhere.exe not found - install Visual Studio 2022 with the MFC component'; exit 2 }
$builder = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\amd64\MSBuild.exe' | Select-Object -First 1
if (-not $builder) { 'FAIL: no MSBuild found by vswhere'; exit 2 }

# Restore the WebView2 SDK once, before the two projects could race for it.
$props = [xml](Get-Content (Join-Path $root 'thirdparty\webview2\WebView2.props') -Raw)
$sdkVersion = (Split-Path -Leaf ($props.Project.PropertyGroup.KHWebView2Sdk | Select-Object -First 1)).Trim()
$sdk = Join-Path $root "_build\WebView2\$sdkVersion"
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'thirdparty\webview2\restore.ps1') -Destination $sdk
if ($LASTEXITCODE -ne 0) { 'FAIL: WebView2 SDK restore failed'; exit 2 }

# v143's default toolset can be an older one without MFC; use the newest installed toolset that has it.
$vsRoot = (& $vswhere -latest -products * -property installationPath | Select-Object -First 1)
$toolset = Get-ChildItem (Join-Path $vsRoot 'VC\Tools\MSVC') -Directory -ErrorAction SilentlyContinue |
    Where-Object { Test-Path (Join-Path $_.FullName 'atlmfc\lib\x64\mfcs140.lib') } |
    Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
$toolsetArg = if ($toolset) { @("/p:VCToolsVersion=$($toolset.Name)") } else { @() }

function Invoke-Build([string]$project, [string]$name) {
    $log = Join-Path $logs "$name-$Configuration.log"
    $buildArgs = @("`"$project`"", '/m', '/nologo', '/v:quiet', "/p:Configuration=$Configuration", '/p:Platform=x64') + $toolsetArg + @(
        "/flp:LogFile=`"$log`";Verbosity=normal")
    $p = Start-Process -FilePath $builder -ArgumentList $buildArgs -NoNewWindow -PassThru `
        -RedirectStandardOutput (Join-Path $logs "$name-$Configuration.out.txt")
    $null = $p.Handle # keep the handle so ExitCode is available after WaitForExit
    if (-not $p.WaitForExit($TimeoutMinutes * 60000)) {
        & taskkill.exe /PID $p.Id /T /F | Out-Null
        return [pscustomobject]@{ Name = $name; Ok = $false; Errors = @("timed out after $TimeoutMinutes min"); Warnings = @(); Log = $log }
    }
    $text = Get-Content $log -ErrorAction SilentlyContinue
    $errors = @($text | Select-String -Pattern ': (fatal )?error ' | ForEach-Object { $_.Line.Trim() } | Sort-Object -Unique)
    $warnings = @($text | Select-String -Pattern ': warning ' | ForEach-Object { $_.Line.Trim() } | Sort-Object -Unique)
    [pscustomobject]@{ Name = $name; Ok = ($p.ExitCode -eq 0 -and $errors.Count -eq 0); Errors = $errors; Warnings = $warnings; Log = $log }
}

$results = @(Invoke-Build (Join-Path $root 'KHRadio.vcxproj') 'KHRadio')
if ($Tests) { $results += Invoke-Build (Join-Path $root 'tests\KHRadioTests.vcxproj') 'KHRadioTests' }

$ok = $true
foreach ($r in $results) {
    "$($r.Name) $Configuration`: $(if ($r.Ok) { 'OK' } else { 'FAILED' }), $($r.Errors.Count) error(s), $($r.Warnings.Count) warning(s)  [log: $($r.Log)]"
    $r.Errors | Select-Object -First 8 | ForEach-Object { '  E: ' + $(if ($_.Length -gt 220) { $_.Substring(0, 220) } else { $_ }) }
    $r.Warnings | Select-Object -First 4 | ForEach-Object { '  W: ' + $(if ($_.Length -gt 220) { $_.Substring(0, 220) } else { $_ }) }
    if (-not $r.Ok) { $ok = $false }
}

if ($ok -and $Tests) {
    $exe = Join-Path $root "_build\$Configuration\tests\KHRadioTests.exe"
    $out = Join-Path $logs "tests-$Configuration.txt"
    $p = Start-Process -FilePath $exe -NoNewWindow -PassThru -RedirectStandardOutput $out -RedirectStandardError "$out.err"
    $null = $p.Handle
    if (-not $p.WaitForExit(120000)) { & taskkill.exe /PID $p.Id /T /F | Out-Null; 'tests: TIMED OUT'; $ok = $false }
    else {
        Get-Content $out -Tail 1
        Get-Content "$out.err" -ErrorAction SilentlyContinue | Select-Object -First 8 | ForEach-Object { '  ' + $_ }
        if ($p.ExitCode -ne 0) { $ok = $false }
    }
}

$exe = Join-Path $root "_build\$Configuration\KHRadio.exe"
if ($ok -and (Test-Path $exe)) { "exe: $exe ($((Get-Item $exe).Length) bytes)" }
"VERDICT: $(if ($ok) { 'PASS' } else { 'FAIL' })"
if ($ok) { exit 0 } else { exit 1 }
