# Flicker study: one run per configuration, 60 consecutive frames dumped from 30 s in (car standing on the grid),
# kept in out/flicker/<name>/. Configs: stock, full stack x2, the stack minus each switch, the stack plus each extra.
param([int]$At = 30, [string]$Only = "", [string]$Drive = "drive=0", [string]$Tag = "")
Set-Location (Split-Path $PSScriptRoot)
$sh = "C:\Program Files\Git\bin\sh.exe"; $env:BENCH_RACENOW = $Drive; $env:BENCH_PERF = ""
$P = @("FM2_UPSCALER=off", "REGPU_HYPER_ANISO=0", "REGPU_HYPER_HELPERS=25", "REGPU_HYPER_RESOLVE1010=1", "REGPU_HYPER_DECIMATE=10", "REGPU_HYPER_NODISCARD=1", "REGPU_HYPER_SHADER_OPT=1", "REGPU_HYPER_VCACHE=2", "REGPU_HYPER_PLAIN_MUL=5", "REGPU_HYPER_FETCHSPEC=1", "REGPU_HYPER_SWIZZLE_SEL=1", "REGPU_HYPER_CULL_PIXELS=256", "REGPU_HYPER_CULL_ALL=1", "REGPU_HYPER_CULL_PASSES=1", "REGPU_HYPER_FREEZE=120", "REGPU_HYPER_FREEZE_REFRESH=3", "REGPU_HYPER_LEAN_LOD=1", "REGPU_HYPER_TAILSPEC=1", "REGPU_HYPER_CULL_PROBE=1")
$cfg = [ordered]@{ "stock" = @("FM2_UPSCALER=off"); "stack" = $P; "stack2" = $P }
foreach ($s in $P) { if ($s -like "REGPU_HYPER_*") { $cfg["minus_" + ($s -replace '^REGPU_HYPER_|=.*$', '')] = @($P | Where-Object { $_ -ne $s }) } }
$extras = @("REGPU_HYPER_LEAN_FETCH=1", "REGPU_HYPER_LEAN_VFETCH=1", "REGPU_HYPER_A2M_TEST=1", "REGPU_HYPER_CULL_RETEST=4", "REGPU_HYPER_UPSCALE=1", "REGPU_HYPER_FREEZE_SHADOWS=1", "REGPU_HYPER_CULL_PROBE=1")
foreach ($e in $extras) { $cfg["plus_" + ($e -replace '^REGPU_HYPER_|=.*$', '')] = $P + @($e) }
New-Item -ItemType Directory -Force "out\flicker$Tag" | Out-Null
foreach ($k in $cfg.Keys) {
  if ($Only -and $k -notmatch $Only) { continue }
  $dest = "out\flicker$Tag\$k"
  if (Test-Path "$dest\059.png") { continue }
  if (Test-Path out\sweep) { Remove-Item -Recurse -Force out\sweep }
  & $sh tools/hyper_bench.sh "flk-$Tag$k" ($At + 10) @($cfg[$k]) "FM2_DUMP_AT=$At" "FM2_SWEEP=0,0,60" | Out-Null
  if (Test-Path $dest) { Remove-Item -Recurse -Force $dest }
  if (Test-Path out\sweep) { Move-Item out\sweep $dest; "$k captured " + (Get-ChildItem $dest).Count } else { "$k FAILED (no frames)" }
}
