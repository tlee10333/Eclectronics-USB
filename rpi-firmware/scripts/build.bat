@echo off
REM scripts\build.bat — west build targets
REM Usage: scripts\build.bat <command>
REM   build             incremental build (re-uses CMake cache)
REM   build-fresh       pristine build (wipes cache)
REM   build-wifi-probe  pristine build with debug_wifi_probe.conf
REM   menuconfig        open Kconfig menu for current build
REM   clean             delete BUILD_DIR
REM   clean-all         delete all build directories

call "%~dp0config.bat"

if "%1"==""                 goto help
if "%1"=="build"            goto build
if "%1"=="build-fresh"      goto build-fresh
if "%1"=="build-wifi-probe" goto build-wifi-probe
if "%1"=="menuconfig"       goto menuconfig
if "%1"=="clean"            goto clean
if "%1"=="clean-all"        goto clean-all

echo Unknown build command: %1
goto help

REM ─────────────────────────────────────────────────────────────────────────────

REM Build the optional -DEXTRA_CONF_FILE argument.
REM Because CMD does not support inline conditionals in a command line, we
REM set _EXTRA in a subroutine and use GOTO to skip the empty-string branch.
:_set-extra
set _EXTRA=
if not "%EXTRA_CONF%"=="" set _EXTRA=-DEXTRA_CONF_FILE=%CD:\=/%/%EXTRA_CONF%
goto :eof

:build
call :_set-extra
echo Incremental build for %BOARD% -^> %BUILD_DIR%\
set ZEPHYR_BASE=%ZEPHYR_BASE%
%WEST_CMD% build ^
    -b "%BOARD%" ^
    -d "%BUILD_DIR%" ^
    "%CD%" ^
    -- ^
    -DDTC_OVERLAY_FILE=%OVERLAY_FILE% ^
    "-DSHIELD=%SHIELD%" ^
    "-DBOARD_ROOT=%BOARD_ROOT_CMAKE%" ^
    %_EXTRA%
goto end

:build-fresh
call :_set-extra
echo Pristine build for %BOARD% -^> %BUILD_DIR%\
set ZEPHYR_BASE=%ZEPHYR_BASE%
%WEST_CMD% build ^
    -p always ^
    -b "%BOARD%" ^
    -d "%BUILD_DIR%" ^
    "%CD%" ^
    -- ^
    -DDTC_OVERLAY_FILE=%OVERLAY_FILE% ^
    "-DSHIELD=%SHIELD%" ^
    "-DBOARD_ROOT=%BOARD_ROOT_CMAKE%" ^
    %_EXTRA%
goto end

:build-wifi-probe
echo Pristine wifi-probe build -^> build-wifi-probe\
set ZEPHYR_BASE=%ZEPHYR_BASE%
%WEST_CMD% build ^
    -p always ^
    -b "%BOARD%" ^
    -d build-wifi-probe ^
    "%CD%" ^
    -- ^
    -DDTC_OVERLAY_FILE=%OVERLAY_FILE% ^
    "-DSHIELD=%SHIELD%" ^
    "-DBOARD_ROOT=%BOARD_ROOT_CMAKE%" ^
    -DEXTRA_CONF_FILE=%CD:\=/%/debug_wifi_probe.conf
goto end

:menuconfig
if not exist "%BUILD_DIR%\build.ninja" (
    echo No build found in %BUILD_DIR% - run: fw build
    exit /b 1
)
set ZEPHYR_BASE=%ZEPHYR_BASE%
%WEST_CMD% build -t menuconfig -d "%BUILD_DIR%"
goto end

:clean
echo Removing %BUILD_DIR%\ ...
if exist "%BUILD_DIR%" rmdir /s /q "%BUILD_DIR%"
goto end

:clean-all
echo Removing all build directories ...
if exist build            rmdir /s /q build
if exist build-wifi-probe rmdir /s /q build-wifi-probe
goto end

:help
echo.
echo Usage: fw build ^| build-fresh ^| build-wifi-probe ^| menuconfig ^| clean ^| clean-all
echo.
echo Override variables before calling (examples):
echo   set EXTRA_CONF=debug_minimal_cdc.conf ^&^& fw build-fresh
echo   set BUILD_DIR=build-test ^&^& fw build
echo.

:end
