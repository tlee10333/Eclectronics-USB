@echo off
REM scripts\flash.bat — flash firmware to the Raspberry Pi Pico 2
REM Usage: scripts\flash.bat <command>
REM   flash            copy zephyr.uf2 to the Pico BOOTSEL drive  (default)
REM   flash-picotool   flash via picotool
REM   flash-find-drive scan for the Pico BOOTSEL drive

call "%~dp0config.bat"

set UF2_FILE=%BUILD_DIR%\zephyr\zephyr.uf2

if "%1"==""                goto flash-uf2
if "%1"=="flash"           goto flash-uf2
if "%1"=="flash-uf2"       goto flash-uf2
if "%1"=="flash-picotool"  goto flash-picotool
if "%1"=="flash-find-drive" goto flash-find-drive

echo Unknown flash command: %1
goto help

REM ─────────────────────────────────────────────────────────────────────────────

:flash-uf2
if not exist "%UF2_FILE%" (
    echo UF2 not found: %UF2_FILE%
    echo Run: fw build
    exit /b 1
)
echo Looking for Pico drive with label "%PICO_DRIVE_LABEL%" ...
call :find-pico-drive PICO_DRIVE
if "%PICO_DRIVE%"=="" (
    echo ERROR: Drive "%PICO_DRIVE_LABEL%" not found.
    echo   * Hold BOOTSEL while plugging in the Pico 2.
    echo   * If the label differs, set: set PICO_DRIVE_LABEL=^<label^>
    exit /b 1
)
echo Copying %UF2_FILE% -^> %PICO_DRIVE%\ ...
copy /y "%UF2_FILE%" "%PICO_DRIVE%\"
if errorlevel 1 (
    echo ERROR: copy failed.
    exit /b 1
)
echo Flash complete — Pico will reboot automatically.
goto end

:flash-picotool
if not exist "%UF2_FILE%" (
    echo UF2 not found: %UF2_FILE%
    echo Run: fw build
    exit /b 1
)
echo Flashing via picotool ...
"%PICOTOOL%" load "%UF2_FILE%" --force
"%PICOTOOL%" reboot
goto end

:flash-find-drive
call :find-pico-drive PICO_DRIVE
if not "%PICO_DRIVE%"=="" (
    echo Found: %PICO_DRIVE%  ^(label=%PICO_DRIVE_LABEL%^)
) else (
    echo Not found. Is the Pico in BOOTSEL mode? ^(hold BOOTSEL + plug in^)
)
goto end

:help
echo.
echo Usage: fw flash ^| flash-picotool ^| flash-find-drive
echo.
goto end

REM ── Subroutine: find-pico-drive ───────────────────────────────────────────────
REM Sets the variable named in %1 to the drive letter+colon (e.g. D:),
REM or leaves it empty if not found.
:find-pico-drive
set %1=
for %%D in (A B C D E F G H I J K L M N O P Q R S T U V W X Y Z) do (
    vol %%D: 2>nul | findstr /i /c:"%PICO_DRIVE_LABEL%" >nul 2>&1
    if not errorlevel 1 (
        set %1=%%D:
        goto :eof
    )
)
goto :eof

:end
