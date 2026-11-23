<#
  Runs on the build machine (copied there by build-remote.ps1): unpacks the
  sources, runs build.ps1 for each configuration with the tests, and optionally
  a short smoke run of the Release exe. Prints a short verdict only.
#>
param(
    [Parameter(Mandatory = $true)][string]$Root,
    [string[]]$Configurations = @('Release'),
    [switch]$Smoke,
    [string]$EvalList = '',  # optional: run the talking-track evaluation on this list (see tests\KHRadioTests.cpp)
    [string]$EvalCache = ''  # where the evaluation keeps fetched windows between runs
)
$ErrorActionPreference = 'Continue'
$ProgressPreference = 'SilentlyContinue'
$Configurations = @($Configurations -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ } | Select-Object -Unique) # -File passes a list as one string
$src = Join-Path $Root 'KHRadio'
$tmp = Join-Path $Root 'tmp'
New-Item -ItemType Directory -Force -Path $tmp | Out-Null
$env:TEMP = $tmp; $env:TMP = $tmp

$tar = Join-Path $Root 'khradio_src.tar'
if (Test-Path $tar) {
    # keep the restored SDK and object files between rounds, replace the sources
    if (Test-Path $src) { Get-ChildItem $src -Force | Where-Object { $_.Name -ne '_build' } | Remove-Item -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $src | Out-Null
    Push-Location $src
    & "$env:SystemRoot\System32\tar.exe" -xf '..\khradio_src.tar'
    Pop-Location
    Remove-Item $tar
}

$ok = $true
foreach ($cfg in $Configurations) {
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $src 'build.ps1') -Configuration $cfg -Tests |
        Where-Object { $_ -notmatch '^exe: ' }
    if ($LASTEXITCODE -ne 0) { $ok = $false }
}

if ($ok -and $Smoke) {
    $exe = Join-Path $src '_build\Release\KHRadio.exe'
    $ad = Join-Path $env:APPDATA 'KHRadio'; $la = Join-Path $env:LOCALAPPDATA 'KHRadio'
    if ((Test-Path $ad) -or (Test-Path $la)) { 'smoke: SKIPPED - KH Radio data already exists on this machine, not touching it'; $ok = $false }
    else {
        Add-Type @'
using System; using System.Runtime.InteropServices; using System.Text;
public static class KHW {
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowW(string c, string t);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumThreadWindows(uint tid, EnumProc f, IntPtr l);
  // by the process's own threads: over ssh the app may sit on a desktop FindWindow can't see
  public static IntPtr MainWindow(System.Diagnostics.Process p) {
    IntPtr found = IntPtr.Zero;
    foreach (System.Diagnostics.ProcessThread t in p.Threads)
      EnumThreadWindows((uint)t.Id, (h, l) => { var c = new StringBuilder(64); GetClassNameW(h, c, 64); if (c.ToString() == "KHRadio") { found = h; return false; } return true; }, IntPtr.Zero);
    return found;
  }
}
'@
        $p = Start-Process -FilePath $exe -PassThru; $null = $p.Handle
        $h = [IntPtr]::Zero
        for ($i = 0; $i -lt 40 -and $h -eq [IntPtr]::Zero; $i++) { Start-Sleep -Milliseconds 250; $p.Refresh(); $h = [KHW]::MainWindow($p) }
        $owner = 0; if ($h -ne [IntPtr]::Zero) { [void][KHW]::GetWindowThreadProcessId($h, [ref]$owner) }
        $window = ($h -ne [IntPtr]::Zero -and $owner -eq $p.Id)
        Start-Sleep -Seconds 4
        $p2 = Start-Process -FilePath $exe -PassThru; $null = $p2.Handle
        $second = $p2.WaitForExit(8000); if (-not $second) { & taskkill.exe /PID $p2.Id /T /F | Out-Null }
        $alive = -not $p.HasExited
        $sw = [Diagnostics.Stopwatch]::StartNew()
        if ($window) { [void][KHW]::PostMessageW($h, 0x10, [IntPtr]::Zero, [IntPtr]::Zero) }
        $closed = $p.WaitForExit(10000); $sw.Stop()
        if (-not $closed) { & taskkill.exe /PID $p.Id /T /F | Out-Null }
        $settings = Join-Path $ad 'settings.json'
        $saved = Test-Path $settings
        $log = Join-Path $ad 'khradio_debug.log'
        $version = if (Test-Path $log) { (Select-String -Path $log -Pattern 'KH Radio [0-9.]+ starting' | Select-Object -First 1).Matches.Value } else { '' }
        "smoke: window=$window alive=$alive second-launch-exit=$second/$($p2.ExitCode) WM_CLOSE-exit=$closed in $([int]$sw.Elapsed.TotalMilliseconds) ms code=$(if ($closed) { $p.ExitCode } else { 'killed' }) settings.json=$saved log='$version'"
        if (-not ($window -and $alive -and $second -and $p2.ExitCode -eq 0 -and $closed -and $saved)) {
            $ok = $false
            if (Test-Path $log) { Get-Content $log -Tail 8 | ForEach-Object { '  log: ' + $(if ($_.Length -gt 160) { $_.Substring(0, 160) } else { $_ }) } }
        }
        Start-Sleep -Seconds 2
        Remove-Item $ad, $la -Recurse -Force -ErrorAction SilentlyContinue
        "smoke: removed the run's data: appdata=$(-not (Test-Path $ad)) localappdata=$(-not (Test-Path $la))"
    }
}
if ($ok -and $EvalList) {
    $tests = Join-Path $src '_build\Release\tests\KHRadioTests.exe'
    $csv = Join-Path $Root 'eval.csv'; $evalOut = Join-Path $Root 'eval-out.txt'
    $p = Start-Process -FilePath $tests -ArgumentList @('--eval', "`"$EvalList`"", "`"$EvalCache`"", "`"$csv`"") -NoNewWindow -PassThru -RedirectStandardOutput $evalOut -RedirectStandardError "$evalOut.err"
    $null = $p.Handle
    if (-not $p.WaitForExit(900000)) { & taskkill.exe /PID $p.Id /T /F | Out-Null; 'eval: TIMED OUT after 15 min'; $ok = $false }
    Get-Content $evalOut, "$evalOut.err" -ErrorAction SilentlyContinue | Select-Object -First 12
    if ($p.HasExited -and $p.ExitCode -ne 0) { $ok = $false }
}
Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue
"REMOTE VERDICT: $(if ($ok) { 'PASS' } else { 'FAIL' })"
