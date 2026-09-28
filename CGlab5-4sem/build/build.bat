call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x64
cd /d D:\KgSem4_2026\KG_Sem4_Laba5\build
cmake .. -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Debug
nmake
