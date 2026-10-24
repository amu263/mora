@echo off
setlocal DisableDelayedExpansion
if exist "%~dp0mora.exe" (
  "%~dp0mora.exe" %*
) else if exist "%~dp0build\mora.exe" (
  "%~dp0build\mora.exe" %*
) else (
  echo MORA executable not found. Build the project or extract the release package. 1>&2
  exit /b 1
)
exit /b %errorlevel%
