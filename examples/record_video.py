#!/usr/bin/env python3
"""Record the screen to a video file, with per-frame processing hooks.

    python3 examples/record_video.py [out.mp4] [--seconds 10] [--fps 30]

Shows the three ways to consume frames:

1. ``Recorder.record()``  - straight to an encoded file (uses ffmpeg).
2. ``on_frame=``          - process each frame on its way into the file.
3. ``for frame in rec.frames()`` - you own the loop (and the writer).

Every frame is an ``(H, W, 3)`` uint8 RGB array, so you can drop in a model
inference, an overlay, a region mask, whatever you like.
"""

from __future__ import annotations

import argparse
import time

import numpy as np

import wl_screencopy as wcs


def annotate(frame: np.ndarray, index: int) -> np.ndarray:
    """In-place demo: mark a corner and print a checksum every 60 frames.

    Frames are reused by the recorder only when ``out=`` is passed, but
    mutating them in place is still the right way to burn in an overlay.
    """
    if index % 60 == 0:
        print(f"  frame {index:5d} checksum={int(frame.sum())}")
    # Draw a small green square that moves, to prove the pipeline is live.
    t = index % 100
    h, w = frame.shape[:2]
    x = int((t / 100) * (w - 40))
    frame[10:40, 10 + x:40 + x] = (0, 255, 0)
    return frame


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("path", nargs="?", default="recording.mp4")
    ap.add_argument("--seconds", type=float, default=10.0)
    ap.add_argument("--fps", type=float, default=30.0)
    ap.add_argument("--output", default=None, help="output monitor name/index")
    ap.add_argument("--cursor", action="store_true", help="grab the pointer too")
    ap.add_argument("--damage", action="store_true",
                    help="only capture when the screen changes (saves CPU)")
    ap.add_argument("--codec", default="auto")
    args = ap.parse_args()

    if not wcs.supports_screencopy():
        print("no wlr-screencopy here; run inside a wlroots compositor",
              file=__import__("sys").stderr)
        return 1

    recorder = wcs.Recorder(
        output=args.output,
        fps=args.fps,
        cursor=args.cursor,
        wait_for_damage=args.damage,
        buffers=3,           # rotate shm buffers: capture N+1 while N is read
        timeout=10.0,
    )

    with recorder:
        h, w, c = recorder.frame_shape
        print(f"recording {w}x{h} @ {args.fps:g} fps -> {args.path}")
        print(f"encoder: {args.codec} (auto picks NVENC when available, else "
              f"libx264)")
        stats = recorder.record(
            args.path,
            duration=args.seconds,
            codec=args.codec,
            on_frame=lambda frame, index: annotate(frame, index),
        )
        # Read the native counters inside the with-block: leaving it closes
        # the Session the recorder owns.
        print(f"\n{stats}")
        print(f"native stats: {recorder.session.stats()}")

    # --- 3. fully manual loop -------------------------------------------
    # (the recorder from above is closed now; a second one opens its own
    #  Wayland connection)
    manual = args.path.replace(".mp4", "-manual.mp4")
    print(f"\nmanual loop -> {manual}")
    with wcs.Recorder(fps=args.fps, output=args.output, buffers=3) as rec:
        h, w, c = rec.frame_shape
        scratch = np.empty((h, w, c), dtype=np.uint8)   # allocated once
        writer = wcs.VideoWriter(manual, w, h, fps=args.fps)
        t0 = time.monotonic()
        with writer:
            for index, frame in enumerate(rec.frames(out=scratch)):
                writer.write(frame)
                if time.monotonic() - t0 > min(args.seconds, 5.0):
                    break
        print(f"wrote {writer.frames_written} frames, "
              f"final stats: {rec.stats()}")

    wcs.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
