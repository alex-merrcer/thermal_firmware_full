@echo off
setlocal EnableExtensions EnableDelayedExpansion
chcp 65001 >nul

REM ============================================================
REM  Full firmware first upload script - NO pull / merge / rebase
REM  It will NOT edit your source files.
REM
REM  Default target repo:
REM    https://github.com/alex-merrcer/thermal_firmware_full.git
REM
REM  If your repo name is different, edit REPO_URL below.
REM ============================================================

set "PROJECT_PATH=E:\26512VSS\source\repos\IAP\IAPWinForms_phase3\firmware"
set "REPO_URL=https://github.com/alex-merrcer/thermal_firmware_full.git"
set "BRANCH=main"

echo.
echo [INFO] Full firmware project path:
echo   %PROJECT_PATH%
echo [INFO] Remote:
echo   %REPO_URL%
echo.

where git >nul 2>nul
if errorlevel 1 (
    echo [ERROR] Git is not installed or not in PATH.
    pause
    exit /b 1
)

if not exist "%PROJECT_PATH%\" (
    echo [ERROR] Project path does not exist.
    pause
    exit /b 1
)

cd /d "%PROJECT_PATH%" || (
    echo [ERROR] Failed to enter project path.
    pause
    exit /b 1
)

call :check_conflict_markers || exit /b 1
call :warn_sensitive_names

if exist ".git\rebase-merge" goto git_state_error
if exist ".git\rebase-apply" goto git_state_error
if exist ".git\MERGE_HEAD" goto git_state_error

if not exist ".git\" (
    echo [INFO] .git not found. Initializing local Git repository at firmware root...
    git init
    if errorlevel 1 goto git_error
) else (
    echo [INFO] Existing .git found at firmware root. This script will not delete it.
)

git branch -M "%BRANCH%"
if errorlevel 1 goto git_error

git remote remove origin >nul 2>nul
git remote add origin "%REPO_URL%"
if errorlevel 1 goto git_error

call :install_local_excludes || exit /b 1

echo.
echo [CHECK] Checking whether remote branch "%BRANCH%" already exists...
git ls-remote --exit-code --heads origin "%BRANCH%" >nul 2>nul
if not errorlevel 1 (
    echo [ERROR] Remote branch "%BRANCH%" already exists on GitHub.
    echo.
    echo This first-upload script refuses to pull/rebase/merge or overwrite remote code,
    echo because those actions can create conflict markers inside your files.
    echo.
    echo Choose one:
    echo   1. Delete/recreate the GitHub repo as EMPTY, then run this script.
    echo   2. Use the update script only if this local firmware folder already matches remote history.
    echo.
    pause
    exit /b 1
)

echo.
echo [INFO] Adding files according to Git rules and local excludes...
git add -A
if errorlevel 1 goto git_error

echo.
echo [INFO] Staged files preview:
git status --short

echo.
echo [INFO] Staged sensitive-looking files check:
git diff --cached --name-only | findstr /I /R "\.pem$ \.key$ \.p12$ \.pfx$ ^\.env$ secrets\.h$ wifi_config_private\.h$ ota_private\.h$"
if not errorlevel 1 (
    echo.
    echo [ERROR] Sensitive-looking file is staged. Remove it before upload.
    echo You can unstage with:
    echo   git reset HEAD -- path\to\file
    pause
    exit /b 1
)

echo.
echo [CONFIRM] This will create a commit and push the entire firmware folder to GitHub.
echo It will NOT run git pull, git merge, git rebase, or edit source files.
set /p CONFIRM=Type YES to continue: 
if /I not "%CONFIRM%"=="YES" (
    echo [CANCELLED] No commit or push was made.
    pause
    exit /b 1
)

git diff --cached --quiet
if errorlevel 1 (
    git commit -m "first upload: full STM32 ESP32 firmware"
    if errorlevel 1 goto git_error
) else (
    git rev-parse --verify HEAD >nul 2>nul
    if errorlevel 1 (
        echo [ERROR] Nothing is staged and there is no existing commit.
        pause
        exit /b 1
    )
    echo [INFO] Nothing new to commit. Existing HEAD will be pushed.
)

git push -u origin "%BRANCH%"
if errorlevel 1 (
    echo.
    echo [ERROR] Push failed. No source files were modified by this script.
    pause
    exit /b 1
)

echo.
echo [OK] Full firmware first upload completed.
pause
exit /b 0

:git_state_error
echo [ERROR] Git is currently in merge/rebase state.
echo This script will not continue, to avoid writing conflict markers into code.
echo Resolve or abort the Git operation manually first:
echo   git rebase --abort
echo or:
echo   git merge --abort
pause
exit /b 1

:git_error
echo.
echo [ERROR] Git command failed. No pull/merge/rebase was performed.
pause
exit /b 1


:check_conflict_markers
set "SCAN_FILE=%TEMP%\redpic1_full_conflict_scan.txt"
del "%SCAN_FILE%" >nul 2>nul

REM Only match REAL Git conflict marker lines:
REM   <<<<<<< HEAD
REM   =======
REM   >>>>>>> commit-id
REM Do NOT match comment separators such as /* ======================== */
findstr /S /N /R /C:"^<<<<<<<" /C:"^=======$" /C:"^>>>>>>>" *.c *.h *.cpp *.hpp *.s *.asm *.py *.txt *.md *.cmake CMakeLists.txt > "%SCAN_FILE%" 2>nul

if exist "%SCAN_FILE%" (
    for %%A in ("%SCAN_FILE%") do if %%~zA GTR 0 (
        echo [ERROR] Real Git conflict markers were found:
        type "%SCAN_FILE%"
        echo.
        echo Fix these files first. This script will not upload broken files.
        pause
        exit /b 1
    )
)
exit /b 0

:install_local_excludes
if not exist ".git\info\" mkdir ".git\info" >nul 2>nul
if not exist ".git\info\exclude" type nul > ".git\info\exclude"

findstr /C:"# BEGIN RedPic1 full firmware local excludes" ".git\info\exclude" >nul 2>nul
if errorlevel 1 (
    echo [INFO] Installing local ignore rules into .git\info\exclude
    >> ".git\info\exclude" echo.
    >> ".git\info\exclude" echo # BEGIN RedPic1 full firmware local excludes
    >> ".git\info\exclude" echo .git_backup_*/
    >> ".git\info\exclude" echo .git_backup_*
    >> ".git\info\exclude" echo .vs/
    >> ".git\info\exclude" echo .vscode/
    >> ".git\info\exclude" echo Objects/
    >> ".git\info\exclude" echo Listings/
    >> ".git\info\exclude" echo Debug/
    >> ".git\info\exclude" echo Release/
    >> ".git\info\exclude" echo build/
    >> ".git\info\exclude" echo out/
    >> ".git\info\exclude" echo dist/
    >> ".git\info\exclude" echo __pycache__/
    >> ".git\info\exclude" echo .pytest_cache/
    >> ".git\info\exclude" echo .pio/
    >> ".git\info\exclude" echo managed_components/
    >> ".git\info\exclude" echo sdkconfig.old
    >> ".git\info\exclude" echo dependencies.lock
    >> ".git\info\exclude" echo *.o
    >> ".git\info\exclude" echo *.obj
    >> ".git\info\exclude" echo *.d
    >> ".git\info\exclude" echo *.crf
    >> ".git\info\exclude" echo *.lst
    >> ".git\info\exclude" echo *.map
    >> ".git\info\exclude" echo *.axf
    >> ".git\info\exclude" echo *.elf
    >> ".git\info\exclude" echo *.hex
    >> ".git\info\exclude" echo *.bin
    >> ".git\info\exclude" echo *.tmp
    >> ".git\info\exclude" echo *.bak
    >> ".git\info\exclude" echo *.log
    >> ".git\info\exclude" echo *.pyc
    >> ".git\info\exclude" echo *.uvguix.*
    >> ".git\info\exclude" echo *.uvopt
    >> ".git\info\exclude" echo *.uvoptx
    >> ".git\info\exclude" echo .env
    >> ".git\info\exclude" echo *.pem
    >> ".git\info\exclude" echo *.key
    >> ".git\info\exclude" echo *.p12
    >> ".git\info\exclude" echo *.pfx
    >> ".git\info\exclude" echo secrets.h
    >> ".git\info\exclude" echo wifi_config_private.h
    >> ".git\info\exclude" echo ota_private.h
    >> ".git\info\exclude" echo # END RedPic1 full firmware local excludes
) else (
    echo [INFO] Local excludes already installed.
)
exit /b 0

:warn_sensitive_names
set "SENSITIVE_FILE=%TEMP%\redpic1_full_sensitive_scan.txt"
del "%SENSITIVE_FILE%" >nul 2>nul

dir /S /B *.pem *.key *.p12 *.pfx .env secrets.h wifi_config_private.h ota_private.h > "%SENSITIVE_FILE%" 2>nul

if exist "%SENSITIVE_FILE%" (
    for %%A in ("%SENSITIVE_FILE%") do if %%~zA GTR 0 (
        echo.
        echo [WARNING] Sensitive-looking files exist under firmware:
        type "%SENSITIVE_FILE%"
        echo.
        echo These are ignored by local excludes when untracked, but if already tracked, Git can still include them.
        echo Please confirm they are not staged before uploading.
    )
)
exit /b 0

