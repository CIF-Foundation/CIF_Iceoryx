@echo off
setlocal

set "NSIS=%ProgramFiles(x86)%\NSIS\makensis.exe"
if not exist "%NSIS%" (
  echo Error: NSIS not found at "%NSIS%"
  exit /b 1
)

set "SCRIPT_DIR=%~dp0"
set "REPO_DIR=%SCRIPT_DIR%..\.."
set "RESOURCE_DLL=%SCRIPT_DIR%resource\iox2_cif.dll"
set "DIST_DLL=%REPO_DIR%\dist\iox2_cif.dll"

if not exist "%RESOURCE_DLL%" (
  if exist "%DIST_DLL%" (
    echo Copying %DIST_DLL% to resource...
    copy /Y "%DIST_DLL%" "%RESOURCE_DLL%" >nul
  ) else (
    echo Error: iox2_cif.dll not found at "%RESOURCE_DLL%" or "%DIST_DLL%"
    echo Build the Windows library first:
    echo   powershell -ExecutionPolicy Bypass -File .\scripts\build.ps1
    exit /b 1
  )
)

pushd "%SCRIPT_DIR%"
"%NSIS%" "cif_iox2.nsi"
if errorlevel 1 (
  popd
  exit /b 1
)

echo Successfully created cif-iox2-0.1.0.exe
popd
endlocal
