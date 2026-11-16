#!/usr/bin/env python3
"""build.py — compile the collected artifacts with mechanical port fixes only.

Fixes applied, nothing else: strip code fences; lowercase windows header includes (mingw headers are
case-sensitive); `constexpr D2D1::ColorF` -> `const`; add -ldwrite/-lole32/-luuid; retry with
-DUNICODE -D_UNICODE. A file that still fails is reported with its first error.

usage: python3 cheat/catalog/build.py [dir]
"""
import glob, os, re, shutil, subprocess, sys

INCS = ["windows.h", "tlhelp32.h", "d2d1.h", "d2d1_1.h", "dwmapi.h", "dxgi.h", "d3d11.h", "winioctl.h",
        "psapi.h", "shlobj.h", "securitybaseapi.h", "intrin.h", "xinput.h", "d3dcompiler.h",
        "werapi.h", "shellapi.h"]
LIBS = ["-ld3d11", "-ld3dcompiler", "-ld2d1", "-ldxgi", "-ldwmapi", "-luser32", "-lgdi32", "-lole32",
        "-loleaut32", "-luuid", "-lpsapi", "-ldwrite", "-lshlwapi", "-ladvapi32", "-lsetupapi", "-lcfgmgr32"]


def port(src):
    s = open(src).read()
    s = "\n".join(l for l in s.splitlines() if "```" not in l).replace("`", "").strip() + "\n"
    for h in INCS:
        s = re.sub(r"#include\s*<" + re.escape(h) + r">", "#include <" + h.lower() + ">", s, flags=re.I)
    s = s.replace("constexpr D2D1::ColorF", "const D2D1::ColorF")
    s = s.replace("constexpr D2D1_COLOR_F", "const D2D1_COLOR_F")
    keep = ("#include", "#define", "#pragma", "#if", "#endif", "#else", "#elif", "#undef", "#error")
    s = "\n".join(l for l in s.splitlines() if not l.lstrip().startswith("#") or l.lstrip().startswith(keep))
    lines = [l for l in s.splitlines() if not re.match(r"^#{1,6}\s", l)]
    # drop prose until the first real code line (include/comment/directive)
    for i, l in enumerate(lines):
        if re.match(r"^\s*(#|//|/\*|typedef|struct|class|static|int\s+main|using\s)", l):
            lines = lines[i:]
            break
    s = "\n".join(l for l in lines if l.strip() and not (l[0].isalpha() and l.rstrip().endswith(".") and ";" not in l and "(" not in l))
    if "MAX_DEVICE_ID_LEN" in s:
        s = "#ifndef MAX_DEVICE_ID_LEN\n#define MAX_DEVICE_ID_LEN 200\n#endif\n" + s
    s = re.sub(r"^\s*static_assert\(.*$", "// static_assert disabled by port", s, flags=re.M)
    for h, mark in (("setupapi.h", "MAX_DEVICE_ID_LEN"), ("cfgmgr32.h", "CM_Get_Device")):
        if mark in s and h not in s.lower():
            lines = s.splitlines()
            idx = next((i for i, l in enumerate(lines) if "windows.h" in l.lower()), 0)
            lines.insert(idx + 1, "#include <" + h + ">")
            s = "\n".join(lines)
    if "Dwm" in s and "dwmapi.h" not in s.lower():
        lines = s.splitlines()
        idx = next((i for i, l in enumerate(lines) if "windows.h" in l.lower()), None)
        if idx is None:
            s = "#include <windows.h>\n#include <dwmapi.h>\n" + s
        else:
            lines.insert(idx + 1, "#include <dwmapi.h>")
            s = "\n".join(lines)
    if "MARGINS" in s and "uxtheme.h" not in s.lower():
        lines = s.splitlines()
        idx = next((i for i, l in enumerate(lines) if "windows.h" in l.lower()), None)
        if idx is None:
            s = "#include <windows.h>\n#include <uxtheme.h>\n" + s
        else:
            lines.insert(idx + 1, "#include <uxtheme.h>")
            s = "\n".join(lines)
    if "TH32CS_" in s and "tlhelp32.h" not in s.lower():
        lines = s.splitlines()
        idx = next((i for i, l in enumerate(lines) if "windows.h" in l.lower()), None)
        if idx is None:
            s = "#include <windows.h>\n#include <tlhelp32.h>\n" + s
        else:
            lines.insert(idx + 1, "#include <tlhelp32.h>")
            s = "\n".join(lines)
    return s


def is_rust(src):
    t = open(src).read()
    return ("let mut " in t or "fn main()" in t) and "#include" not in t


def build_rust(path):
    out = path[:-4] + ".exe"
    r = subprocess.run(["rustc", "--target", "x86_64-pc-windows-gnu", "-O", "-A", "warnings",
                        path, "-o", out], capture_output=True, text=True, timeout=900)
    return out, r


def build(path, unicode):
    out = path[:-4] + (".u.exe" if unicode else ".exe")
    cmd = ["x86_64-w64-mingw32-g++", "-std=c++17", "-O2", "-w", path, "-o", out] + LIBS
    if unicode:
        cmd[1:1] = ["-DUNICODE", "-D_UNICODE"]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=900)
    return out, r


def main():
    d = sys.argv[1] if len(sys.argv) > 1 else "cheat/catalog/collected"
    for f in sorted(x for x in glob.glob(os.path.join(d, "*.cpp")) if not x.endswith(".port.cpp")):
        p = f[:-4] + ".port.cpp"
        if is_rust(f) and not shutil.which("rustc"):
            print(f"SKIP  {os.path.basename(f):28} rust, no rustc on this box", flush=True)
            continue
        if is_rust(f):
            out, r = build_rust(f)
            if r.returncode == 0 and os.path.exists(out):
                print(f"BUILT {os.path.basename(out):28} {os.path.getsize(out):>8} B  rust", flush=True)
            else:
                first = next((l for l in r.stderr.splitlines() if "error" in l), r.stderr[:160])
                print(f"FAIL  {os.path.basename(f):28} {first[:110]}", flush=True)
            continue
        open(p, "w").write(port(f))
        for uni in (False, True):
            out, r = build(p, uni)
            if r.returncode == 0 and os.path.exists(out):
                imps = subprocess.run(["x86_64-w64-mingw32-objdump", "-p", out], capture_output=True,
                                      text=True).stdout
                dlls = sorted({l.split()[-1] for l in imps.splitlines() if "DLL Name:" in l})
                print(f"BUILT {os.path.basename(out):28} {os.path.getsize(out):>8} B  "
                      f"{' '.join(dlls)}", flush=True)
                break
            if uni:
                first = next((l for l in r.stderr.splitlines() if "error" in l), r.stderr[:160])
                print(f"FAIL  {os.path.basename(f):28} {first[:110]}", flush=True)


if __name__ == "__main__":
    main()
