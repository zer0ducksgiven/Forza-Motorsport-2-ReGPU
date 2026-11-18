# Climb 2 wave 2b: one build, then A/B pairs plus a picture check per cut on the Intel stack.
Set-Location (Split-Path $PSScriptRoot)
$tests = [ordered]@{
  dead60   = "REGPU_HYPER_DEAD_RESOLVE=60"
  dead8    = "REGPU_HYPER_DEAD_RESOLVE=8"
}
$first = $true
foreach ($k in $tests.Keys) {
  $a = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "tools\intel_run.ps1", "-Name", $k, "-Test", $tests[$k], "-Batch", "w2b", "-Pairs", 3)
  if ($first) { $a += "-Build"; $first = $false }
  & powershell @a | Where-Object { $_ -match '^w2b|picture|BUILD|error' }
}
