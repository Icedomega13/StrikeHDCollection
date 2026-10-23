@echo off
rem Builds dist\StrikeCollectionHD-Setup.exe from dist\StrikeCollectionHD.exe.
rem Needs Inno Setup 6: https://jrsoftware.org/isdl.php
setlocal
set ISCC="%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe"
if not exist %ISCC% set ISCC="%ProgramFiles%\Inno Setup 6\ISCC.exe"
if not exist %ISCC% set ISCC="%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe"
if not exist %ISCC% (
  echo Inno Setup 6 was not found. Install it from https://jrsoftware.org/isdl.php
  exit /b 1
)
%ISCC% "%~dp0StrikeCollectionHD.iss"
