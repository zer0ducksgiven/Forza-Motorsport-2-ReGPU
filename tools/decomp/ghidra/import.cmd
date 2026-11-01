@echo off
rem Imports FM2's TU4 image into a Ghidra project (D:\ghidra\projects\FM2) with the symbol map, then analyses it.
rem Needs out\tu4\image.bin and out\decomp\symbols.txt (tools\decomp\symbols.py). GHIDRA defaults to D:\ghidra\ghidra_12.2_PUBLIC,
rem with the Xenon language installed (install_xenon.py: VMX128).
if "%GHIDRA%"=="" set GHIDRA=D:\ghidra\ghidra_12.2_PUBLIC
if "%PROJECTS%"=="" set PROJECTS=D:\ghidra\projects
if not exist "%PROJECTS%" mkdir "%PROJECTS%"
rem 50,000 functions don't fit in the default 2 GB
if "%GHIDRA_HEADLESS_MAXMEM%"=="" set GHIDRA_HEADLESS_MAXMEM=12G
rem %~dp0 ends in a backslash, which would escape a closing quote
set HERE=%~dp0.
set OUT=%HERE%\..\..\..\out
rem a copy of the symbols: symbols.py rewriting them during an import left Ghidra with half the functions
copy /y "%OUT%\decomp\symbols.txt" "%PROJECTS%\symbols.txt" > NUL
call "%GHIDRA%\support\analyzeHeadless.bat" "%PROJECTS%" FM2 -import "%OUT%\tu4\image.bin" -overwrite ^
  -loader BinaryLoader -loader-baseAddr 0x82000000 -processor PowerPC:BE:64:Xenon ^
  -scriptPath "%HERE%" -preScript ImportFm2Symbols.java "%PROJECTS%\symbols.txt" ^
  -max-cpu 8 -log "%PROJECTS%\fm2-import.log" -scriptlog "%PROJECTS%\fm2-script.log" < NUL
