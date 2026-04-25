@echo off
setlocal EnableExtensions EnableDelayedExpansion
chcp 65001 >nul

REM ============================================================
REM  Clear all files in a GitHub repository, but KEEP the repo.
REM
REM  This script DOES NOT delete the GitHub repository itself.
REM  This script DOES NOT touch your local firmware project files.
REM
REM  It creates a temporary clone, runs `git rm -r .`, commits the
REM  deletion, and pushes that deletion commit to the default branch.
REM
REM  Default target repo:
REM    alex-merrcer/thermal_firmware_full
REM
REM  If your repo name is different, edit OWNER_REPO below.
REM ============================================================

set "OWNER_REPO=alex-merrcer/thermal_firmware_full"
set "REMOTE_URL=https://github.com/%OWNER_REPO%.git"
set "COMMIT_MSG=clear: remove all repository files"

REM Optional: set a branch manually. Leave empty to auto-detect.
set "TARGET_BRANCH="

echo.
echo [WARNING] This will remove ALL TRACKED FILES from GitHub repository:
echo   %OWNER_REPO%
echo.
echo The GitHub repository itself will be KEPT.
echo Your local firmware source folder will NOT be modified.
echo.
echo A deletion commit will be pushed to the repository branch.
echo.

where git >nul 2>nul
if errorlevel 1 (
    echo [ERROR] Git was not found. Please install Git first.
    pause
    exit /b 1
)

REM Detect default branch if TARGET_BRANCH is empty.
if "%TARGET_BRANCH%"=="" (
    echo [INFO] Detecting default branch...
    for /f "tokens=2" %%A in ('git ls-remote --symref "%REMOTE_URL%" HEAD 2^>nul ^| findstr /b "ref:"') do (
        set "REF=%%A"
        set "TARGET_BRANCH=!REF:refs/heads/=!"
    )
)

if "%TARGET_BRANCH%"=="" (
    set "TARGET_BRANCH=main"
)

echo [INFO] Target branch:
echo   %TARGET_BRANCH%
echo.

echo To confirm clearing repository files, type exactly:
echo   CLEAR %OWNER_REPO%
echo.
set /p CONFIRM=Confirm: 

if not "%CONFIRM%"=="CLEAR %OWNER_REPO%" (
    echo [CANCELLED] Repository files were not removed.
    pause
    exit /b 1
)

set "WORKDIR=%TEMP%\github_clear_%RANDOM%_%RANDOM%"
if exist "%WORKDIR%" rmdir /s /q "%WORKDIR%" >nul 2>nul

echo.
echo [INFO] Cloning repository to temporary folder:
echo   %WORKDIR%
echo.

git clone --depth 1 --branch "%TARGET_BRANCH%" "%REMOTE_URL%" "%WORKDIR%"
if errorlevel 1 (
    echo.
    echo [ERROR] Clone failed.
    echo Possible causes:
    echo   1. Repository URL is wrong.
    echo   2. You are not logged in to GitHub in Git Credential Manager.
    echo   3. Branch name is wrong.
    echo   4. Network problem.
    echo.
    echo Try opening this URL in browser:
    echo   https://github.com/%OWNER_REPO%
    pause
    exit /b 1
)

cd /d "%WORKDIR%"
if errorlevel 1 (
    echo [ERROR] Cannot enter temp clone folder.
    pause
    exit /b 1
)

echo.
echo [INFO] Removing all tracked files in temp clone...

git rm -r --ignore-unmatch .

REM If there are no staged deletions, repo may already be empty.
git diff --cached --quiet
if not errorlevel 1 (
    echo.
    echo [INFO] No tracked files found. Repository branch already has no files.
    cd /d "%TEMP%"
    rmdir /s /q "%WORKDIR%" >nul 2>nul
    pause
    exit /b 0
)

REM Ensure local commit identity exists for this temp clone.
git config user.name >nul 2>nul
if errorlevel 1 git config user.name "Repo Cleaner"

git config user.email >nul 2>nul
if errorlevel 1 git config user.email "repo-cleaner@example.local"

echo.
echo [INFO] Committing deletion...

git commit -m "%COMMIT_MSG%"
if errorlevel 1 (
    echo.
    echo [ERROR] Commit failed.
    pause
    exit /b 1
)

echo.
echo [INFO] Pushing deletion commit to GitHub...

git push origin "HEAD:%TARGET_BRANCH%"
if errorlevel 1 (
    echo.
    echo [ERROR] Push failed.
    echo Possible causes:
    echo   1. You do not have write permission.
    echo   2. Branch protection blocks direct push.
    echo   3. GitHub authentication failed.
    echo.
    echo Your local firmware source files were NOT modified.
    pause
    exit /b 1
)

echo.
echo [OK] All tracked files were removed from GitHub repository branch:
echo   %OWNER_REPO% / %TARGET_BRANCH%
echo.
echo The GitHub repository still exists.
echo Your local firmware source files were NOT modified.
echo.
echo Next step if you want to upload your current local firmware folder again:
echo   Use your normal update script, or reset local Git to origin/%TARGET_BRANCH% first

echo   cd /d E:\26512VSS\source\repos\IAP\IAPWinForms_phase3\firmware
echo   git fetch origin
echo   git reset --mixed origin/%TARGET_BRANCH%
echo   git add -A
echo   git commit -m "upload: full firmware"
echo   git push origin %TARGET_BRANCH%
echo.

cd /d "%TEMP%"
rmdir /s /q "%WORKDIR%" >nul 2>nul

pause
exit /b 0
