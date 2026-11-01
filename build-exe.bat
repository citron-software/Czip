@echo off
python -m PyInstaller --noconfirm --clean --onefile --windowed --name Czip --add-binary "runtime\7z.exe;." --add-binary "runtime\7z.dll;." --add-data "NOTICE-7ZIP.txt;." Czip.py
if errorlevel 1 exit /b %errorlevel%
echo Build complete: dist\Czip.exe
