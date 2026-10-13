@echo off
setlocal
title F4MP - Instalar dependencias (una sola vez)

REM Situarse en la carpeta del repo (donde esta este .bat)
cd /d "%~dp0"

set "DEPS_DIR=%~dp0.deps\vcpkg"

echo ==================================================
echo   F4MP - Instalacion de dependencias
echo ==================================================
echo.
echo   Esto compila GameNetworkingSockets y spdlog.
echo   Solo hay que hacerlo UNA vez. La primera vez
echo   tarda bastante (10-20 min). Ten paciencia.
echo.
echo   Requisito: tener 'git' instalado.
echo ==================================================
echo.

REM --- 1. Preparar el gestor de dependencias dentro del proyecto (.deps) ---
if not exist "%DEPS_DIR%\vcpkg.exe" (
  if not exist "%DEPS_DIR%\.git" (
    echo [1/3] Descargando el gestor de dependencias...
    git clone https://github.com/microsoft/vcpkg "%DEPS_DIR%"
    if errorlevel 1 (
      echo [ERROR] Fallo la descarga. Comprueba que tienes git instalado y conexion.
      pause
      exit /b 1
    )
  )
  echo [2/3] Preparando el gestor...
  call "%DEPS_DIR%\bootstrap-vcpkg.bat" -disableMetrics
  if errorlevel 1 (
    echo [ERROR] Fallo la preparacion del gestor.
    pause
    exit /b 1
  )
) else (
  echo [1-2/3] Gestor ya preparado.
)
echo.

REM --- 2. Compilar/instalar las dependencias ---
echo [3/3] Compilando dependencias (esto es lo que tarda)...
"%DEPS_DIR%\vcpkg.exe" install gamenetworkingsockets:x64-windows spdlog:x64-windows
if errorlevel 1 (
  echo [ERROR] Fallo al compilar las dependencias.
  pause
  exit /b 1
)
echo.

echo ==================================================
echo   [OK] Dependencias listas.
echo   Ubicacion: .deps\vcpkg\installed\x64-windows
echo.
echo   Ahora ya puedes ejecutar:
echo       build_client.bat
echo       build_server.bat
echo ==================================================
pause
endlocal
