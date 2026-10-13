@echo off
REM Runs idf.py with the ESP-IDF 5.5.3 environment active.
REM
REM The machine default python is 3.14 but the installed IDF virtual environment
REM is py3.11, so the stock export.bat cannot find its own env. Pin it first.
REM
REM   idf.bat build
REM   idf.bat -p COM13 flash monitor
REM   idf.bat menuconfig

set "IDF_PATH=C:\Espressif\frameworks\esp-idf-v5.5.3"
set "IDF_TOOLS_PATH=C:\Espressif"
set "IDF_PYTHON_ENV_PATH=C:\Espressif\python_env\idf5.5_py3.11_env"
set "PATH=%IDF_PYTHON_ENV_PATH%\Scripts;%PATH%"

call "%IDF_PATH%\export.bat" >nul 2>&1

if "%~1"=="" (
    set "IDF_ARGS=build"
) else (
    set "IDF_ARGS=%*"
)

python "%IDF_PATH%\tools\idf.py" %IDF_ARGS%
exit /b %ERRORLEVEL%
