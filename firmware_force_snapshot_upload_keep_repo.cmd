@echo off
setlocal EnableExtensions EnableDelayedExpansion

REM ============================================================
REM  firmware_force_snapshot_upload_keep_repo.cmd
REM
REM  Purpose:
REM    Keep the GitHub repository itself, but FORCE REPLACE the
REM    remote main branch with a fresh snapshot of the local
REM    firmware folder.
REM
REM  Important:
REM    - This script does NOT modify your local source files.
REM    - This script does NOT delete your local .git folder.
REM    - This script rewrites remote GitHub main history.
REM    - Use only when GitHub is just a backup and may be overwritten.
REM ============================================================

set "PROJECT_PATH=E:\26512VSS\source\repos\IAP\IAPWinForms_phase3\firmware"
set "REMOTE_URL=https://github.com/alex-merrcer/thermal_firmware_full.git"
set "BRANCH=main"
set "REPO_NAME=alex-merrcer/thermal_firmware_full"

echo.
echo [WARNING] This will FORCE REPLACE the GitHub branch:
echo   %REPO_NAME% / %BRANCH%
echo.
echo The GitHub repository will be kept, but remote files/history on main
echo will be replaced by a fresh snapshot of:
echo   %PROJECT_PATH%
echo.
echo Your local source files will NOT be modified.
echo Your local .git folder will NOT be deleted.
echo.
echo To continue, type exactly:
echo   FORCE %REPO_NAME%
echo.

set /p CONFIRM="Confirm: "
if not "%CONFIRM%"=="FORCE %REPO_NAME%" (
    echo.
    echo [CANCELLED] Confirmation text did not match.
    pause
    exit /b 1
)

if not exist "%PROJECT_PATH%" (
    echo.
    echo [ERROR] Project path does not exist:
    echo   %PROJECT_PATH%
    pause
    exit /b 1
)

where git >nul 2>nul
if errorlevel 1 (
    echo.
    echo [ERROR] Git was not found. Please install Git and try again.
    pause
    exit /b 1
)

set "STAMP=%DATE:/=-%_%TIME::=-%"
set "STAMP=%STAMP: =0%"
set "STAGE_DIR=%TEMP%\firmware_snapshot_upload_%RANDOM%%RANDOM%"

echo.
echo [INFO] Creating clean temporary snapshot folder:
echo   %STAGE_DIR%

if exist "%STAGE_DIR%" rmdir /s /q "%STAGE_DIR%"
mkdir "%STAGE_DIR%"
if errorlevel 1 (
    echo [ERROR] Failed to create temp folder.
    pause
    exit /b 1
)

echo.
echo [INFO] Copying project snapshot...
echo [INFO] Excluding local Git metadata, build outputs, node modules, and private local config.

robocopy "%PROJECT_PATH%" "%STAGE_DIR%" /E ^
  /XD ".git" ".vs" ".vscode" "OBJ" "Objects" "Listings" "node_modules" "miniprogram_npm" ^
  /XF "project.private.config.json" "*.uvguix.*" "*.bak" "*.tmp" "*.log" "*.lnk" "*.dep" "*.lnp" "*.build_log.htm" >nul

set "ROBO_RC=%ERRORLEVEL%"
if %ROBO_RC% GEQ 8 (
    echo.
    echo [ERROR] Robocopy failed with exit code %ROBO_RC%.
    echo [INFO] Temp folder kept for inspection:
    echo   %STAGE_DIR%
    pause
    exit /b 1
)

echo.
echo [INFO] Snapshot copy completed.

echo.
echo [SAFE CHECK] Looking for common secret keywords in snapshot...
echo [INFO] This is a simple warning scan. Review results carefully.

findstr /S /I /N ^
  /C:"AccessKeySecret" ^
  /C:"accessKeySecret" ^
  /C:"DeviceSecret" ^
  /C:"deviceSecret" ^
  /C:"ALIYUN_IOT_DEVICE_SECRET" ^
  /C:"SECRET_KEY" ^
  /C:"PASSWORD" ^
  "%STAGE_DIR%\*.c" "%STAGE_DIR%\*.h" "%STAGE_DIR%\*.js" "%STAGE_DIR%\*.json" "%STAGE_DIR%\*.env" 2>nul

echo.
echo If real secrets were listed above, press Ctrl+C now.
echo Otherwise type PUSH to continue.
set /p PUSH_CONFIRM="Continue: "
if /I not "%PUSH_CONFIRM%"=="PUSH" (
    echo.
    echo [CANCELLED] Push not confirmed.
    echo [INFO] Temp folder kept:
    echo   %STAGE_DIR%
    pause
    exit /b 1
)

cd /d "%STAGE_DIR%"
if errorlevel 1 (
    echo [ERROR] Failed to enter temp folder.
    pause
    exit /b 1
)

echo.
echo [INFO] Initializing temporary Git repository...
git init
if errorlevel 1 goto :git_fail

git checkout -b %BRANCH%
if errorlevel 1 goto :git_fail

git config user.name >nul 2>nul
if errorlevel 1 git config user.name "snapshot-upload"

git config user.email >nul 2>nul
if errorlevel 1 git config user.email "snapshot-upload@example.invalid"

git remote add origin "%REMOTE_URL%"
if errorlevel 1 goto :git_fail

echo.
echo [INFO] Staging files...
git add -A
if errorlevel 1 goto :git_fail

echo.
echo [INFO] Files to upload:
git status --short

echo.
git diff --cached --quiet
if not errorlevel 1 (
    echo [ERROR] No files were staged. Snapshot appears empty.
    echo [INFO] Temp folder kept:
    echo   %STAGE_DIR%
    pause
    exit /b 1
)

echo.
echo [INFO] Creating commit...
git commit -m "upload: full firmware snapshot %DATE% %TIME%"
if errorlevel 1 goto :git_fail

echo.
echo [WARNING] About to force push and replace remote %BRANCH%.
echo Type UPLOAD to force push.
set /p FINAL_CONFIRM="Final confirm: "
if /I not "%FINAL_CONFIRM%"=="UPLOAD" (
    echo.
    echo [CANCELLED] Final upload not confirmed.
    echo [INFO] Temp folder kept:
    echo   %STAGE_DIR%
    pause
    exit /b 1
)

echo.
echo [INFO] Force pushing snapshot to GitHub...
git push --force origin %BRANCH%
if errorlevel 1 goto :git_fail

echo.
echo [SUCCESS] GitHub main branch has been replaced with this local snapshot.
echo [INFO] Repository kept:
echo   %REMOTE_URL%

echo.
echo [INFO] Removing temporary folder...
cd /d "%TEMP%"
rmdir /s /q "%STAGE_DIR%" >nul 2>nul

pause
exit /b 0

:git_fail
echo.
echo [ERROR] Git command failed.
echo [INFO] Your local source files were NOT modified.
echo [INFO] Temp folder kept for inspection:
echo   %STAGE_DIR%
pause
exit /b 1
