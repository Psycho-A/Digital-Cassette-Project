# Microsoft Developer Studio Project File - Name="etm_table" - Package Owner=<4>
# Microsoft Developer Studio Generated Build File, Format Version 6.00
# ** DO NOT EDIT **

# TARGTYPE "Win32 (x86) Console Application" 0x0103

CFG=etm_table - Win32
!MESSAGE This is not a valid makefile. To build this project using NMAKE,
!MESSAGE use the Export Makefile command and run
!MESSAGE 
!MESSAGE NMAKE /f "etm_table.mak".
!MESSAGE 
!MESSAGE You can specify a configuration when running NMAKE
!MESSAGE by defining the macro CFG on the command line. For example:
!MESSAGE 
!MESSAGE NMAKE /f "etm_table.mak" CFG="etm_table - Win32"
!MESSAGE 
!MESSAGE Possible choices for configuration are:
!MESSAGE 
!MESSAGE "etm_table - Win32" (based on "Win32 (x86) Console Application")
!MESSAGE 

# Begin Project
# PROP AllowPerConfigDependencies 0
CPP=cl.exe
MTL=midl.exe

!IF "$(CFG)" == "etm_table - Win32"

# PROP BASE Use_MFC 0
# PROP BASE Use_Debug_Libraries 0
# PROP BASE Output_Dir "Release"
# PROP BASE Intermediate_Dir "Release"
# PROP BASE Target_Dir ""
# PROP Use_MFC 0
# PROP Use_Debug_Libraries 0
# PROP Output_Dir "Release"
# PROP Intermediate_Dir "Release"
# PROP Ignore_Export_Lib 0
# PROP Target_Dir ""
# ADD BASE CPP /nologo /W3 /I "include" /TC /MT /G6 /O2
# ADD CPP /nologo /W3 /I "include" /TC /MT /G6 /O2
# ADD BASE MTL /nologo /win32
# ADD MTL /nologo /win32
BSC32=bscmake.exe
# ADD BASE BSC32 /nologo
# ADD BSC32 /nologo
LINK32=link.exe
# ADD BASE LINK32 /nologo /subsystem:console
# ADD LINK32 /nologo /subsystem:console
# Begin Custom Build - Publishing to target directory (..\)...
TargetDir=.\Release
TargetPath=.\Release\etm_table.exe
InputPath=.\Release\etm_table.exe
SOURCE="$(InputPath)"
BuildCmds= \
	if exist ..\etm_table.exe attrib -r ..\etm_table.exe \
	move /y $(TargetPath) ..\etm_table.exe > nul \
	del /f /s /q .\*.exe .\*.obj > nul \

"..\etm_table.exe" : $(SOURCE) "$(INTDIR)" "$(OUTDIR)"
$(BuildCmds)
	
# End Custom Build

!ENDIF

# Begin Target

# Name "etm_table - Win32"

# Begin Group "Source Files"
# PROP Default_Filter "cpp;c;cxx;rc;def;r;odl;idl;hpj;bat"

# Begin Source File
SOURCE=.\etm_table.c
# End Source File

# End Group

# End Target
# End Project
