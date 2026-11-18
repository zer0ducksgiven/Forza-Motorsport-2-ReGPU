# Climb 2 Track G uniform scene scale: one build, then A/B pairs plus a picture check per cut on the Intel stack.
Set-Location (Split-Path $PSScriptRoot)
$tests = [ordered]@{
  sc90     = "REGPU_HYPER_SCENE=90,FM2_UPSCALER=fsr1"
  sc80     = "REGPU_HYPER_SCENE=80,FM2_UPSCALER=fsr1"
  sc70     = "REGPU_HYPER_SCENE=70,FM2_UPSCALER=fsr1"
}
$first = $true
foreach ($k in $tests.Keys) {
  $a = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "tools\intel_run.ps1", "-Name", $k, "-Test", $tests[$k], "-Batch", "g1", "-Pairs", 3)
  if ($first) { $a += "-Build"; $first = $false }
  & powershell @a | Where-Object { $_ -match '^g1|picture|BUILD|error' }
}
