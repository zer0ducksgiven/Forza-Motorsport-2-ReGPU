@echo off
rem Racing wheel test: the game with the wheel driver, its log kept as out\wheel-test.log.
rem The strength and the rest come from the launcher's Racing wheel settings.
rem   wheel-test.cmd 40     overrides the strength (percent) for this run
cd /d "%~dp0"
if not exist out mkdir out
set FM2_NO_LAUNCHER=1
if not "%~1"=="" set FM2_WHEEL_STRENGTH=%~1
build\tu\fm2.exe > out\wheel-test.log 2>&1
echo Log: out\wheel-test.log
