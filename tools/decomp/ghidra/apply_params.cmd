@echo off
rem Writes the real parameters (tools/decomp/livein.py's out/decomp/livein.txt) into the Ghidra project:
rem   apply_params.cmd [livein.txt]
if "%GHIDRA%"=="" set GHIDRA=D:\ghidra\ghidra_12.2_PUBLIC
if "%PROJECTS%"=="" set PROJECTS=D:\ghidra\projects
if "%GHIDRA_HEADLESS_MAXMEM%"=="" set GHIDRA_HEADLESS_MAXMEM=12G
set HERE=%~dp0.
set LIVEIN=%1
if "%LIVEIN%"=="" set LIVEIN=%HERE%\..\..\..\out\decomp\livein.txt
call "%GHIDRA%\support\analyzeHeadless.bat" "%PROJECTS%" FM2 -process image.bin -noanalysis ^
  -scriptPath "%HERE%" -postScript ApplyParamsFm2.java "%LIVEIN%" < NUL
