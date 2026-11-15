# Climb 2 Track G uniform scene scale: one build, then A/B pairs plus a picture check per cut on the Intel stack.
Set-Location (Split-Path $PSScriptRoot)
$tests = [ordered]@{
  mulceil  = "REGPU_HYPER_PLAIN_MUL=1"
}
$first = $true
foreach ($k in $tests.Keys) {
  $a = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "tools\intel_run.ps1", "-Name", $k, "-Test", $tests[$k], "-Batch", "w4", "-Pairs", 3)
  & powershell @a | Where-Object { $_ -match '^w4|picture|BUILD|error' }
}
