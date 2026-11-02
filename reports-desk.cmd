@echo off
rem The FM4 reports desk: bug reports and shared play.logs from FM4's private reports repo (needs gh, logged in;
rem the repo is REPORTS_REPO in tools\reports\reports.py, or FM4_REPORTS_REPO).
cd /d "%~dp0"
start "" http://127.0.0.1:8790
py -I tools\reports\app.py 8790
