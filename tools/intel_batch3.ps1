# Batch 3: the four phase-1 experiments first skipped on measured grounds (sloppy and welded decimation, 8-bit colour
# surfaces, whole-surface clears). One build, an A/B each, then a screenshot each for the picture check.
Set-Location (Split-Path $PSScriptRoot)
$ab = "tools\intel_ab.ps1"
$tests = [ordered]@{ sloppy = "REGPU_HYPER_DECIMATE_SLOPPY=1"; weld = "REGPU_HYPER_DECIMATE_WELD=1"; col8 = "REGPU_MEASURE_COLOR8=1"; clears = "REGPU_HYPER_FULL_CLEAR=1" }
$first = $true
foreach ($k in $tests.Keys) {
  $args2 = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $ab, "-Name", $k, "-Base", "REGPU_HYPER_PREPASS=0", "-Test", $tests[$k], "-Batch", "i3")
  if ($first) { $args2 += "-Build"; $first = $false }
  & powershell @args2 | ForEach-Object { if ($_ -match '^(\S+).*GPU\s+([\d.]+).*compiled during race (\d+).*median ([\d.]+)') { "{0,-20} GPU {1}  median {3}  compiled {2}" -f $Matches[1], $Matches[2], $Matches[3], $Matches[4] } else { $_ } }
  if ($LASTEXITCODE -ne 0 -and $k -eq 'sloppy') { "build failed, stopping"; exit 1 }
}
$env:PATH = "$env:LOCALAPPDATA\Programs\Python\Launcher;$env:PATH"
$sh = "C:\Program Files\Git\bin\sh.exe"; $env:BENCH_RACENOW = "drive=0"; $env:BENCH_PERF = ""
$P = @("FM2_UPSCALER=off", "REGPU_HYPER_ANISO=0", "REGPU_HYPER_HELPERS=25", "REGPU_HYPER_RESOLVE1010=1", "REGPU_HYPER_DECIMATE=10", "REGPU_HYPER_NODISCARD=1", "REGPU_HYPER_SHADER_OPT=1", "FM2_DUMP_AT=25")
& $sh tools/hyper_bench.sh shot-i3-base 30 @P | Out-Null
foreach ($k in $tests.Keys) {
  & $sh tools/hyper_bench.sh "shot-i3-$k" 30 @P $tests[$k] | Out-Null
  "$k picture: " + (py -3 -I tools\imgdiff.py out\bench_shot-i3-base.png "out\bench_shot-i3-$k.png" "out\i3_${k}_compare.png" 2>$null)
}
