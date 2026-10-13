@echo off
rem Builds "FathersFallout.exe" with the .NET Framework 4 compiler that ships with Windows.
rem Art (art\logo.png, art\icon.ico) is made from FatherLamb's logo by art\make_art.py.
set CSC=%WINDIR%\Microsoft.NET\Framework64\v4.0.30319\csc.exe
if not exist "%CSC%" set CSC=%WINDIR%\Microsoft.NET\Framework\v4.0.30319\csc.exe
cd /d "%~dp0"
"%CSC%" /nologo /target:winexe /optimize+ /out:"FathersFallout.exe" /win32icon:art\icon.ico ^
  /resource:art\logo.png,FathersFallout.logo.png ^
  /r:System.dll /r:System.Core.dll /r:System.Drawing.dll /r:System.Windows.Forms.dll Launcher.cs
if errorlevel 1 (echo [ERROR] build failed & exit /b 1)
echo [OK] FathersFallout.exe
