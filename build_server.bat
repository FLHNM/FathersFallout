@echo off
setlocal
title F4MP - Compilar Servidor
cd /d "%~dp0"

echo ============================================
echo   F4MP - Compilando SERVIDOR  [F4MPServer.exe]
echo ============================================
echo.

REM -- Localizar Visual Studio --
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto :no_vs

set "VSPATH="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH goto :no_vc

echo Visual Studio: %VSPATH%
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
set "PATH=%VSPATH%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%VSPATH%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"

where cmake >nul 2>&1
if errorlevel 1 goto :no_cmake

set "VENDORED=%~dp0third_party\win-x64"
if not exist "%VENDORED%\share" goto :no_deps

echo.
echo Configurando el proyecto...
cmake -S server -B server\build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
if errorlevel 1 goto :fail_cfg

echo.
echo Compilando...
cmake --build server\build
if errorlevel 1 goto :fail_build

if exist "%VENDORED%\bin" copy /Y "%VENDORED%\bin\*.dll" "server\build\" >nul 2>&1

echo.
echo ============================================
echo   [OK] Servidor compilado.
echo   Salida: server\build\F4MPServer.exe
echo ============================================
goto :end

:no_vs
echo [ERROR] No encuentro Visual Studio. Instalalo con la carga "Desarrollo para el escritorio con C++".
goto :end
:no_vc
echo [ERROR] Visual Studio no tiene las herramientas de C++ x64.
goto :end
:no_cmake
echo [ERROR] No encuentro CMake. Instala en Visual Studio el componente "Herramientas de CMake para C++".
goto :end
:no_deps
echo [ERROR] Faltan las dependencias en third_party\deps. Descargalas o ejecuta setup_deps.bat.
goto :end
:fail_cfg
echo [ERROR] Fallo la configuracion de CMake. Revisa el texto de arriba.
goto :end
:fail_build
echo [ERROR] Fallo la compilacion. Revisa el texto de arriba.
goto :end

:end
echo.
pause
endlocal
