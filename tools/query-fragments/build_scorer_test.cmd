@echo off
REM Сборка standalone-теста скоринга (C++-порт query_scan.py) и сверка с Python.
REM Требует: MSVC (cl), Python. Пути с кириллицей cl/ifstream не берут — работаем во временной папке.
REM
REM Использование:  build_scorer_test.cmd
setlocal
set SRC=%~dp0src
set OUT=%TEMP%\qf_scorer
if not exist "%OUT%" mkdir "%OUT%"

REM 1. Найти vcvarsall
set VCVARS=
for %%P in (
  "%ProgramFiles%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
  "%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"
  "%ProgramFiles(x86)%\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
) do if exist %%P set VCVARS=%%~P
if "%VCVARS%"=="" (
  echo Не найден vcvarsall.bat. Укажите путь к VS Build Tools вручную.
  exit /b 1
)

call "%VCVARS%" x64 >nul
cd /d "%OUT%"
cl /nologo /EHsc /std:c++17 /W3 /Fe:scorer_test.exe "%SRC%\scorer_test.cpp" "%SRC%\query_scorer.cpp" /I"%SRC%"
if errorlevel 1 exit /b 1

echo.
echo Собран: %OUT%\scorer_test.exe
echo Лексикон: %~dp0lexicon.json
echo Примеры:  %~dp0examples.md  (по одному тексту на строку в отдельном файле)
endlocal
