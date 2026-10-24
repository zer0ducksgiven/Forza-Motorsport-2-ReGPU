@echo off
rem Performance test for this PC (not part of the installer, so updates keep it).
rem Runs the benchmark race with FM2_PERF=1 and keeps each run's results in out\runs\<time>_<label>\,
rem so nothing is overwritten and Claude can read every run over the network.
rem
rem   perf-test.cmd          the normal run
rem   perf-test.cmd ab       three rounds of: the normal run, then each A/B variant (about 2 minutes
rem                          each, 25 minutes in all; perf-test-ab.cmd does this on a double-click)
rem Single runs on a slow PC vary by up to half: compare the medians of the rounds.
cd /d "%~dp0"
if not exist out mkdir out
if not exist out\runs mkdir out\runs
call "%~dp0install-content.cmd"

if /i not "%~1"=="ab" (
    call :run normal
    goto done
)
for %%r in (1 2 3) do (
    call :run normal
    call :run copy-path "FM2_NO_GUEST_HEAP=1"
    call :run low-performance "FM2_LOW_PERFORMANCE=1"
    call :run no-prehash "FM2_NO_TEXTURE_PREHASH=1"
)
goto done

:run
setlocal
set label=%~1
if not "%~2"=="" set %~2
set FM2_NO_LAUNCHER=1
set FM2_PROFILE=Benchmark
set FM2_MAX_FPS=360
set FM2_VSYNC=0
set FM2_PERF=1
set FM2_BENCHMARK=out\benchmark.txt
set FM2_TOUR=WAIT:7,START,IDLE,A,IDLE,A,IDLE,A,IDLE,A,IDLE,A,IDLE,A,IDLE,A,IDLE,A,IDLE,A,IDLE,WAIT:2,A,IDLE,WAIT:3,RACE:34,QUIT
if exist out\benchmark.txt del out\benchmark.txt
for /f %%t in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd-HHmmss"') do set stamp=%%t
set dir=out\runs\%stamp%_%label%
mkdir "%dir%"
echo.
echo === %label% %~2
build\tu\fm2.exe > "%dir%\benchmark.log" 2>&1
if exist out\benchmark.txt (move /y out\benchmark.txt "%dir%\benchmark.txt" >nul & type "%dir%\benchmark.txt") else (echo The benchmark did not finish: see %dir%\benchmark.log)
endlocal
goto :eof

:done
echo.
echo Results are in out\runs\
pause
