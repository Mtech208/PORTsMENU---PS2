@echo off
REM Build script for PORTMENU PS2
REM Run from native cmd.exe (NOT MSYS2 terminal!)
REM Open cmd.exe from Windows Start menu.

setlocal enabledelayedexpansion

set REPO_ROOT=C:\Users\gamer\OneDrive\Desktop\PORTsMENUPS2
set BUILD_DIR=%REPO_ROOT%\build
set PS2DEV=C:\Users\gamer\OneDrive\Desktop\ps2dev-official\ps2dev
set CMAKE_EXE=C:\Program Files\CMake\bin\cmake.exe
set NINJA_EXE=C:\Users\gamer\AppData\Local\Microsoft\WinGet\Packages\Ninja-build.Ninja_Microsoft.Winget.Source_8wekyb3d8bbwe\ninja.exe

echo === PORTMENU PS2 Build ===
echo PS2DEV: %PS2DEV%
echo Build dir: %BUILD_DIR%

REM Force CMake reconfigure but KEEP user assets (ART/, games.cfg, *.log)
if exist "%BUILD_DIR%\CMakeCache.txt" del /q "%BUILD_DIR%\CMakeCache.txt"
if exist "%BUILD_DIR%\CMakeFiles" rmdir /s /q "%BUILD_DIR%\CMakeFiles"

set GCC=%PS2DEV%\ee\bin\mips64r5900el-ps2-elf-gcc.exe
if not exist "%GCC%" (
    echo Cross-compiler not found at %GCC%
    echo Trying alternative path...
    set GCC=%PS2DEV%\ee\mips64r5900el-ps2-elf\bin\mips64r5900el-ps2-elf-gcc.exe
    if not exist "%GCC%" (
        echo Cross-compiler not found at alternative path either.
        exit /b 1
    )
)

echo Testing cross-compiler...
%GCC% --version

echo Configuring with CMake...
"%CMAKE_EXE%" -G Ninja ^
    -DCMAKE_BUILD_TYPE=Release ^
    -DCMAKE_MAKE_PROGRAM=%NINJA_EXE% ^
    -DCMAKE_TOOLCHAIN_FILE=%REPO_ROOT%\ps2_toolchain.cmake ^
    -DPS2DEV=%PS2DEV% ^
    -B %BUILD_DIR% ^
    -S %REPO_ROOT%

if errorlevel 1 (
    echo.
    echo CMake configure failed
    exit /b 1
)

echo Building with Ninja...
"%CMAKE_EXE%" --build %BUILD_DIR% --parallel

if errorlevel 1 (
    echo.
    echo === BUILD FAILED ===
    echo Check errors above
    exit /b 1
)

set ELF=%BUILD_DIR%\PORTMENU.elf
if exist "%ELF%" (
    echo.
    echo === BUILD SUCCESS ===
    echo ELF: %ELF%
    for %%I in ("%ELF%") do echo Size: %%~zI bytes

    set STRIP=%PS2DEV%\ee\bin\mips64r5900el-ps2-elf-strip.exe
    if exist "%STRIP%" (
        set STRIPPED_ELF=%BUILD_DIR%\PORTMENU_stripped.elf
        copy "%ELF%" "%STRIPPED_ELF%" /Y
        "%STRIP%" "%STRIPPED_ELF%"
        for %%I in ("%STRIPPED_ELF%") do echo Stripped: %%~zI bytes
    )

    echo.
    echo Copia PORTMENU.elf a tu USB/MC/HDD y ejecútalo con uLaunchELF/OPL/FMCB
    echo No olvides crear la carpeta ART/ con covers/fondos/logos
    echo y editar games.cfg con tus rutas de ELFs.
) else (
    echo ELF not found at %ELF%
    exit /b 1
)

echo.
echo Generando ISO autocontenida (menu + juegos + arte)...
powershell -NoProfile -ExecutionPolicy Bypass -File "%REPO_ROOT%\make_iso.ps1"
if errorlevel 1 (
    echo === ISO FAILED ===
    exit /b 1
)

echo.
echo Build completed successfully!
pause