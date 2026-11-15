# Intel A/B: optional incremental build, then N pairs of (base, test) on the native potato stack.
# Usage: intel_ab.ps1 -Name nodiscard -Base "REGPU_HYPER_NODISCARD=0" -Test "REGPU_HYPER_NODISCARD=1" [-Pairs 2] [-Build]
param([string]$Name, [string[]]$Base = @(), [string[]]$Test = @(), [int]$Pairs = 2, [switch]$Build, [string]$Batch = "i2")
$ErrorActionPreference = 'Continue'
# Under "powershell -File" an array argument arrives as one comma-joined string: split on commas.
$Base = @(($Base -join ',') -split ',(?=[A-Za-z_][A-Za-z0-9_]*=)' | Where-Object { $_ })
$Test = @(($Test -join ',') -split ',(?=[A-Za-z_][A-Za-z0-9_]*=)' | Where-Object { $_ })
Set-Location (Split-Path $PSScriptRoot)
$env:PATH = "$env:LOCALAPPDATA\Programs\Python\Launcher;C:\Program Files\LLVM\bin;C:\Program Files\Git\cmd;$env:PATH"
if ($Build) {
  cmd /c "tools\vcenv.cmd cmake --build build\rel --target fm2 > out\build_ab.log 2>&1"
  if ($LASTEXITCODE -ne 0) { "BUILD FAILED"; Get-Content out\build_ab.log | Select-String "error" | Select-Object -First 15; exit 1 }
  "build ok"
}
$sh = "C:\Program Files\Git\bin\sh.exe"
$env:BENCH_RACENOW = "drive=0"; $env:BENCH_PERF = "1"
$P = @("FM2_UPSCALER=off","REGPU_HYPER_ANISO=0","REGPU_HYPER_HELPERS=25","REGPU_HYPER_RESOLVE1010=1","REGPU_HYPER_DECIMATE=10","REGPU_HYPER_NODISCARD=1","REGPU_HYPER_SHADER_OPT=1","REGPU_HYPER_VCACHE=2","REGPU_HYPER_PLAIN_MUL=5","REGPU_HYPER_FETCHSPEC=1", "REGPU_HYPER_SWIZZLE_SEL=1", "REGPU_HYPER_CULL_PIXELS=256", "REGPU_HYPER_CULL_ALL=1", "REGPU_HYPER_CULL_PASSES=1", "REGPU_HYPER_FREEZE=120", "REGPU_HYPER_FREEZE_REFRESH=3", "REGPU_HYPER_LEAN_LOD=1", "REGPU_HYPER_TAILSPEC=1", "REGPU_HYPER_CULL_PROBE=1", "REGPU_HYPER_CULL_HYST=200", "REGPU_HYPER_RESOLVE_BY_SIZE=1")  # Intel stack: grows as cuts are kept
function Run($label, $extra) {
  for ($a = 1; $a -le 3; $a++) {
    & $sh tools/hyper_bench.sh $label 30 @P @extra | Out-Null
    $d = Select-String "out\bench_$label.err" -Pattern "\| (\d+) draws/frame" | Select-Object -Last 1
    $n = if ($d) { [int]$d.Matches[0].Groups[1].Value } else { 0 }
    if ($n -ge 500) { break }
    Add-Content out\bench_failures.txt "${label}: stuck scene ($n draws), retry $a"
  }
}
# Warm-up: one throwaway run with the test setting, so new shader variants are compiled and cached before measuring.
Run "$Batch-$Name-warm" $Test
for ($i = 1; $i -le $Pairs; $i++) {
  Run "$Batch-$Name-base-$i" $Base
  Run "$Batch-$Name-$i" $Test
}
Select-String out\bench.txt -Pattern "^$Batch-$Name-" | ForEach-Object { $_.Line }
