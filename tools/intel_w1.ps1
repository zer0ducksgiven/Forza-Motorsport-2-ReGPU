# Climb 2 wave 1: one build, then A/B pairs plus a picture check per cut on the Intel stack.
Set-Location (Split-Path $PSScriptRoot)
$tests = [ordered]@{
  tail     = "REGPU_HYPER_TAILSPEC=1"
  bias     = "REGPU_HYPER_BIASSPEC=1"
  flow1    = "REGPU_HYPER_SHADER_FLOW=1"
  flow2    = "REGPU_HYPER_SHADER_FLOW=2"
  cull128  = "REGPU_HYPER_CULL_PIXELS=128"
  cull256  = "REGPU_HYPER_CULL_PIXELS=256"
  frz4     = "REGPU_HYPER_FREEZE_REFRESH=4"
  frz6     = "REGPU_HYPER_FREEZE_REFRESH=6"
  frzshad  = "REGPU_HYPER_FREEZE_SHADOWS=1"
}
$first = $true
foreach ($k in $tests.Keys) {
  $a = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "tools\intel_run.ps1", "-Name", $k, "-Test", $tests[$k], "-Batch", "w1", "-Pairs", 2)
  if ($first) { $a += "-Build"; $first = $false }
  & powershell @a | Where-Object { $_ -match '^w1|picture|BUILD|error' }
}
