@echo off
setlocal
title F4MP - Copiar dependencias al repo

REM Situarse en la carpeta del repo (donde esta este .bat)
cd /d "%~dp0"

set "SRC=%~dp0.deps\vcpkg\installed\x64-windows"
set "DST=%~dp0third_party\win-x64"

echo ==================================================
echo   F4MP - Vendorizar dependencias en el repo
echo ==================================================
echo.
echo   Copia las dependencias ya compiladas a
echo   third_party\deps\  para poder subirlas al repo
echo   y compilar SIN vcpkg.
echo ==================================================
echo.

if not exist "%SRC%\share" (
  echo [ERROR] No encuentro las dependencias compiladas.
  echo         Ejecuta primero  setup_deps.bat
  pause
  exit /b 1
)

echo Copiando... (puede tardar un poco)
robocopy "%SRC%" "%DST%" /E /NFL /NDL /NJH /NJS /NP >nul
if errorlevel 8 (
  echo [ERROR] Fallo la copia.
  pause
  exit /b 1
)

echo.
echo ==================================================
echo   [OK] Dependencias copiadas a:
echo   third_party\win-x64
echo.
echo   Ahora haz commit de la carpeta third_party\ y
echo   ya podras compilar sin vcpkg (los .bat lo detectan
echo   automaticamente).
echo ==================================================
pause
endlocal
