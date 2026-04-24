@echo off
setlocal EnableExtensions EnableDelayedExpansion
chcp 65001 >nul

REM ============================================================
REM  Delete GitHub repository script V2
REM  This script does NOT touch your local project files.
REM  It uses GitHub CLI: gh repo delete
REM ============================================================

set "OWNER_REPO=alex-merrcer/thermal_stm32"

echo.
echo [WARNING] This will delete the GitHub repository:
echo   %OWNER_REPO%
echo.
echo This operation is destructive on GitHub.
echo Your local project files will NOT be modified by this script.
echo.

where gh >nul 2>nul
if errorlevel 1 (
    echo [ERROR] GitHub CLI "gh" was not found.
    echo Install GitHub CLI first, or delete the repo from GitHub web UI:
    echo   Repository page ^> Settings ^> General ^> Danger Zone ^> Delete this repository
    pause
    exit /b 1
)

gh auth status >nul 2>nul
if errorlevel 1 (
    echo [ERROR] GitHub CLI is not logged in.
    echo Run:
    echo   gh auth login
    echo Then run this script again.
    pause
    exit /b 1
)

echo To confirm deletion, type exactly:
echo   DELETE %OWNER_REPO%
echo.
set /p CONFIRM=Confirm: 

if not "%CONFIRM%"=="DELETE %OWNER_REPO%" (
    echo [CANCELLED] Repository was not deleted.
    pause
    exit /b 1
)

gh repo delete "%OWNER_REPO%" --yes
if errorlevel 1 (
    echo.
    echo [ERROR] Delete failed. You may need delete_repo permission in GitHub CLI.
    echo Try:
    echo   gh auth refresh -h github.com -s delete_repo
    pause
    exit /b 1
)

echo.
echo [OK] GitHub repository deleted:
echo   %OWNER_REPO%
echo.
echo Next step:
echo   1. Recreate an EMPTY GitHub repo named thermal_stm32.
echo   2. Run github_first_upload_no_conflict_v2.cmd.
pause
exit /b 0
