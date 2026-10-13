@echo off
setlocal enabledelayedexpansion
title F4MP - Compilar Cliente (client-ng)
cd /d "%~dp0"

echo ============================================
echo   F4MP - Compilando CLIENTE  [client-ng / plugin F4SE]
echo ============================================
echo.

REM -- Localizar Visual Studio (xmake usa el compilador MSVC) --
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto :no_vs

set "VSPATH="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH goto :no_vc

echo Visual Studio: %VSPATH%
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul

REM -- Comprobar xmake --
where xmake >nul 2>&1
if errorlevel 1 goto :no_xmake

REM -- Localizar Fallout 4 automaticamente --
call :find_game
if not defined GAMEDIR goto :no_game
echo Fallout 4:     %GAMEDIR%
set "XSE_FO4_GAME_PATH=%GAMEDIR%"

cd client-ng

echo.
echo Configurando (modo releasedbg)...
xmake f -m releasedbg -y
if errorlevel 1 goto :fail_build

echo.
echo Compilando... (la primera vez baja y compila CommonLibF4 + GNS, tarda)
xmake -y
if errorlevel 1 goto :fail_build

set "DEPLOYED=NO"
if exist "%GAMEDIR%\Data\F4SE\Plugins\F4MPClient.dll" set "DEPLOYED=SI"

echo.
echo ============================================
echo   [OK] Cliente (client-ng) compilado.
echo   Fallout 4: %GAMEDIR%
echo   Desplegado en Data\F4SE\Plugins: %DEPLOYED%
echo ============================================
goto :end


REM =====================================================================
REM  Busca la carpeta de Fallout 4 y deja la ruta en la variable GAMEDIR
REM =====================================================================
:find_game
set "GAMEDIR="

REM 1) Si ya hay una variable de entorno valida, usarla.
if defined XSE_FO4_GAME_PATH if exist "%XSE_FO4_GAME_PATH%\Fallout4.exe" (
    set "GAMEDIR=%XSE_FO4_GAME_PATH%"
    goto :eof
)

REM 2) Preguntar a Steam (registro + libraryfolders.vdf) via PowerShell.
set "PS=%TEMP%\f4mp_findfo4.ps1"
> "%PS%" echo $ErrorActionPreference='SilentlyContinue'
>> "%PS%" echo $steam=(Get-ItemProperty 'HKCU:\Software\Valve\Steam').SteamPath
>> "%PS%" echo if(-not $steam){$steam=(Get-ItemProperty 'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam').InstallPath}
>> "%PS%" echo $libs=@()
>> "%PS%" echo if($steam){$libs+=$steam}
>> "%PS%" echo $vdf=Join-Path $steam 'steamapps\libraryfolders.vdf'
>> "%PS%" echo if(Test-Path $vdf){Select-String -Path $vdf -Pattern '"path"\s+"(.+?)"' ^| ForEach-Object {$libs+=($_.Matches[0].Groups[1].Value -replace '\\\\','\')}}
>> "%PS%" echo foreach($l in $libs){$p=Join-Path $l 'steamapps\common\Fallout 4'; if(Test-Path (Join-Path $p 'Fallout4.exe')){Write-Output $p; break}}
for /f "usebackq delims=" %%p in (`powershell -NoProfile -ExecutionPolicy Bypass -File "%PS%" 2^>nul`) do set "GAMEDIR=%%p"
del "%PS%" >nul 2>&1
if defined GAMEDIR if exist "%GAMEDIR%\Fallout4.exe" goto :eof
set "GAMEDIR="

REM 3) Fallbacks en rutas tipicas por si Steam no respondio.
for %%d in (C D E F G) do (
    if not defined GAMEDIR if exist "%%d:\SteamLibrary\steamapps\common\Fallout 4\Fallout4.exe" set "GAMEDIR=%%d:\SteamLibrary\steamapps\common\Fallout 4"
    if not defined GAMEDIR if exist "%%d:\Steam\steamapps\common\Fallout 4\Fallout4.exe" set "GAMEDIR=%%d:\Steam\steamapps\common\Fallout 4"
    if not defined GAMEDIR if exist "%%d:\Program Files (x86)\Steam\steamapps\common\Fallout 4\Fallout4.exe" set "GAMEDIR=%%d:\Program Files (x86)\Steam\steamapps\common\Fallout 4"
    if not defined GAMEDIR if exist "%%d:\SteamLibrary\SteamLibrary\steamapps\common\Fallout 4\Fallout4.exe" set "GAMEDIR=%%d:\SteamLibrary\SteamLibrary\steamapps\common\Fallout 4"
)
goto :eof


:no_vs
echo [ERROR] No encuentro Visual Studio. Instalalo con la carga "Desarrollo para el escritorio con C++".
goto :end
:no_vc
echo [ERROR] Visual Studio no tiene las herramientas de C++ x64.
goto :end
:no_xmake
echo [ERROR] No encuentro xmake. Instalalo con:  winget install xmake
goto :end
:no_game
echo [ERROR] No encuentro Fallout 4 automaticamente.
echo         Define la ruta a mano antes de ejecutar, por ejemplo:
echo             set XSE_FO4_GAME_PATH=D:\SteamLibrary\steamapps\common\Fallout 4
echo         y vuelve a lanzar este .bat.
goto :end
:fail_build
echo [ERROR] Fallo la compilacion. Revisa el texto de arriba.
goto :end

:end
echo.
pause
endlocal
