# One experiment: optional build, A/B pairs on the Intel stack, then a screenshot pair and a picture diff.
# Usage: intel_run.ps1 -Name x -Test "A=1,B=2" [-Base "C=0"] [-Pairs 2] [-Build] [-Batch i4]
param([string]$Name, [string]$Test, [string]$Base = "REGPU_HYPER_PREPASS=0", [int]$Pairs = 2, [switch]$Build, [string]$Batch = "i4")
Set-Location (Split-Path $PSScriptRoot)
$env:PATH = "$env:LOCALAPPDATA\Programs\Python\Launcher;$env:PATH"
$a = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "tools\intel_ab.ps1", "-Name", $Name, "-Base", $Base, "-Test", $Test, "-Batch", $Batch, "-Pairs", $Pairs)
if ($Build) { $a += "-Build" }
& powershell @a | ForEach-Object {
  if ($_ -match '^(\S+).*GPU\s+([\d.]+).*compiled during race (\d+).*median ([\d.]+)') { "{0,-22} GPU {1,6}  median {2,5}  compiled {3}" -f $Matches[1], $Matches[2], $Matches[4], $Matches[3] } else { $_ }
}
$sh = "C:\Program Files\Git\bin\sh.exe"; $env:BENCH_RACENOW = "drive=0"; $env:BENCH_PERF = ""
$P = @("FM2_UPSCALER=off", "REGPU_HYPER_ANISO=0", "REGPU_HYPER_HELPERS=25", "REGPU_HYPER_RESOLVE1010=1", "REGPU_HYPER_DECIMATE=10", "REGPU_HYPER_NODISCARD=1", "REGPU_HYPER_SHADER_OPT=1", "REGPU_HYPER_VCACHE=2", "REGPU_HYPER_PLAIN_MUL=5", "REGPU_HYPER_FETCHSPEC=1", "REGPU_HYPER_SWIZZLE_SEL=1", "REGPU_HYPER_CULL_PIXELS=256", "REGPU_HYPER_CULL_ALL=1", "REGPU_HYPER_CULL_PASSES=1", "REGPU_HYPER_FREEZE=120", "REGPU_HYPER_FREEZE_REFRESH=3", "REGPU_HYPER_LEAN_LOD=1", "REGPU_HYPER_TAILSPEC=1", "REGPU_HYPER_CULL_PROBE=1", "REGPU_HYPER_CULL_HYST=200", "REGPU_HYPER_RESOLVE_BY_SIZE=1", "FM2_DUMP_AT=25")
$b = @($Base -split ',' | Where-Object { $_ }); $t = @($Test -split ',' | Where-Object { $_ })
& $sh tools/hyper_bench.sh "shot-$Batch-$Name-base" 30 @P @b | Out-Null
& $sh tools/hyper_bench.sh "shot-$Batch-$Name" 30 @P @t | Out-Null
"$Name picture: " + (py -3 -I tools\imgdiff.py "out\bench_shot-$Batch-$Name-base.png" "out\bench_shot-$Batch-$Name.png" "out\${Batch}_${Name}_compare.png" 2>$null)
