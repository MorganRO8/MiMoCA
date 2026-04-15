@echo off
setlocal
set SCRIPT_DIR=%~dp0
set INSTALL_ROOT=%SCRIPT_DIR%..
set LOG_ROOT=%LOCALAPPDATA%\MiMoCA\logs
set SETUP_LOG=%LOG_ROOT%\first_launch_setup.log

if not exist "%LOG_ROOT%" mkdir "%LOG_ROOT%" >nul 2>nul
powershell -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT_DIR%first_launch_setup.ps1" -InstallRoot "%INSTALL_ROOT%" >"%SETUP_LOG%" 2>&1
if errorlevel 1 (
    echo MiMoCA first-launch setup failed. See "%SETUP_LOG%" for details.
    exit /b 1
)

set MIMOCA_MODEL_CACHE_ROOT=%LOCALAPPDATA%\MiMoCA\model_cache
start "" "%INSTALL_ROOT%\bin\mimoca.exe"
endlocal
