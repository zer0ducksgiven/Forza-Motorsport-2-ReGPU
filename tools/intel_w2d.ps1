# Climb 2 wave 2d (tuning sweep): one build, then A/B pairs plus a picture check per cut on the Intel stack.
Set-Location (Split-Path $PSScriptRoot)
$tests = [ordered]@{
  help15   = "REGPU_HYPER_HELPERS=15"
  decim5   = "REGPU_HYPER_DECIMATE=5"
  frz5     = "REGPU_HYPER_FREEZE_REFRESH=5"
}
$first = $true
foreach ($k in $tests.Keys) {
  $a = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "tools\intel_run.ps1", "-Name", $k, "-Test", $tests[$k], "-Batch", "w2d", "-Pairs", 3)
  if ($first) { $a += "-Build"; $first = $false }
  & powershell @a | Where-Object { $_ -match '^w2d|picture|BUILD|error' }
}
