# Climb 2 Track G uniform scene scale: one build, then A/B pairs plus a picture check per cut on the Intel stack.
Set-Location (Split-Path $PSScriptRoot)
$tests = [ordered]@{
  sc90     = "FM2_UPSCALER=fsr1,FM2_FSR_SCALE=90"
  sc80     = "FM2_UPSCALER=fsr1,FM2_FSR_SCALE=80"
  sc70     = "FM2_UPSCALER=fsr1,FM2_FSR_SCALE=70"
  hd720    = "FM2_RESOLUTION=1920x1080,FM2_UPSCALER=fsr1,FM2_FSR_SCALE=67"
  hd576    = "FM2_RESOLUTION=1920x1080,FM2_UPSCALER=fsr1,FM2_FSR_SCALE=53"
}
$first = $true
foreach ($k in $tests.Keys) {
  $a = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "tools\intel_run.ps1", "-Name", $k, "-Test", $tests[$k], "-Batch", "g2", "-Pairs", 3)
  if ($first) { $a += "-Build"; $first = $false }
  & powershell @a | Where-Object { $_ -match '^g2|picture|BUILD|error' }
}
