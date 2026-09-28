"""Reference implementation of the test pattern painted by
``tests/testclient.c`` (see ``wsc_test_pattern`` there).

Keeping the formula in two languages is deliberate: if the recorder ever
mangles bytes, strides, or row order, the integration tests notice because
the captured array no longer matches this one.
"""

from __future__ import annotations

import numpy as np


def pattern_frame(width: int, height: int, seq: int = 0) -> np.ndarray:
    """Return the RGB image (H, W, 3) uint8 that testclient draws for ``seq``."""
    x = np.arange(width, dtype=np.int64)[None, :]
    y = np.arange(height, dtype=np.int64)[:, None]
    s = int(seq)

    r = (x * 3 + y * 5 + s * 11) & 0xFF
    g = (x ^ y ^ ((s * 7) & 0xFF)) & 0xFF
    b = (x * 7 + y * 2 + s * 3) & 0xFF

    img = np.empty((height, width, 3), dtype=np.uint8)
    img[..., 0] = r
    img[..., 1] = g
    img[..., 2] = b
    return img
