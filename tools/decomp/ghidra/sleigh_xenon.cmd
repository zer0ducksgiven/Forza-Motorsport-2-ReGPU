@echo off
rem Compiles the Xenon language in the Ghidra install (after install_xenon.py has copied it there).
if "%GHIDRA%"=="" set GHIDRA=D:\ghidra\ghidra_12.2_PUBLIC
call "%GHIDRA%\support\sleigh.bat" "%GHIDRA%\Ghidra\Processors\PowerPC\data\languages\ppc_64_xenon_be.slaspec" < NUL
