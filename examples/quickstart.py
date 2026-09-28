#!/usr/bin/env python3
"""Minimal start: grab the screen and look at it as a NumPy array.

    python3 examples/quickstart.py

Prints shape/dtype/statistics, then saves a PNG. Everything here is the
public API - there is nothing else to know.
"""

from __future__ import annotations

import sys

import numpy as np

import wl_screencopy as wcs


def main() -> int:
    print(f"wl_screencopy {wcs.__version__}")
    print(f"compositor backends: {wcs.backends() or 'NONE'}")
    if not wcs.supports_screencopy():
        print("This compositor does not implement wlr-screencopy-unstable-v1.")
        print("Outputs it still reports:")
        for out in wcs.outputs():
            print("  ", out)
        return 1

    for out in wcs.outputs():
        print("  ", out)

    # The one-liner: an (H, W, 3) uint8 array of RGB values.
    frame = wcs.capture()
    print(f"\ncapture() -> {type(frame).__name__} shape={frame.shape} "
          f"dtype={frame.dtype} C-contiguous={frame.flags['C_CONTIGUOUS']}")

    # It is an ordinary array, so anything NumPy does just works.
    print(f"  mean colour      : {frame.reshape(-1, 3).mean(axis=0).round(1)}")
    print(f"  brightest pixel  : {frame.reshape(-1, 3).max(axis=0)}")
    print(f"  unique colours   : {len(np.unique(frame.reshape(-1, 3), axis=0))}")
    print(f"  top-left 4x4 red : {frame[:4, :4, 0].tolist()}")

    # Arithmetic, slicing, downsampling.
    grey = frame @ np.array([0.299, 0.587, 0.114], dtype=np.float32)
    print(f"  luma range       : {grey.min():.0f} .. {grey.max():.0f}")
    small = frame[::8, ::8, :]          # cheap 8x downsample
    print(f"  sliced view      : {small.shape}")

    # Metadata (presentation timestamp, damage, scale) via grab().
    array, info = wcs.grab()
    print(f"\ngrab() metadata: pts={array.shape} info.pts={info.pts:.6f}s "
          f"scale={info.scale} damage={info.damage}")

    out = "quickstart.png"
    if len(sys.argv) > 1:
        out = sys.argv[1]
    wcs.save_image(frame, out)
    print(f"wrote {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
