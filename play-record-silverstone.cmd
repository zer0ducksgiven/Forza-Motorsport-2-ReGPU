@echo off
rem Records your inputs for a Silverstone run to out\silverstone_input.txt (replay with FM2_REPLAY_INPUT).
rem Same settings as the shadow tests: Benchmark profile, intro skipped, 60 fps cap.
cd /d "%~dp0"
if not exist out mkdir out
set FM2_NO_LAUNCHER=1
set FM2_PROFILE=Benchmark
set FM2_SKIP_INTRO=1
set FM2_MAX_FPS=60
set FM2_RECORD_INPUT=out\silverstone_input.txt
build\tu\fm2.exe > out\play.log 2>&1
