@echo off
setlocal ENABLEEXTENSIONS
pushd "%~dp0"
del /f /s /q *.obj *.sbr *.pdb *.plg *.exp *.pch *.map *.log *.idb *.opt *.ncb *.dsw *.aps *.plg
del /f /q /a *.obj *.wav.* *.raw *.data *.out *.inf *.bin.* *.mp3 *.aac *.m4a *.opus raw_samples*.*

for /f "delims=" %%a in ('dir /b /s /ad *Release*') do (rd /s /q "%%~a")
for /f "delims=" %%a in ('dir /b /s /ad *Debug*') do (rd /s /q "%%~a")
popd

cls
exit /b
