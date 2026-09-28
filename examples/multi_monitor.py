#!/usr/bin/env python3
"""Multi-monitor and region capture.

    python3 examples/multi_monitor.py

Grabs every output the compositor exposes, then crops a region out of the
first one, and shows how outputs are addressed (index or connector name).
"""

from __future__ import annotations

import numpy as np

import wl_screencopy as wcs


def main() -> int:
    session = wcs.open_session(buffers=2)
    try:
        if not session.supports_screencopy:  # property, like .backends
            print(f"unsupported: {session.unsupported_reason}")
            return 1

        outputs = session.outputs(refresh=True)
        print(f"{len(outputs)} output(s), backends={session.backends}")

        frames = {}
        for out in outputs:
            # Addressing by index or by name are equivalent:
            frame = session.capture(out.index, timeout=5.0)
            frames[out.name or str(out.index)] = frame
            print(f"  {out.index} {out.name:<10} logical {out.width}x{out.height}"
                  f" scale {out.scale} -> array {frame.shape} "
                  f"mean {frame.reshape(-1, 3).mean(axis=0).round(1)}")

        # Selecting by name (connector names come from xdg_output / wl_output):
        first = outputs[0].name or "0"
        by_name = session.capture(first, timeout=5.0)
        np.testing.assert_array_equal(by_name, frames[outputs[0].name or "0"])
        print(f"\nselecting by name '{first}' matches selecting by index 0")

        # A region is expressed in *logical* pixels; the array comes back in
        # device pixels (logical x scale), matching what you see on screen.
        out = outputs[0]
        scale = max(1, out.scale)
        x, y = 64, 64
        lw, lh = 320, 200
        region = session.capture(out.index, (x, y, lw, lh), timeout=5.0)
        full = frames[out.name or "0"]
        print(f"region logical {lw}x{lh} -> array {region.shape} "
              f"(device px = logical x scale {scale})")

        # With scale == 1 a region capture must equal the matching crop.
        if scale == 1:
            crop = full[y:y + lh, x:x + lw]
            diff = np.abs(crop.astype(int) - region.astype(int)).mean()
            print(f"  mean |crop - region| = {diff:.3f} "
                  f"({'identical' if diff == 0 else 'screen changed in between'})")

        # Composite a contact sheet so you can eyeball everything at once.
        thumbs = []
        for name, frame in frames.items():
            tw = 480
            th = max(1, int(frame.shape[0] * tw / frame.shape[1]))
            # Nearest neighbour resize with NumPy (no Pillow required).
            ys = (np.arange(th) * frame.shape[0] // th)
            xs = (np.arange(tw) * frame.shape[1] // tw)
            thumbs.append(frame[np.ix_(ys, xs)])
        sheet = np.concatenate(thumbs, axis=0) if thumbs else np.zeros((1, 1, 3))
        wcs.save_image(sheet, "contact_sheet.png")
        print(f"\nwrote contact_sheet.png {sheet.shape}")
        return 0
    finally:
        session.close()
        wcs.close()


if __name__ == "__main__":
    raise SystemExit(main())
