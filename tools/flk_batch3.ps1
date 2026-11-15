# Flicker batch 3: rebuild (Intel potato default now has the cull probe), warm the swizzle-off shader cache and capture
# it, then driving captures (auto-drive) for motion artefacts.
Set-Location (Split-Path $PSScriptRoot)
cmd /c "tools\vcenv.cmd cmake --build build\rel --target fm2 > out\build_ab.log 2>&1"
if ($LASTEXITCODE -ne 0) { "BUILD FAILED"; exit 1 }
"build ok"
$sh = "C:\Program Files\Git\bin\sh.exe"; $env:BENCH_RACENOW = "drive=0"
$P = @("FM2_UPSCALER=off", "REGPU_HYPER_ANISO=0", "REGPU_HYPER_HELPERS=25", "REGPU_HYPER_RESOLVE1010=1", "REGPU_HYPER_DECIMATE=10", "REGPU_HYPER_NODISCARD=1", "REGPU_HYPER_SHADER_OPT=1", "REGPU_HYPER_VCACHE=2", "REGPU_HYPER_PLAIN_MUL=5", "REGPU_HYPER_FETCHSPEC=1", "REGPU_HYPER_CULL_PIXELS=256", "REGPU_HYPER_CULL_ALL=1", "REGPU_HYPER_CULL_PASSES=1", "REGPU_HYPER_FREEZE=120", "REGPU_HYPER_FREEZE_REFRESH=3", "REGPU_HYPER_LEAN_LOD=1", "REGPU_HYPER_TAILSPEC=1", "REGPU_HYPER_CULL_PROBE=1")
$env:BENCH_RERUN = ""
# warm: one long run that compiles the swizzle-off shaders (failures are fine)
& $sh -c "cd '$((Get-Location).Path -replace '\','/')' && env $($P -join ' ') timeout 600 ./build/rel/fm2.exe game --racenow drive=0 > out/flk_warm.err 2>&1; true"
"warm done"
& powershell -NoProfile -ExecutionPolicy Bypass -File tools\flicker_capture.ps1 -At 60 -Tag late -Only 'minus_SWIZZLE_SEL'
& powershell -NoProfile -ExecutionPolicy Bypass -File tools\flicker_capture.ps1 -At 60 -Tag drive -Drive "drive=1" -Only '^stock$|^stack$|^stack2$|minus_CULL_PIXELS|minus_DECIMATE|minus_FREEZE$|minus_HELPERS|minus_CULL_PROBE|minus_VCACHE'
