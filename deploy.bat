@echo off
rem Create a self contained dist\ folder: binaries + runtime DLLs + config.
rem Usage: deploy.bat [source-bin-dir]   (default: build\bin)
setlocal
set "ROOT=%~dp0"
set "SRC=%~1"
if "%SRC%"=="" set "SRC=%ROOT%build\bin"
set "DST=%ROOT%dist"

if not exist "%SRC%\aegis.exe" (
    echo [deploy] %SRC%\aegis.exe not found, build first.
    exit /b 1
)

if exist "%DST%" rmdir /s /q "%DST%"
mkdir "%DST%"

copy /y "%SRC%\aegis.exe" "%DST%" >nul
copy /y "%SRC%\mock_upstream.exe" "%DST%" >nul
copy /y "%SRC%\*.dll" "%DST%" >nul
xcopy "%ROOT%config" "%DST%\config" /E /I /Q >nul

echo [deploy] dist ready: aegis.exe, mock_upstream.exe, runtime DLLs, config\
echo [deploy] run it with:  dist\aegis.exe --config dist\config\local.json
