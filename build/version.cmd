@echo off
setlocal

set "repo_root=%~1"
if "%repo_root%"=="" set "repo_root=."

where pwsh >nul 2>nul
if %errorlevel%==0 (
    pwsh -NoProfile -ExecutionPolicy Bypass -File "%~dp0version.ps1" -RepoRoot "%repo_root%"
) else (
    powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0version.ps1" -RepoRoot "%repo_root%"
)

exit /b %errorlevel%
