@echo off
rem One shot local demo: start the fake upstream in its own window, then run the
rem gateway in this window. Press Ctrl-C in this window to stop the gateway, then
rem close the "mock upstream" window.
setlocal
set "ROOT=%~dp0"
set "BIN=%ROOT%build\bin"

if not exist "%BIN%\aegis.exe" (
    echo [run] %BIN%\aegis.exe not found.
    echo [run] build first:  cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release ^&^& cmake --build build
    exit /b 1
)

echo [run] starting mock upstream on port 9001 ...
start "aegis mock upstream" "%BIN%\mock_upstream.exe" --port 9001 --tokens 5 --delay-ms 10
timeout /t 1 /nobreak >nul

echo [run] starting gateway on http://127.0.0.1:8080  (Ctrl-C to stop)
echo [run] health: curl -s http://127.0.0.1:8080/healthz
"%BIN%\aegis.exe" --config "%ROOT%config\local.json"
