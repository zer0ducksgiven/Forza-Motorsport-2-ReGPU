@echo off
rem The FM2 reports desk: bug reports and shared play.logs from the private fm2-reports repo (needs gh, logged in).
cd /d "%~dp0"
start "" http://127.0.0.1:8790
py -I tools\reports\app.py 8790
