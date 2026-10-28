@echo off
rem APU benchmark: drives the same race five times on its own (no controller needed) and keeps
rem every log, to find where an integrated GPU's time goes. Copy this file into the game folder
rem (the one with build\ and out\), double-click it, and leave the laptop alone for ~15 minutes.
rem Send back the out\apu folder when it says it's finished.
cd /d "%~dp0"
if not exist build\tu\fm2.exe (
    echo Put this file in the game folder, next to the build folder.
    pause
    exit /b 1
)
if not exist out\apu mkdir out\apu

set FM2_NO_LAUNCHER=1
set FM2_NO_UPDATE_CHECK=1
set FM2_PROFILE=Benchmark
set FM2_SKIP_INTRO=1
set FM2_VSYNC=0
set FM2_MAX_FPS=360
set FM2_MSAA=1
set FM2_PERF=1
set FM2_TOUR=WAIT:7,START,IDLE,A,IDLE,A,IDLE,A,IDLE,A,IDLE,A,IDLE,A,IDLE,A,IDLE,A,IDLE,A,IDLE,WAIT:2,A,IDLE,WAIT:3,A,WAIT:20,RACE:34,QUIT

call :run base "FM2_RESOLUTION=1280x720"
call :run passes "FM2_RESOLUTION=1280x720" "FM2_GPU_PASSES=1"
call :run copypath "FM2_RESOLUTION=1280x720" "FM2_NO_GUEST_HEAP=1" "FM2_GPU_PASSES=1"
call :run noarena "FM2_RESOLUTION=1280x720" "FM2_NO_VERTEX_CACHE=1"
call :run 540p "FM2_RESOLUTION=960x540"

echo.
echo Finished. Send the out\apu folder.
pause
exit /b 0

:run
setlocal
set label=%1
shift
:args
if "%~1"=="" goto go
set "%~1"
shift
goto args
:go
echo Running %label% ...
set FM2_BENCHMARK=out\apu\%label%.txt
if defined APU_DRY (set FM2_ & endlocal & exit /b 0)
build\tu\fm2.exe > out\apu\%label%.log 2>&1
copy /y out\play.log out\apu\%label%-play.log > nul 2>&1
endlocal
exit /b 0
