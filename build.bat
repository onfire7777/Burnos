@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "Q:\C++\BurnOS\build\Desktop_Qt_6_10_2_MSVC2022_64bit-Release"
"C:\Qt\6.10.2\msvc2022_64\bin\qmake.exe" ..\..\BurnOS.pro -spec win32-msvc
"C:\Qt\Tools\QtCreator\bin\jom\jom.exe" -j8 release
