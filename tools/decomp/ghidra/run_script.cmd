@echo off
rem Runs one of these Ghidra scripts on the FM2 project, read-only:  run_script.cmd <Script.java> [args...]
if "%GHIDRA%"=="" set GHIDRA=D:\ghidra\ghidra_12.2_PUBLIC
if "%PROJECTS%"=="" set PROJECTS=D:\ghidra\projects
if "%GHIDRA_HEADLESS_MAXMEM%"=="" set GHIDRA_HEADLESS_MAXMEM=12G
set HERE=%~dp0.
call "%GHIDRA%\support\analyzeHeadless.bat" "%PROJECTS%" FM2 -process image.bin -noanalysis -readOnly ^
  -scriptPath "%HERE%" -postScript %* < NUL
