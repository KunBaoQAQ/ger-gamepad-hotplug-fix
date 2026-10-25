@echo off
rem ---------------------------------------------------------------------
rem  Build dinput8.dll (32-bit DirectInput8 proxy)
rem  Requires: Visual Studio 2019/2022/2026 with "Desktop development with C++"
rem            + Windows 10/11 SDK
rem ---------------------------------------------------------------------
setlocal enabledelayedexpansion
cd /d "%~dp0"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo [ERROR] vswhere.exe not found - is Visual Studio installed?
    pause
    exit /b 1
)

set "VSPATH="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH (
    echo [ERROR] Visual Studio with the C++ toolset was not found.
    echo         Install "Desktop development with C++" and try again.
    pause
    exit /b 1
)

echo Using Visual Studio: !VSPATH!
call "!VSPATH!\VC\Auxiliary\Build\vcvars32.bat" >nul
if errorlevel 1 (
    echo [ERROR] vcvars32.bat failed.
    pause
    exit /b 1
)

if not exist obj mkdir obj
if not exist out mkdir out

cl /nologo /O2 /MT /EHsc /W3 /D_CRT_SECURE_NO_WARNINGS /Fo:obj\ /Fd:obj\dinput8_fix.pdb dinput8_fix.cpp ^
   /link /DLL /DEF:dinput8.def /OUT:out\dinput8.dll /MACHINE:X86 ^
   dxguid.lib user32.lib kernel32.lib

if errorlevel 1 (
    echo.
    echo [ERROR] build failed.
    pause
    exit /b 1
)

echo.
echo Done:  out\dinput8.dll
echo Copy it to the game folder together with dinput8_fix.ini
echo (if the game already has a dinput8.dll from a translation patch,
echo  rename that one to GECV_LOADER.dll first).
echo.
pause
