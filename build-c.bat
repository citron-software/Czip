@echo off
gcc Czip.c -std=c11 -O2 -municode -mwindows -lcomctl32 -lcomdlg32 -lshell32 -lole32 -lmsimg32 -o Czip.exe
if errorlevel 1 exit /b %errorlevel%
echo Build complete: Czip.exe
