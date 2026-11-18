# Laptop (AMD) cuts retested on the Intel stack after the merge: A/B pairs plus a picture check each.
Set-Location (Split-Path $PSScriptRoot)
$tests = [ordered]@{
  cull      = "REGPU_HYPER_CULL_PIXELS=64,REGPU_HYPER_CULL_ALL=1"
  leanvf    = "REGPU_HYPER_LEAN_VFETCH=1"
  half      = "REGPU_MEASURE_HALF=1"
  a2m       = "REGPU_HYPER_A2M_TEST=1"
  freeze    = "REGPU_HYPER_FREEZE=120,REGPU_HYPER_FREEZE_REFRESH=3"
  cullpass  = "REGPU_HYPER_CULL_PIXELS=64,REGPU_HYPER_CULL_ALL=1,REGPU_HYPER_CULL_PASSES=1"
}
foreach ($k in $tests.Keys) {
  & powershell -NoProfile -ExecutionPolicy Bypass -File tools\intel_run.ps1 -Name $k -Test $tests[$k] -Batch m2 -Pairs 2 | Where-Object { $_ -match '^m2|picture' }
}
