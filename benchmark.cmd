@echo off
rem FM2 benchmark: drives the menus into a Sunset race (8 cars), holds the throttle for
rem 25 seconds and writes out\benchmark.txt (average fps, lows, CPU, GPU, settings).
rem Uses your saved launcher settings (resolution, upscaler) with no frame cap or VSync,
rem and its own "Benchmark" profile so every machine starts from the same save.
rem Start the game once with play.cmd first: the first start compiles shaders.
rem Do not touch the keyboard or controller while it runs (about 3 minutes: the intro videos play first).
cd /d "%~dp0"
if not exist out mkdir out
call "%~dp0install-content.cmd"
if exist out\benchmark.txt del out\benchmark.txt
set FM2_NO_LAUNCHER=1
set FM2_PROFILE=Benchmark
set FM2_MAX_FPS=360
set FM2_VSYNC=0
set FM2_BENCHMARK=out\benchmark.txt
set FM2_TOUR=WAIT:7,START,IDLE,A,IDLE,A,IDLE,A,IDLE,A,IDLE,A,IDLE,A,IDLE,A,IDLE,A,IDLE,A,IDLE,WAIT:2,A,IDLE,WAIT:3,RACE:34,QUIT
build\tu\fm2.exe > out\benchmark.log 2>&1
echo.
if exist out\benchmark.txt (type out\benchmark.txt) else (echo The benchmark did not finish: see out\benchmark.log)
echo.
pause
