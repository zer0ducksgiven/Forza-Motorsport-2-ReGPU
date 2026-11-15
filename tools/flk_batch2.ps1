# Flicker batch 2: build with the cull probe fix, capture it and minus_PLAIN_MUL late, A/B the probe for speed.
Set-Location (Split-Path $PSScriptRoot)
& powershell -NoProfile -ExecutionPolicy Bypass -File tools\intel_run.ps1 -Name cullprobe -Test "REGPU_HYPER_CULL_PROBE=1" -Batch flk -Pairs 3 -Build | Where-Object { $_ -match '^flk|picture|BUILD|error' }
& powershell -NoProfile -ExecutionPolicy Bypass -File tools\flicker_capture.ps1 -At 60 -Tag late -Only 'plus_CULL_PROBE|minus_PLAIN_MUL'
