@echo off
setlocal EnableExtensions EnableDelayedExpansion
chcp 65001 >nul

REM ============================================================
REM  firmware_delete_and_recreate_github_repo.cmd
REM
REM  Purpose:
REM    Completely delete the GitHub repository, then recreate an
REM    EMPTY repository with the SAME name.
REM
REM  This script:
REM    - DOES delete the GitHub repository.
REM    - DOES recreate an empty GitHub repository with the same name.
REM    - DOES NOT modify your local firmware files.
REM    - DOES NOT delete your local .git folder.
REM
REM  Requirements:
REM    - GitHub CLI: gh
REM    - Logged in with gh auth login
REM    - delete_repo permission may be required:
REM        gh auth refresh -h github.com -s delete_repo -s repo
REM
REM  After this script succeeds:
REM    Run firmware_first_upload_no_conflict.cmd
REM ============================================================

set "OWNER=alex-merrcer"
set "REPO=thermal_firmware_full"
set "OWNER_REPO=%OWNER%/%REPO%"
set "VISIBILITY=public"
set "DESCRIPTION=Full firmware project for STM32, ESP32, OTA, thermal imaging, and miniprogram"

echo.
echo ============================================================
echo  DELETE AND RECREATE GITHUB REPOSITORY
echo ============================================================
echo.
echo Target repository:
echo   %OWNER_REPO%
echo.
echo Visibility after recreation:
echo   %VISIBILITY%
echo.
echo This operation is destructive on GitHub.
echo It will:
echo   1. Delete the existing GitHub repository if it exists.
echo   2. Recreate an EMPTY repository with the same name.
echo.
echo Your local firmware files will NOT be modified.
echo Your local .git folder will NOT be deleted.
echo.

where gh >nul 2>nul
if errorlevel 1 (
    echo [ERROR] GitHub CLI "gh" was not found.
    echo Install GitHub CLI first:
    echo   https://cli.github.com/
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

echo [INFO] Checking GitHub CLI permissions...
echo If deletion fails later, run:
echo   gh auth refresh -h github.com -s delete_repo -s repo
echo.

echo To confirm DELETE and RECREATE, type exactly:
echo   RECREATE %OWNER_REPO%
echo.
set /p CONFIRM=Confirm: 

if not "%CONFIRM%"=="RECREATE %OWNER_REPO%" (
    echo.
    echo [CANCELLED] Confirmation text did not match.
    pause
    exit /b 1
)

echo.
echo [STEP 1/3] Checking whether repository exists...
gh repo view "%OWNER_REPO%" >nul 2>nul
if errorlevel 1 (
    echo [INFO] Repository does not currently exist on GitHub.
    echo [INFO] Skipping deletion step.
) else (
    echo [STEP 2/3] Deleting existing GitHub repository:
    echo   %OWNER_REPO%
    echo.
    gh repo delete "%OWNER_REPO%" --yes
    if errorlevel 1 (
        echo.
        echo [ERROR] Delete failed.
        echo You may need delete_repo permission:
        echo   gh auth refresh -h github.com -s delete_repo -s repo
        echo.
        echo Or delete manually:
        echo   GitHub repo page ^> Settings ^> General ^> Danger Zone ^> Delete this repository
        pause
        exit /b 1
    )
    echo [OK] Repository deleted.
)

echo.
echo [INFO] Waiting a few seconds before recreation...
timeout /t 5 /nobreak >nul

echo.
echo [STEP 3/3] Creating new empty GitHub repository:
echo   %OWNER_REPO%
echo.

if /I "%VISIBILITY%"=="private" (
    gh repo create "%OWNER_REPO%" --private --description "%DESCRIPTION%"
) else (
    gh repo create "%OWNER_REPO%" --public --description "%DESCRIPTION%"
)

if errorlevel 1 (
    echo.
    echo [WARN] First create attempt failed. Waiting and retrying once...
    timeout /t 10 /nobreak >nul

    if /I "%VISIBILITY%"=="private" (
        gh repo create "%OWNER_REPO%" --private --description "%DESCRIPTION%"
    ) else (
        gh repo create "%OWNER_REPO%" --public --description "%DESCRIPTION%"
    )

    if errorlevel 1 (
        echo.
        echo [ERROR] Recreate failed.
        echo Possible reasons:
        echo   1. GitHub has not released the repo name yet. Wait 1 minute and retry.
        echo   2. Your gh account does not have permission to create this repo.
        echo   3. The repo already exists.
        pause
        exit /b 1
    )
)

echo.
echo [SUCCESS] Repository has been recreated as EMPTY:
echo   https://github.com/%OWNER_REPO%
echo.
echo Next step:
echo   Run firmware_first_upload_no_conflict.cmd
echo.
echo Important:
echo   If GitHub blocks your upload due to secret scanning, remove real secrets
echo   from source files first. Do NOT bypass secret scanning for real keys.
echo.
pause
exit /b 0
