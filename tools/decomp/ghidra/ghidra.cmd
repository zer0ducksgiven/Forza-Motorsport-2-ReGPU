@echo off
rem Opens Ghidra's window (with the JDK and settings folders launch.properties points at); then File > Open
rem Project > D:\ghidra\projects\FM2.gpr, and double-click image.bin.
if "%GHIDRA%"=="" set GHIDRA=D:\ghidra\ghidra_12.2_PUBLIC
if "%GHIDRA_MAXMEM%"=="" set GHIDRA_MAXMEM=8G
start "" "%GHIDRA%\ghidraRun.bat"
