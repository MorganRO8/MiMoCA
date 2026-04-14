@echo off
setlocal
set SCRIPT_DIR=%~dp0
set INSTALL_ROOT=%SCRIPT_DIR%..

powershell -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT_DIR%first_launch_setup.ps1" -InstallRoot "%INSTALL_ROOT%" >nul 2>nul

set MIMOCA_MODEL_CACHE_ROOT=%LOCALAPPDATA%\MiMoCA\model_cache
start "" "%INSTALL_ROOT%\bin\mimoca.exe"
endlocal
