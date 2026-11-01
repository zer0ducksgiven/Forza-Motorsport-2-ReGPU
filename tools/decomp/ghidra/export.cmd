@echo off
rem Decompiles every function of the Ghidra project (import.cmd) into C files and a quality report:
rem   export.cmd [out folder]       default D:\ghidra\export
if "%GHIDRA%"=="" set GHIDRA=D:\ghidra\ghidra_12.2_PUBLIC
if "%PROJECTS%"=="" set PROJECTS=D:\ghidra\projects
if "%GHIDRA_HEADLESS_MAXMEM%"=="" set GHIDRA_HEADLESS_MAXMEM=12G
set HERE=%~dp0.
set DEST=%~1
if "%DEST%"=="" set DEST=D:\ghidra\export
call "%GHIDRA%\support\analyzeHeadless.bat" "%PROJECTS%" FM2 -process image.bin -noanalysis -readOnly ^
  -scriptPath "%HERE%" -postScript ExportAllFm2.java "%DEST%" -max-cpu 12 < NUL
