// windows.h — POSIX stand-in for validating model-written Windows artifacts on Linux.
//
// Scope: exactly the symbols the three cheat/valorant files reference. Winsock, D3D, ntddk and the
// real GDI are absent on purpose — anything not listed here is a symbol the artifacts do not use.
//
// Semantics that matter for validation are real, not stubbed:
//   * FindPid / FindModuleBase / ReadProcessMemory work against live /proc data (shim.cpp), so the
//     pointer-chain walk in reader.cpp is exercised end to end against a running target process.
// Everything drawing-related is a recorded no-op: the GDI calls append to a draw log (g_shim_draws)
// instead of pixels, which is what the radar test asserts on.
#pragma once

#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <strings.h>
#include <vector>
#include <string>

// ── base types ───────────────────────────────────────────────────────────────
using DWORD   = uint32_t;
using ULONG   = uint32_t;
using LONG    = int32_t;
using WORD    = uint16_t;
using BYTE    = uint8_t;
using BOOL    = int;
using ATOM    = uint16_t;
using UINT    = unsigned int;
using WPARAM  = uintptr_t;
using LPARAM  = intptr_t;
using LRESULT = intptr_t;
using SIZE_T  = size_t;
using HINSTANCE = void*;
using HANDLE    = void*;
using HWND      = void*;
using HDC       = void*;
using HBRUSH    = void*;
using HPEN      = void*;
using HGDIOBJ   = void*;
using HBITMAP   = void*;
using HICON     = void*;
using HCURSOR   = void*;
using LPCVOID   = const void*;
using LPVOID    = void*;
using LPSTR     = char*;
using LPCWSTR   = const wchar_t*;
using LPWSTR    = wchar_t*;

#define WINAPI
#define CALLBACK
#define TRUE  1
#define FALSE 0
#define MAX_PATH 260
#ifndef INVALID_HANDLE_VALUE
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#endif

typedef struct _RECT { LONG left, top, right, bottom; } RECT;
typedef struct _POINT { LONG x, y; } POINT;
typedef struct _PAINTSTRUCT { HDC hdc; BOOL fErase; RECT rcPaint; } PAINTSTRUCT;
typedef struct _MSG { HWND hwnd; UINT message; WPARAM wParam; LPARAM lParam; } MSG;

typedef LRESULT (CALLBACK* WNDPROC)(HWND, UINT, WPARAM, LPARAM);
typedef struct _WNDCLASSEXW {
    UINT cbSize; UINT style; WNDPROC lpfnWndProc; int cbClsExtra; int cbWndExtra;
    HINSTANCE hInstance; HICON hIcon; HCURSOR hCursor; HBRUSH hbrBackground;
    LPCWSTR lpszMenuName; LPCWSTR lpszClassName; HICON hIconSm;
} WNDCLASSEXW;

// ── constants the artifacts reference ────────────────────────────────────────
#define CS_VREDRAW       0x0001
#define CS_HREDRAW       0x0002
#define WS_POPUP         0x80000000L
#define WS_VISIBLE       0x10000000L
#define WS_EX_TOPMOST    0x00000008L
#define WS_EX_TOOLWINDOW 0x00000080L
#define WS_EX_NOACTIVATE 0x08000000L
#define SW_SHOWNOACTIVATE 4
#define WM_PAINT         0x000F
#define WM_DESTROY       0x0002
#define WM_ERASEBKGND    0x0014
#define MB_ICONERROR     0x00000010
#define IDC_ARROW        ((LPCWSTR)(intptr_t)32512)
#define NULL_PEN         8
#define BLACK_BRUSH      4
#define SRCCOPY          0x00CC0020
#define PM_REMOVE        0x0001
#define ERROR_CLASS_ALREADY_EXISTS 1410
#define PS_SOLID         0
#define DT_CENTER        0x0001
#define DT_SINGLELINE    0x0020
#define DT_VCENTER       0x0004
#define PROCESS_VM_READ          0x0010
#define PROCESS_QUERY_INFORMATION 0x0400

#define RGB(r, g, b) ((DWORD)(((BYTE)(r)) | (((BYTE)(g)) << 8) | (((BYTE)(b)) << 16)))

// ── process / module access (real, see shim.cpp) ─────────────────────────────
HANDLE OpenProcess(DWORD access, BOOL inherit, DWORD pid);
BOOL   CloseHandle(HANDLE h);
BOOL   ReadProcessMemory(HANDLE proc, LPCVOID addr, LPVOID buf, SIZE_T len, SIZE_T* read);
DWORD  GetLastError();
void   Sleep(DWORD ms);
int    _stricmp(const char* a, const char* b);
BOOL   GetExitCodeProcess(HANDLE proc, DWORD* code);
#define STILL_ACTIVE 259

// ── drawing / windowing (recorded no-ops, see shim.cpp) ──────────────────────
struct ShimDraw { char op[16]; int a, b, c, d; DWORD color; };
extern std::vector<ShimDraw> g_shim_draws;

HDC    GetDC(HWND h);
int    ReleaseDC(HWND h, HDC dc);
HDC    CreateCompatibleDC(HDC h);
HBITMAP CreateCompatibleBitmap(HDC h, int w, int hh);
HPEN   CreatePen(int style, int width, DWORD color);
HBRUSH CreateSolidBrush(DWORD color);
HGDIOBJ SelectObject(HDC dc, HGDIOBJ obj);
BOOL   DeleteObject(HGDIOBJ obj);
BOOL   DeleteDC(HDC dc);
BOOL   BitBlt(HDC dst, int x, int y, int w, int hh, HDC src, int sx, int sy, DWORD rop);
BOOL   Ellipse(HDC dc, int l, int t, int r, int b);
BOOL   MoveToEx(HDC dc, int x, int y, LPVOID old);
BOOL   LineTo(HDC dc, int x, int y);
int    FillRect(HDC dc, const RECT* rc, HBRUSH br);
HGDIOBJ GetStockObject(int obj);
BOOL   GetClientRect(HWND h, RECT* rc);
HDC    BeginPaint(HWND h, PAINTSTRUCT* ps);
BOOL   EndPaint(HWND h, const PAINTSTRUCT* ps);
BOOL   InvalidateRect(HWND h, const RECT* rc, BOOL erase);
int    MessageBoxW(HWND h, LPCWSTR text, LPCWSTR caption, UINT type);
HWND   CreateWindowExW(DWORD ex, LPCWSTR cls, LPCWSTR title, DWORD style,
                       int x, int y, int w, int hh, HWND parent, void* menu,
                       HINSTANCE inst, LPVOID param);
BOOL   ShowWindow(HWND h, int cmd);
BOOL   UpdateWindow(HWND h);
BOOL   DestroyWindow(HWND h);
HWND   GetForegroundWindow();
HWND   FindWindowW(LPCWSTR cls, LPCWSTR title);
BOOL   PeekMessageW(MSG* msg, HWND h, UINT min, UINT max, UINT remove);
BOOL   TranslateMessage(const MSG* msg);
LRESULT DispatchMessageW(const MSG* msg);
void   PostQuitMessage(int code);
ATOM   RegisterClassExW(const WNDCLASSEXW* wc);
BOOL   UnregisterClassW(LPCWSTR cls, HINSTANCE inst);
LRESULT DefWindowProcW(HWND h, UINT msg, WPARAM wp, LPARAM lp);
HINSTANCE GetModuleHandleW(LPCWSTR name);
HCURSOR LoadCursorW(HINSTANCE inst, LPCWSTR name);
