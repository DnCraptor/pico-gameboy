@echo off
setlocal EnableExtensions EnableDelayedExpansion

rem Build every supported ARM Cortex-M33 / RP2350 configuration.
rem Excluded intentionally:
rem   PICO_PLATFORM=rp2040
rem   RISC-V RP2350 targets
rem   m1p2launcher
rem   TFT / ILI9341
rem   legacy TV (SOFTTV is included)

cd /d "%~dp0"
set "ROOT=%CD%"
set "BUILD_ROOT=%ROOT%\build-all"

rem Prefer tools already present in PATH.
set "CMAKE=cmake"
set "NINJA=ninja"
where cmake >nul 2>nul || set "CMAKE="
where ninja >nul 2>nul || set "NINJA="

rem Fall back to the Raspberry Pi Pico VS Code Extension tool cache.
if not defined CMAKE (
    for /f "delims=" %%I in ('dir /b /s "%USERPROFILE%\.pico-sdk\cmake\*\bin\cmake.exe" 2^>nul') do if not defined CMAKE set "CMAKE=%%I"
)
if not defined NINJA (
    for /f "delims=" %%I in ('dir /b /s "%USERPROFILE%\.pico-sdk\ninja\*\ninja.exe" 2^>nul') do if not defined NINJA set "NINJA=%%I"
)

if not defined CMAKE (
    echo ERROR: cmake.exe not found.
    exit /b 1
)
if not defined NINJA (
    echo ERROR: ninja.exe not found.
    exit /b 1
)

set /a TOTAL=0
set /a OK=0
set /a FAILED=0

for %%B in (murmulator murmulator2 olimex-pico-pc waveshare_rp2350_pizero) do (
    for %%V in (VGA HDMI SOFTTV) do (
        for %%A in (PWM I2S I2S-CS4334 HWAY) do (
            set /a TOTAL+=1
            call :BUILD_ONE "%%B" "%%V" "%%A"
            if errorlevel 1 (
                set /a FAILED+=1
                echo.
                echo ============================================================
                echo FAILED: board=%%B video=%%V audio=%%A
                echo ============================================================
                goto :SUMMARY
            ) else (
                set /a OK+=1
            )
        )
    )
)

goto :SUMMARY

:BUILD_ONE
set "BOARD=%~1"
set "VIDEO=%~2"
set "AUDIO=%~3"
set "BDIR=%BUILD_ROOT%\%BOARD%\%VIDEO%-%AUDIO%"

set "VGA=OFF"
set "HDMI=OFF"
set "SOFTTV=OFF"
if /I "%VIDEO%"=="VGA" set "VGA=ON"
if /I "%VIDEO%"=="HDMI" set "HDMI=ON"
if /I "%VIDEO%"=="SOFTTV" set "SOFTTV=ON"

set "I2S=OFF"
set "I2S_CS4334=OFF"
set "HWAY=OFF"
if /I "%AUDIO%"=="I2S" set "I2S=ON"
if /I "%AUDIO%"=="I2S-CS4334" (
    set "I2S=ON"
    set "I2S_CS4334=ON"
)
if /I "%AUDIO%"=="HWAY" set "HWAY=ON"

echo.
echo ============================================================
echo [!TOTAL!/48] board=%BOARD%  video=%VIDEO%  audio=%AUDIO%
echo ============================================================

rem A dedicated directory per combination prevents stale CMake cache options.
"%CMAKE%" -S "%ROOT%" -B "%BDIR%" -G Ninja ^
    -DCMAKE_MAKE_PROGRAM="%NINJA%" ^
    -DCMAKE_BUILD_TYPE=Release ^
    -DPICO_PLATFORM=rp2350-arm-s ^
    -DPICO_BOARD=%BOARD% ^
    -Dm1p2launcher=OFF ^
    -DTFT=OFF ^
    -DILI9341=OFF ^
    -DTV=OFF ^
    -DVGA=%VGA% ^
    -DHDMI=%HDMI% ^
    -DSOFTTV=%SOFTTV% ^
    -DI2S=%I2S% ^
    -DI2S_CS4334=%I2S_CS4334% ^
    -DHWAY=%HWAY%
if errorlevel 1 exit /b 1

"%CMAKE%" --build "%BDIR%" --config Release --target all
if errorlevel 1 exit /b 1

exit /b 0

:SUMMARY
echo.
echo ============================================================
echo Build summary: %OK% OK, %FAILED% failed, %TOTAL% attempted, 48 total
if %FAILED%==0 (
    echo ALL BUILDS COMPLETED SUCCESSFULLY.
    echo UF2 files are in: "%ROOT%\bin\Release"
) else (
    echo Build stopped on the first failure.
)
echo ============================================================

if not "%FAILED%"=="0" exit /b 1
exit /b 0
