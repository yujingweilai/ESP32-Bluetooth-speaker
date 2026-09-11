@echo off
setlocal EnableDelayedExpansion

cd /d "%~dp0"

set "IDF_BASE=D:\ruanjiangjia\ESPIDF\v551\Espressif"
set "IDF_OLD_BASE=D:\espressif\idf551\Espressif"
set "IDF_PYTHON="

if "%IDF_TOOLS_PATH%"=="" (
    if exist "%IDF_BASE%\tools" (
        set "IDF_TOOLS_PATH=%IDF_BASE%\tools"
    )
)

if "%IDF_PYTHON_ENV_PATH%"=="" (
    for /d %%D in ("%IDF_BASE%\python_env\*_env") do (
        if exist "%%~fD\Scripts\python.exe" (
            set "IDF_PYTHON_ENV_PATH=%%~fD"
            goto :idf_python_env_ready
        )
    )
)

if "%IDF_PYTHON_ENV_PATH%"=="" (
    for /d %%D in ("%IDF_OLD_BASE%\python_env\*_env") do (
        if exist "%%~fD\Scripts\python.exe" (
            set "IDF_PYTHON_ENV_PATH=%%~fD"
            goto :idf_python_env_ready
        )
    )
)

:idf_python_env_ready
if not "%IDF_PYTHON_ENV_PATH%"=="" (
    if exist "%IDF_PYTHON_ENV_PATH%\Scripts\python.exe" (
        set "IDF_PYTHON=%IDF_PYTHON_ENV_PATH%\Scripts\python.exe"
    )
)

if "%IDF_PATH%"=="" (
    if exist "%IDF_BASE%\frameworks\esp-idf-v5.5.1\export.bat" (
        set "IDF_PATH=%IDF_BASE%\frameworks\esp-idf-v5.5.1"
    )
)

if "%IDF_PATH%"=="" (
    if exist "%IDF_OLD_BASE%\frameworks\esp-idf-v5.5.1\export.bat" (
        set "IDF_PATH=%IDF_OLD_BASE%\frameworks\esp-idf-v5.5.1"
    )
)

if "%IDF_PATH%"=="" (
    for /d %%D in ("%IDF_BASE%\frameworks\esp-idf*") do (
        if exist "%%~fD\export.bat" (
            set "IDF_PATH=%%~fD"
            goto :idf_path_ready
        )
    )
)

if "%IDF_PATH%"=="" (
    for /d %%D in ("%IDF_OLD_BASE%\frameworks\esp-idf*") do (
        if exist "%%~fD\export.bat" (
            set "IDF_PATH=%%~fD"
            goto :idf_path_ready
        )
    )
)

:idf_path_ready
if "%IDF_PATH%"=="" (
    echo Error: IDF_PATH is not set and no ESP-IDF was found under "%IDF_BASE%".
    pause
    exit /b 1
)

if not exist "%IDF_PATH%\export.bat" (
    echo Error: "%IDF_PATH%\export.bat" not found.
    pause
    exit /b 1
)

if "%IDF_PYTHON%"=="" (
    echo Error: ESP-IDF Python virtual environment not found under "%IDF_BASE%\python_env".
    pause
    exit /b 1
)

call "%IDF_PATH%\export.bat"
if errorlevel 1 (
    echo Error: failed to activate ESP-IDF environment.
    pause
    exit /b 1
)

if not exist ".\build\flash_args" (
    echo Error: flash_args file not found.
    pause
    exit /b 1
)

if not exist ".\version.txt" (
    echo Error: version.txt file not found.
    pause
    exit /b 1
)

set "flash_args="
for /f "usebackq delims=" %%L in (".\build\flash_args") do (
    if not "%%~L"=="" (
        set "flash_args=!flash_args! %%L"
    )
)

if "!flash_args!"=="" (
    echo Error: flash_args content is empty.
    pause
    exit /b 1
)

set /p version=<".\version.txt"

for /f %%i in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd"') do set date=%%i
set "output_filename=vib_player_v%version%_%date%_full_firmware.bin"

pushd build
"%IDF_PYTHON%" "%IDF_PATH%\components\esptool_py\esptool\esptool.py" --chip esp32 merge_bin -o "%output_filename%" !flash_args!
if errorlevel 1 (
    echo Error: merge_bin failed.
    popd
    pause
    exit /b 1
)

echo.
echo merge bin completed, md5:
certutil -hashfile "%output_filename%" MD5

if not exist "..\firmware" mkdir "..\firmware"
copy /Y "%output_filename%" "..\firmware\" >nul
popd

echo.
echo Done.
pause
exit /b 0
