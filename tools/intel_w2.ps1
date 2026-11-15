# Climb 2 wave 2a: one build, then A/B pairs plus a picture check per cut on the Intel stack.
Set-Location (Split-Path $PSScriptRoot)
$tests = [ordered]@{
  nullps   = "REGPU_HYPER_NULLPS=1"
  tail     = "REGPU_HYPER_TAILSPEC=1"
  cull384  = "REGPU_HYPER_CULL_PIXELS=384"
}
$first = $true
foreach ($k in $tests.Keys) {
  $a = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "tools\intel_run.ps1", "-Name", $k, "-Test", $tests[$k], "-Batch", "w2", "-Pairs", 3)
  if ($first) { $a += "-Build"; $first = $false }
  & powershell @a | Where-Object { $_ -match '^w2|picture|BUILD|error' }
}
