#!/usr/bin/env python3
"""Use captured frames for analysis: motion detection, pure NumPy.

    python3 examples/motion_detect.py [--seconds 15] [--threshold 22]

Demonstrates the pattern you normally want for CV work:

* one shared scratch buffer (``out=``) so there is no allocation per frame;
* the previous frame kept as a float32 array for temporal difference;
* optional OpenCV path (``cv2`` is used when importable, never required).

Prints one line per frame: mean activity and the bounding box of the motion.
"""

from __future__ import annotations

import argparse
import time

import numpy as np

import wl_screencopy as wcs

try:  # optional, purely illustrative
    import cv2
except ImportError:  # pragma: no cover
    cv2 = None


def motion_stats(prev_f32: np.ndarray, frame: np.ndarray,
                 threshold: int) -> tuple:
    """Return (mean_activity, bounding_box_or_None, mask)."""
    cur = frame.astype(np.float32, copy=False)
    diff = np.abs(cur - prev_f32).mean(axis=2)      # (H, W) luma-ish delta
    mask = diff > threshold
    if not mask.any():
        return float(diff.mean()), None, mask
    rows = np.flatnonzero(mask.any(axis=1))
    cols = np.flatnonzero(mask.any(axis=0))
    box = (int(cols[0]), int(rows[0]),
           int(cols[-1] - cols[0] + 1), int(rows[-1] - rows[0] + 1))
    return float(diff.mean()), box, mask


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--seconds", type=float, default=15.0)
    ap.add_argument("--fps", type=float, default=30.0)
    ap.add_argument("--threshold", type=float, default=22.0,
                    help="mean channel delta that counts as motion")
    ap.add_argument("--output", default=None)
    ap.add_argument("--downscale", type=int, default=2,
                    help="analyse every Nth pixel to save CPU")
    args = ap.parse_args()

    if not wcs.supports_screencopy():
        raise SystemExit("no wlr-screencopy support on this compositor")

    recorder = wcs.Recorder(output=args.output, fps=args.fps, buffers=3,
                            timeout=10.0)
    with recorder:
        h, w, c = recorder.frame_shape
        step = max(1, args.downscale)
        print(f"analysing {w}x{h} every {step}px at {args.fps:g} fps "
              f"(cv2={'yes' if cv2 else 'no'})")

        scratch = np.empty((h, w, c), dtype=np.uint8)
        prev = None
        start = time.monotonic()
        moving_frames = 0
        for index, frame in enumerate(recorder.frames(out=scratch)):
            small = frame[::step, ::step, :]
            if prev is None:
                prev = small.astype(np.float32)
                continue

            activity, box, mask = motion_stats(prev, small, args.threshold)
            prev = small.astype(np.float32)
            if box is not None:
                moving_frames += 1
                print(f"[{time.monotonic() - start:6.2f}s] frame {index:4d} "
                      f"activity={activity:6.2f} motion bbox=x{box[0] * step},"
                      f"y{box[1] * step} {box[2] * step}x{box[3] * step}")
            elif index % 15 == 0:
                print(f"[{time.monotonic() - start:6.2f}s] frame {index:4d} "
                      f"activity={activity:6.2f} (idle)")

            if time.monotonic() - start > args.seconds:
                break

    print(f"\nmotion in {moving_frames} frames")
    print(f"{recorder.stats()}")
    recorder.close()
    wcs.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
