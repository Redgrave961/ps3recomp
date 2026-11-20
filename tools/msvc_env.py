#!/usr/bin/env python3
import glob
import os
import subprocess
import sys
import tempfile

_CACHED = None


def find_vcvars():
    if os.name != "nt":
        return None
    roots = [
        os.environ.get("ProgramFiles", r"C:\Program Files"),
        os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"),
    ]
    hits = []
    for root in roots:
        hits += glob.glob(os.path.join(
            root, "Microsoft Visual Studio", "*", "*",
            "VC", "Auxiliary", "Build", "vcvars64.bat"))
    return sorted(hits, reverse=True)[0] if hits else None


def environ():
    global _CACHED
    if os.name != "nt":
        return dict(os.environ)
    if _CACHED is not None:
        return dict(_CACHED)

    env = dict(os.environ)
    vcvars = find_vcvars()
    if vcvars:
        bat = None
        try:
            fd, bat = tempfile.mkstemp(suffix=".bat", prefix="ps3_msvc_")
            with os.fdopen(fd, "w") as f:
                f.write("@echo off\n")
                f.write('call "%s" >nul 2>&1\n' % vcvars)
                f.write("set\n")
            out = subprocess.run(["cmd", "/c", bat], capture_output=True,
                                 text=True, timeout=120).stdout
            for line in out.splitlines():
                key, sep, value = line.partition("=")
                if sep and key:
                    env[key] = value
        except (OSError, subprocess.SubprocessError):
            pass
        finally:
            if bat:
                try:
                    os.remove(bat)
                except OSError:
                    pass

    for cand in [os.path.join(os.environ.get("ProgramFiles", r"C:\Program Files"),
                              "LLVM", "bin")]:
        if os.path.isdir(cand) and cand.lower() not in env.get("PATH", "").lower():
            env["PATH"] = cand + os.pathsep + env.get("PATH", "")

    _CACHED = env
    return dict(env)


if __name__ == "__main__":
    v = find_vcvars()
    print("vcvars64.bat: %s" % (v or "not found"))
    if os.name == "nt":
        e = environ()
        print("INCLUDE entries: %d" % len(e.get("INCLUDE", "").split(os.pathsep)))
        print("LIB entries:     %d" % len(e.get("LIB", "").split(os.pathsep)))
        import shutil
        print("clang-cl:        %s" % (shutil.which("clang-cl", path=e.get("PATH")) or "not found"))
    sys.exit(0)
