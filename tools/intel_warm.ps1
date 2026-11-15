# Long warm-up boot: runs the parked race with the given env settings until the race has run 20 s (max -Minutes),
# so every shader variant is compiled and cached before an A/B. Usage: intel_warm.ps1 -Env "REGPU_HYPER_SHADER_OPT=1" [-Minutes 40]
param([string[]]$Env = @(), [int]$Minutes = 40, [switch]$Build)
Set-Location (Split-Path $PSScriptRoot)
$env:PATH = "$env:LOCALAPPDATA\Programs\Python\Launcher;C:\Program Files\LLVM\bin;C:\Program Files\Git\cmd;$env:PATH"
if ($Build) {
  cmd /c "tools\vcenv.cmd cmake --build build\rel --target fm2 > out\build_ab.log 2>&1"
  if ($LASTEXITCODE -ne 0) { "BUILD FAILED"; Get-Content out\build_ab.log | Select-String "error" | Select-Object -First 15; exit 1 }
  "build ok"
}
$env:FM2_NO_LAUNCHER = 1; $env:FM2_DEBUG = 1; $env:FM2_PERF = 1; $env:FM2_UPSCALER = "off"
# Under "powershell -File" an array argument arrives as one comma-joined string: split on commas too.
foreach ($e in ($Env -join ',') -split ',') { if ($e -match '^\s*([^=\s]+)=(.*)$') { Set-Item "env:$($Matches[1])" $Matches[2]; "env $($Matches[1])=$($Matches[2])" } }
$log = "out\warm.err"
$p = Start-Process build\rel\fm2.exe -ArgumentList "game", "--racenow=drive=0" -RedirectStandardError $log -PassThru
$null = $p.Handle
$sw = [Diagnostics.Stopwatch]::StartNew()
while ($sw.Elapsed.TotalMinutes -lt $Minutes) {
  Start-Sleep 15
  if ($p.HasExited) { "exited after $([int]$sw.Elapsed.TotalSeconds) s"; break }
  $n = (Select-String $log -Pattern "perf: [\d.]+ fps.*\| (\d{4,}) draws/frame").Count
  if ($n -ge 20) { break }
}
Stop-Process $p -Force -ErrorAction SilentlyContinue
$c = Select-String $log -Pattern "compiled in ([\d.]+) ms"
$ms = ($c | ForEach-Object { [double]$_.Matches[0].Groups[1].Value } | Measure-Object -Sum -Maximum)
"warm-up {0:N0} s; {1} shaders compiled, sum {2:N0} s, slowest {3:N0} ms" -f $sw.Elapsed.TotalSeconds, $c.Count, ($ms.Sum / 1000), $ms.Maximum
(Select-String $log -Pattern "precompile: done" | Select-Object -Last 1).Line
(Select-String $log -Pattern "perf: [\d.]+ fps" | Select-Object -Last 1).Line
