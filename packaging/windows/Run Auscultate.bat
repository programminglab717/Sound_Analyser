@echo off
rem ---------------------------------------------------------------------------
rem  Auscultate -- Developed by Delta Creation Co.
rem
rem  WHAT THIS IS FOR
rem
rem  Windows marks every file that came out of a downloaded zip as having come
rem  from the internet. That mark is a small piece of data attached to the file
rem  called the Zone Identifier, and SmartScreen reads it: an application that
rem  carries it, and that Windows has not seen enough copies of to have an
rem  opinion about, gets "Windows protected your PC".
rem
rem  Auscultate is not signed, because a signing certificate is sold and this
rem  product does not buy anything. So it carries no reputation and the mark is
rem  what triggers the block.
rem
rem  This removes that mark from the files you just extracted, and then starts
rem  the application. It is doing the same thing as right-clicking the zip,
rem  opening Properties and ticking Unblock -- which is Microsoft's own remedy
rem  for this -- except it covers every file at once and after the fact.
rem
rem  It does not disable anything, it does not need administrator rights, and it
rem  changes nothing outside this folder.
rem
rem  IF THIS DOES NOT WORK
rem
rem  Smart App Control is a separate and much stricter feature. It refuses
rem  unsigned applications outright and offers no way past. If it is switched on
rem  you will get no "More info" link and this script will not help, because the
rem  mark is not what is stopping it. See README-FIRST.txt next to this file.
rem ---------------------------------------------------------------------------

setlocal
pushd "%~dp0"

if not exist "auscultate.exe" (
    echo.
    echo   Could not find auscultate.exe beside this script.
    echo.
    echo   Extract the whole zip first, then run this from the extracted
    echo   folder. Running it from inside the zip will not work, because
    echo   Windows opens the zip in a temporary place.
    echo.
    popd
    pause
    exit /b 1
)

echo.
echo   Removing the downloaded-from-the-internet mark...

powershell -NoProfile -ExecutionPolicy Bypass -Command "Get-ChildItem -LiteralPath '.' -Recurse -File | Unblock-File" >nul 2>&1

if errorlevel 1 (
    echo.
    echo   That did not work. You can do the same thing by hand:
    echo     right-click the zip you downloaded, choose Properties,
    echo     tick Unblock, press OK, and extract it again.
    echo.
)

echo   Starting Auscultate...
echo.

start "" "auscultate.exe"

popd
endlocal
exit /b 0
