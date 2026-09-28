"""Build shim: make ``pip install .`` compile the native library too.

The heavy lifting stays in the Makefile (transparent, easy to audit); this
just invokes it when the shared object is missing so that a plain
``pip install .`` / ``pip install -e .`` works from a fresh checkout.

Set ``WL_SCREENCOPY_SKIP_NATIVE=1`` to skip it (e.g. when the library is
already installed system-wide).
"""

from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path

from setuptools import setup
from setuptools.command.build_py import build_py
from setuptools.command.develop import develop

HERE = Path(__file__).resolve().parent
NATIVE = HERE / "python" / "wl_screencopy" / "_native" / "libwl_screencopy.so"


def _build_native(reason: str) -> None:
    if os.environ.get("WL_SCREENCOPY_SKIP_NATIVE") == "1":
        print(f"[wl_screencopy] skipping native build ({reason})", file=sys.stderr)
        return
    if sys.platform != "linux":
        print(f"[wl_screencopy] native library is only built on Linux "
              f"({sys.platform}); skipping", file=sys.stderr)
        return
    try:
        subprocess.run(["make", "native"], cwd=str(HERE), check=True)
    except FileNotFoundError:
        print("[wl_screencopy] `make` not found: build libwl_screencopy.so "
              "manually (see README.md) and set WL_SCREENCOPY_LIB.",
              file=sys.stderr)
    except subprocess.CalledProcessError as exc:
        print(f"[wl_screencopy] `make native` failed (status {exc.returncode}).\n"
              "Install the build dependencies first:\n"
              "  Debian/Ubuntu: apt install build-essential pkg-config "
              "libwayland-dev libwayland-bin\n"
              "  Fedora:        dnf install gcc make pkgconf-pkg-config "
              "libwayland-devel libwayland\n"
              "or build the shared object yourself with `make` and set "
              "WL_SCREENCOPY_LIB=/path/to/libwl_screencopy.so.",
              file=sys.stderr)
        raise


class build_py_with_native(build_py):
    def run(self) -> None:
        if not NATIVE.exists():
            _build_native("shared object missing")
        super().run()


class develop_with_native(develop):
    def run(self) -> None:
        if not NATIVE.exists():
            _build_native("shared object missing")
        super().run()


setup(
    cmdclass={"build_py": build_py_with_native, "develop": develop_with_native},
)
