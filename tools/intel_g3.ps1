# Climb 2 Track G uniform scene scale: one build, then A/B pairs plus a picture check per cut on the Intel stack.
Set-Location (Split-Path $PSScriptRoot)
$tests = [ordered]@{
  hd900    = "FM2_RESOLUTION=1600x900,FM2_UPSCALER=fsr1,FM2_FSR_SCALE=80"
}
$first = $true
foreach ($k in $tests.Keys) {
  $a = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "tools\intel_run.ps1", "-Name", $k, "-Test", $tests[$k], "-Batch", "g3", "-Pairs", 3)
  & powershell @a | Where-Object { $_ -match '^g3|picture|BUILD|error' }
}
