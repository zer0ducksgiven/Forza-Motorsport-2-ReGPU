@echo off
rem Decompiles functions from the Ghidra project (import.cmd) into a file, without opening Ghidra:
rem   decompile.cmd <out.c> <TU4 address> [<TU4 address> ...]
if "%GHIDRA%"=="" set GHIDRA=D:\ghidra\ghidra_12.2_PUBLIC
if "%PROJECTS%"=="" set PROJECTS=D:\ghidra\projects
if "%GHIDRA_HEADLESS_MAXMEM%"=="" set GHIDRA_HEADLESS_MAXMEM=12G
set HERE=%~dp0.
call "%GHIDRA%\support\analyzeHeadless.bat" "%PROJECTS%" FM2 -process image.bin -noanalysis -readOnly ^
  -scriptPath "%HERE%" -postScript DecompileFm2.java %* < NUL
