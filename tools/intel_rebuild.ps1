# Regenerate out/ppc (XenonRecomp) and build fm2 on the Intel box. Log: out/build_intel.log
$ErrorActionPreference = 'Stop'
Set-Location (Split-Path $PSScriptRoot)
$env:PATH = "$env:LOCALAPPDATA\Programs\Python\Launcher;C:\Program Files\LLVM\bin;C:\Program Files\Git\usr\bin;$env:PATH"
$log = 'out\build_intel.log'
Copy-Item src\kernel\stubs.cpp out\stubs.cpp.before-recomp -Force
function Step($name, $cmd) {
  Add-Content $log "===== $name"
  cmd /c "$cmd >> $log 2>&1"
  if ($LASTEXITCODE -ne 0) { Add-Content $log "FAILED: $name ($LASTEXITCODE)"; throw "$name failed" }
}
Set-Content $log "start $(Get-Date)"
if (-not (Test-Path build\tools\build.ninja)) {
  Step 'configure tools' 'tools\vcenv.cmd cmake -S tools/xexdump -B build/tools -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl'
}
Step 'recomp' '"C:\Program Files\Git\bin\sh.exe" tools/recomp.sh'
$diff = Compare-Object (Get-Content out\stubs.cpp.before-recomp) (Get-Content src\kernel\stubs.cpp)
if ($diff) { Add-Content $log "stubs.cpp changed by genstubs ($($diff.Count) lines); restoring the Hyper version"; Copy-Item out\stubs.cpp.before-recomp src\kernel\stubs.cpp -Force }
Remove-Item build\rel\CMakeCache.txt -ErrorAction SilentlyContinue
Step 'configure rel' 'tools\vcenv.cmd cmake -G Ninja -B build\rel -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl'
Step 'build fm2' 'tools\vcenv.cmd cmake --build build\rel --target fm2'
Add-Content $log "done $(Get-Date)"
