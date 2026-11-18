# Climb 2 wave 2c: one build, then A/B pairs plus a picture check per cut on the Intel stack.
Set-Location (Split-Path $PSScriptRoot)
$tests = [ordered]@{
  predflat = "REGPU_HYPER_PRED_FLATTEN=1"
}
$first = $true
foreach ($k in $tests.Keys) {
  $a = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "tools\intel_run.ps1", "-Name", $k, "-Test", $tests[$k], "-Batch", "w2c", "-Pairs", 3)
  if ($first) { $a += "-Build"; $first = $false }
  & powershell @a | Where-Object { $_ -match '^w2c|picture|BUILD|error' }
}
