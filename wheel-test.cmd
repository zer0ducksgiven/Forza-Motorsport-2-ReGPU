@echo off
rem Racing wheel test: the game with the wheel driver at a low strength, its log kept as out\wheel-test.log.
rem Before a race: press each pedal on its own, then each button and paddle, then drive.
rem   wheel-test.cmd 40     the strength in percent (25 by default)
cd /d "%~dp0"
if not exist out mkdir out
set FM2_NO_LAUNCHER=1
set FM2_WHEEL_STRENGTH=25
if not "%~1"=="" set FM2_WHEEL_STRENGTH=%~1
echo Wheel strength %FM2_WHEEL_STRENGTH%%%
build\tu\fm2.exe > out\wheel-test.log 2>&1
echo Log: out\wheel-test.log
