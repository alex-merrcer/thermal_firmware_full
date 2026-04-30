@echo off
setlocal EnableExtensions EnableDelayedExpansion
chcp 65001 >nul

REM ============================================================
REM  RedPic firmware incremental GitHub update script
REM
REM  Purpose:
REM    Real small update / incremental update:
REM      - new files
REM      - deleted files
REM      - renamed files
REM      - modified files
REM
REM  Core command:
REM      git add -A -- .
REM
REM  Important:
REM    This script does NOT pull / merge / rebase automatically.
REM    If remote history is ahead or unrelated, it stops by default.
REM    If your LOCAL project is the only correct source and GitHub is only
REM    a backup, you may run once with:
REM
REM      firmware_incremental_update.cmd "sync local project" --overwrite-remote
REM
REM    After that, normal small updates should work without deleting repo.
REM ============================================================

set "PROJECT_PATH=E:\26512VSS\source\repos\IAP\IAPWinForms_phase3\firmware"
set "REPO_URL=https://github.com/alex-merrcer/thermal_firmware_full.git"
set "BRANCH=main"

set "COMMIT_MSG=%~1"
if "%COMMIT_MSG%"=="" (
    set "COMMIT_MSG=update: firmware incremental %date% %time%"
)

set "ALLOW_OVERWRITE_REMOTE=0"
if /I "%~2"=="--overwrite-remote" (
    set "ALLOW_OVERWRITE_REMOTE=1"
)

echo.
echo [INFO] Project path:
echo   %PROJECT_PATH%
echo [INFO] Branch:
echo   %BRANCH%
echo [INFO] Commit message:
echo   %COMMIT_MSG%
if "%ALLOW_OVERWRITE_REMOTE%"=="1" (
    echo [WARN] overwrite remote mode is ENABLED.
)
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

if not exist ".git\" (
    echo [ERROR] .git not found at firmware root.
    echo.
    echo To support real incremental updates, this folder must remain a Git repo.
    echo Do NOT delete the local .git folder.
    echo If this is a new local folder, run your first-upload script once.
    pause
    exit /b 1
)

git config core.longpaths true >nul 2>nul
git config core.quotepath false >nul 2>nul

call :check_conflict_markers || exit /b 1
call :warn_sensitive_names
call :install_local_excludes || exit /b 1

if exist ".git\rebase-merge" goto git_state_error
if exist ".git\rebase-apply" goto git_state_error
if exist ".git\MERGE_HEAD" goto git_state_error

git rev-parse --verify HEAD >nul 2>nul
if errorlevel 1 (
    echo [ERROR] This repo has no commit yet. Run first upload first.
    pause
    exit /b 1
)

git remote get-url origin >nul 2>nul
if errorlevel 1 (
    echo [INFO] origin remote not found. Adding origin...
    git remote add origin "%REPO_URL%"
    if errorlevel 1 goto git_error
) else (
    git remote set-url origin "%REPO_URL%" >nul 2>nul
)

echo.
echo [INFO] Current working tree changes, including untracked files:
git status --short --untracked-files=all

echo.
echo [INFO] Ignored files preview.
echo If your new source file appears below with '!!', it is ignored and will NOT upload.
git status --ignored --short | findstr /B "!!" 2>nul

echo.
echo [INFO] Staging all add / delete / rename / modify changes...
git add -A -- .
if errorlevel 1 goto git_error

git diff --cached --quiet
if not errorlevel 1 (
    echo.
    echo [OK] No staged changes after git add -A. Nothing to commit.
    echo.
    echo If you expected new files to upload, they may be ignored.
    echo Check:
    echo   git status --ignored --short
    echo To force-add one ignored file:
    echo   git add -f path\to\file
    pause
    exit /b 0
)

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
echo [INFO] Files staged for this incremental commit:
git diff --cached --name-status --find-renames

echo.
echo [CONFIRM] This will create one incremental commit and push it.
echo It supports new files, deleted files and renamed files.
echo It will NOT pull, merge or rebase automatically.
set /p CONFIRM=Type YES to continue: 
if /I not "%CONFIRM%"=="YES" (
    echo [CANCELLED] No commit or push was made.
    pause
    exit /b 1
)

echo.
echo [INFO] Creating local commit...
git commit -m "%COMMIT_MSG%"
if errorlevel 1 goto git_error

echo.
echo [SAFE CHECK] Fetching remote refs only.
git fetch origin "%BRANCH%" >nul 2>nul
if errorlevel 1 (
    echo [WARN] Could not fetch origin/%BRANCH%.
    echo The remote branch may not exist yet. Trying normal push...
    git push -u origin "%BRANCH%"
    if errorlevel 1 goto push_error
    goto push_ok
)

git show-ref --verify --quiet "refs/remotes/origin/%BRANCH%"
if errorlevel 1 (
    echo [INFO] origin/%BRANCH% does not exist. Pushing first branch...
    git push -u origin "%BRANCH%"
    if errorlevel 1 goto push_error
    goto push_ok
)

git merge-base --is-ancestor "origin/%BRANCH%" HEAD
if not errorlevel 1 (
    echo [INFO] Remote is ancestor of local HEAD. Safe fast-forward push.
    git push -u origin "%BRANCH%"
    if errorlevel 1 goto push_error
    goto push_ok
)

echo.
echo [STOP] Remote origin/%BRANCH% has commits/history that are not in local HEAD.
echo This is why real small updates fail: local and GitHub histories are not aligned.
echo.
echo This script will not pull/merge/rebase automatically to avoid conflict markers.
echo.
echo Choices:
echo   1. If LOCAL firmware is the only correct version and GitHub is only backup:
echo      run once:
echo        firmware_incremental_update.cmd "sync local project" --overwrite-remote
echo.
echo   2. If GitHub changes also matter:
echo      use a separate clean clone, merge manually, then push.
echo.

if "%ALLOW_OVERWRITE_REMOTE%"=="1" (
    echo [WARN] You enabled overwrite remote mode.
    set /p FORCE_CONFIRM=Type OVERWRITE_REMOTE to force update GitHub from local HEAD: 
    if /I not "!FORCE_CONFIRM!"=="OVERWRITE_REMOTE" (
        echo [CANCELLED] Remote was not overwritten.
        pause
        exit /b 1
    )

    echo.
    echo [INFO] Force-with-lease push: local HEAD will become GitHub main.
    git push --force-with-lease -u origin "%BRANCH%"
    if errorlevel 1 goto push_error
    goto push_ok
)

pause
exit /b 1

:push_ok
echo.
echo [OK] Incremental firmware update pushed successfully.
echo GitHub now contains this small commit only, not a full repo recreation.
pause
exit /b 0

:push_error
echo.
echo [ERROR] Push failed.
echo No pull/merge/rebase was performed.
echo If Git says non-fast-forward, local and remote histories differ.
pause
exit /b 1

:git_state_error
echo [ERROR] Git is currently in merge/rebase state.
echo Resolve or abort the Git operation manually first:
echo   git rebase --abort
echo or:
echo   git merge --abort
pause
exit /b 1

:git_error
echo.
echo [ERROR] Git command failed.
pause
exit /b 1

:check_conflict_markers
set "SCAN_FILE=%TEMP%\redpic_incremental_conflict_scan.txt"
del "%SCAN_FILE%" >nul 2>nul

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

findstr /C:"# BEGIN RedPic incremental local excludes" ".git\info\exclude" >nul 2>nul
if errorlevel 1 (
    echo [INFO] Installing local ignore rules into .git\info\exclude
    >> ".git\info\exclude" echo.
    >> ".git\info\exclude" echo # BEGIN RedPic incremental local excludes
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
    >> ".git\info\exclude" echo # END RedPic incremental local excludes
) else (
    echo [INFO] Local excludes already installed.
)
exit /b 0

:warn_sensitive_names
set "SENSITIVE_FILE=%TEMP%\redpic_incremental_sensitive_scan.txt"
del "%SENSITIVE_FILE%" >nul 2>nul

dir /S /B *.pem *.key *.p12 *.pfx .env secrets.h wifi_config_private.h ota_private.h > "%SENSITIVE_FILE%" 2>nul

if exist "%SENSITIVE_FILE%" (
    for %%A in ("%SENSITIVE_FILE%") do if %%~zA GTR 0 (
        echo.
        echo [WARNING] Sensitive-looking files exist under firmware:
        type "%SENSITIVE_FILE%"
        echo.
        echo These are ignored when untracked, but if already tracked, Git can still include them.
    )
)
exit /b 0
