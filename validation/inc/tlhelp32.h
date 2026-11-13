// tlhelp32.h — POSIX stand-in. The snapshot functions are implemented against /proc, so reader.cpp's
// find_pid()/find_module_base() walk live kernel state rather than a fixture.
#pragma once
#include "windows.h"

#define TH32CS_SNAPPROCESS  0x00000002
#define TH32CS_SNAPMODULE   0x00000008
#define TH32CS_SNAPMODULE32 0x00000010

typedef struct _PROCESSENTRY32W {
    DWORD dwSize; DWORD cntUsage; DWORD th32ProcessID; DWORD th32DefaultHeapID;
    DWORD th32ModuleID; DWORD cntThreads; DWORD th32ParentProcessID;
    LONG  pcPriClassBase; DWORD dwFlags; wchar_t szExeFile[MAX_PATH];
} PROCESSENTRY32W, *LPPROCESSENTRY32W;

typedef struct _PROCESSENTRY32 {
    DWORD dwSize; DWORD cntUsage; DWORD th32ProcessID; DWORD th32DefaultHeapID;
    DWORD th32ModuleID; DWORD cntThreads; DWORD th32ParentProcessID;
    LONG  pcPriClassBase; DWORD dwFlags; char szExeFile[MAX_PATH];
} PROCESSENTRY32, *LPPROCESSENTRY32;

typedef struct _MODULEENTRY32W {
    DWORD dwSize; DWORD th32ModuleID; DWORD th32ProcessID; DWORD GlblcntUsage;
    DWORD ProccntUsage; BYTE* modBaseAddr; DWORD modBaseSize; void* hModule;
    wchar_t szModule[MAX_PATH]; wchar_t szExePath[MAX_PATH];
} MODULEENTRY32W, *LPMODULEENTRY32W;

typedef struct _MODULEENTRY32 {
    DWORD dwSize; DWORD th32ModuleID; DWORD th32ProcessID; DWORD GlblcntUsage;
    DWORD ProccntUsage; BYTE* modBaseAddr; DWORD modBaseSize; void* hModule;
    char szModule[MAX_PATH]; char szExePath[MAX_PATH];
} MODULEENTRY32, *LPMODULEENTRY32;

HANDLE CreateToolhelp32Snapshot(DWORD flags, DWORD pid);
BOOL Process32First(HANDLE snap, PROCESSENTRY32* pe);
BOOL Process32Next(HANDLE snap, PROCESSENTRY32* pe);
BOOL Module32First(HANDLE snap, MODULEENTRY32* me);
BOOL Module32Next(HANDLE snap, MODULEENTRY32* me);
