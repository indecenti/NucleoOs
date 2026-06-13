@echo off
REM Avvia la GUI di flash NucleoOS (doppio click).
cd /d "%~dp0\.."
python "%~dp0flasher.py"
if errorlevel 1 pause
