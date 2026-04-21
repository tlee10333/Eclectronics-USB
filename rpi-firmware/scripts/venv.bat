@echo off
REM scripts\venv.bat — venv and west workspace management
REM Usage: scripts\venv.bat <command>
REM   venv-info      print resolved venv / west paths
REM   venv-create    create .venv in WEST_TOPDIR (if absent)
REM   venv-install   install / upgrade west + Zephyr requirements
REM   west-update    run west update in the workspace
REM   west-blobs     fetch nRF Wi-Fi binary blobs

call "%~dp0config.bat"

if "%1"==""            goto help
if "%1"=="venv-info"   goto venv-info
if "%1"=="venv-create" goto venv-create
if "%1"=="venv-install" goto venv-install
if "%1"=="west-update" goto west-update
if "%1"=="west-blobs"  goto west-blobs

echo Unknown venv command: %1
goto help

REM ─────────────────────────────────────────────────────────────────────────────
:venv-info
echo.
echo WEST_TOPDIR : %WEST_TOPDIR%
echo ZEPHYR_BASE : %ZEPHYR_BASE%
echo VENV_DIR    : %VENV_DIR%
echo PYTHON      : %PYTHON%
if exist "%PYTHON%" (
    "%PYTHON%" --version
) else (
    echo   ^(venv not found — run: fw venv-create^)
)
goto end

:venv-create
if exist "%VENV_DIR%\Scripts\activate.bat" (
    echo venv already exists at %VENV_DIR%
    goto end
)
echo Creating venv at %VENV_DIR% ...
python -m venv "%VENV_DIR%"
if errorlevel 1 (
    echo ERROR: venv creation failed. Is Python on PATH?
    exit /b 1
)
echo Done. Run: fw venv-install
goto end

:venv-install
if not exist "%VENV_DIR%\Scripts\activate.bat" (
    echo Venv not found — creating first ...
    call :venv-create-inline
)
echo Upgrading pip ...
"%PYTHON%" -m pip install --upgrade pip
echo Installing west ...
"%PYTHON%" -m pip install west
if exist "%ZEPHYR_BASE%\scripts\requirements.txt" (
    echo Installing Zephyr Python requirements ...
    "%PYTHON%" -m pip install -r "%ZEPHYR_BASE%\scripts\requirements.txt"
) else (
    echo Note: %ZEPHYR_BASE%\scripts\requirements.txt not found, skipping.
)
goto end

:west-update
echo Running west update in %WEST_TOPDIR% ...
pushd "%WEST_TOPDIR%"
%WEST_CMD% update
popd
goto end

:west-blobs
echo Fetching nrf_wifi blobs ...
pushd "%WEST_TOPDIR%"
%WEST_CMD% blobs fetch nrf_wifi
popd
goto end

:venv-create-inline
python -m venv "%VENV_DIR%"
goto :eof

:help
echo.
echo Usage: fw venv-info ^| venv-create ^| venv-install ^| west-update ^| west-blobs
echo.

:end
