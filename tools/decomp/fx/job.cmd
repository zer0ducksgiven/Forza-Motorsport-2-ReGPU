@echo off
rem Runs each line of lift\job.txt (written by Claude over SSH) on the desktop; output in lift\job.out,
rem lift\job.done when it ends.
cd /d "%~dp0.."
set FM2_ROOT=%CD%
set FM2_EXE=%CD%\build\tu\fm2-lift.exe
del lift\job.done 2>nul
del lift\job.out 2>nul
for /f "usebackq delims=" %%c in ("lift\job.txt") do %%c >> lift\job.out 2>&1
echo done > lift\job.done
