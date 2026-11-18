$root = "$env:USERPROFILE\Desktop\reGPU_Hyper\Forza-Motorsport-2-ReGPU"
Set-Location $root
$sh = "C:\Program Files\Git\bin\sh.exe"
$env:BENCH_RACENOW = "drive=0"; $env:BENCH_PERF = "1"
$P = @("FM2_UPSCALER=off","REGPU_HYPER_ANISO=0","REGPU_HYPER_HELPERS=25","REGPU_HYPER_RESOLVE1010=1","REGPU_HYPER_DECIMATE=10")
$tests = [ordered]@{
  "bilinear"   = @("REGPU_MEASURE_BILINEAR=1")
  "nodepthat"  = @("FM2_NO_DEPTH_ALPHATEST=1")
  "workers3"   = @("FM2_DRAW_WORKERS=3")
  "decloose"   = @("REGPU_HYPER_DECIMATE_ERROR=0.6","REGPU_HYPER_DECIMATE_MIN=100")
  "C-empty"    = @("REGPU_MEASURE_EMPTY_DRAWS=1")
  "C-nocars"   = @("REGPU_MEASURE_SKIP_BLENDED=4")
  "C-noblend"  = @("REGPU_MEASURE_SKIP_BLENDED=1")
  "C-nohelp"   = @("REGPU_MEASURE_SKIP_PITCH=560,280,320")
  "C-fasttf"   = @("REGPU_MEASURE_FAST_TFETCH=1")
  "C-noat"     = @("REGPU_MEASURE_NO_ALPHATEST=1")
  "C-noresolve"= @("REGPU_MEASURE_SKIP_RESOLVE=1000")
}
function Run($label, $extra) {
  for ($a = 1; $a -le 3; $a++) {
    & $sh tools/hyper_bench.sh $label 30 @P @extra | Out-Null
    $d = Select-String "out\bench_$label.err" -Pattern "\| (\d+) draws/frame" | Select-Object -Last 1
    $n = if ($d) { [int]$d.Matches[0].Groups[1].Value } else { 0 }
    if ($n -ge 500 -or $label -like "C-empty*") { break }
    Add-Content out\bench_failures.txt "${label}: stuck scene ($n draws), retry $a"
  }
}
$i = 0
foreach ($r in 1, 2) {
  foreach ($k in $tests.Keys) {
    if ($i % 4 -eq 0) { Run ("i1-base-{0}" -f ($i / 4 + 1)) @() }
    Run "i1-$k-$r" $tests[$k]
    $i++
  }
}
Select-String out\bench.txt -Pattern "^i1-" | ForEach-Object { $_.Line }
