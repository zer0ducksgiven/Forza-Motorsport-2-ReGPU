# Sloppy decimation with the HUD excluded and a bounded error: one build, then a screenshot + A/B per error bound.
Set-Location (Split-Path $PSScriptRoot)
$env:PATH = "$env:LOCALAPPDATA\Programs\Python\Launcher;C:\Program Files\LLVM\bin;C:\Program Files\Git\cmd;$env:PATH"
cmd /c "tools\vcenv.cmd cmake --build build\rel --target fm2 > out\build_ab.log 2>&1"
if ($LASTEXITCODE -ne 0) { "BUILD FAILED"; Get-Content out\build_ab.log | Select-String "error" | Select-Object -First 15; exit 1 }
"build ok"
$sh = "C:\Program Files\Git\bin\sh.exe"; $env:BENCH_RACENOW = "drive=0"
$P = @("FM2_UPSCALER=off", "REGPU_HYPER_ANISO=0", "REGPU_HYPER_HELPERS=25", "REGPU_HYPER_RESOLVE1010=1", "REGPU_HYPER_DECIMATE=10", "REGPU_HYPER_NODISCARD=1", "REGPU_HYPER_SHADER_OPT=1", "FM2_DUMP_AT=25")
foreach ($err in "1.0", "0.05", "0.02", "0.01") {
  $env:BENCH_PERF = ""
  & $sh tools/hyper_bench.sh "shot-sl-$err" 30 @P REGPU_HYPER_DECIMATE_SLOPPY=1 "REGPU_HYPER_DECIMATE_SLOPPY_ERROR=$err" | Out-Null
  "error $err picture: " + (py -3 -I tools\imgdiff.py out\bench_shot-i3-base.png "out\bench_shot-sl-$err.png" "out\sloppy_${err}_compare.png" 2>$null)
  & powershell -NoProfile -ExecutionPolicy Bypass -File tools\intel_ab.ps1 -Name "sl$($err -replace '\.','')" -Base "REGPU_HYPER_PREPASS=0" -Test "REGPU_HYPER_DECIMATE_SLOPPY=1,REGPU_HYPER_DECIMATE_SLOPPY_ERROR=$err" -Batch i3 -Pairs 1 |
    ForEach-Object { if ($_ -match '^(\S+).*GPU\s+([\d.]+).*compiled during race (\d+).*median ([\d.]+)') { "{0,-20} GPU {1}  median {3}" -f $Matches[1], $Matches[2], $Matches[4] } }
}
