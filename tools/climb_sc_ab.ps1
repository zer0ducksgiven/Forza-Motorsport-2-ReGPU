Set-Location (Split-Path $PSScriptRoot)
$tests = [ordered]@{
  frzshad = "REGPU_HYPER_FREEZE_SHADOWS=1,REGPU_HYPER_RESOLVE_BY_SIZE=1"
  bysize  = "REGPU_HYPER_RESOLVE_BY_SIZE=1"
  hyst150 = "REGPU_HYPER_CULL_HYST=150"
  hyst200 = "REGPU_HYPER_CULL_HYST=200"
}
foreach ($k in $tests.Keys) {
  & powershell -NoProfile -ExecutionPolicy Bypass -File tools\intel_run.ps1 -Name $k -Test $tests[$k] -Batch sc -Pairs 3 | Where-Object { $_ -match '^sc|picture|BUILD|error' }
}
