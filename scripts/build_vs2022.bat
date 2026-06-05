@echo off
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build_vs2022.ps1" %*
exit /b %ERRORLEVEL%
