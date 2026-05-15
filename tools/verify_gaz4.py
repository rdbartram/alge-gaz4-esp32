#!/usr/bin/env python3
"""
Verify the C++ GAZ4 frame builder against the Python reference in alge_match.py.

We can't run the C++ in this environment, so we re-implement build_match_frame
exactly as gaz4.cpp does, then check it produces byte-identical output to the
Python ground truth across a battery of test vectors.

Run:
    python3 tools/verify_gaz4.py
"""
from __future__ import annotations
import sys
from pathlib import Path

# --- Python reference ----------------------------------------------------
def py_build_frame(home: int, gast: int, total_sec: int) -> bytes:
    """From alge_match.py — empirically validated against the real board."""
    mm = (total_sec // 60) % 100
    ss = total_sec % 60
    h = str(home % 10)
    g = str(gast % 10)
    m1, m2 = str(mm // 10), str(mm % 10)
    s1, s2 = str(ss // 10), str(ss % 10)
    return f"000 {h} {g}    {m1}{m2} {s1}{s2}\r".encode("latin-1")


# --- C++ logic translated to Python --------------------------------------
def cpp_build_match_frame(home: int, away: int, total_seconds: int) -> bytes:
    """Mirror of gaz4::build_match_frame in alge-wallbox/src/gaz4.cpp."""
    if total_seconds > 5999:
        total_seconds = 5999
    mm = (total_seconds // 60) % 100
    ss = total_seconds % 60
    home %= 10
    away %= 10
    out = bytearray(17)
    out[0]  = ord("0")
    out[1]  = ord("0")
    out[2]  = ord("0")
    out[3]  = ord(" ")
    out[4]  = ord("0") + home
    out[5]  = ord(" ")
    out[6]  = ord("0") + away
    out[7]  = ord(" ")
    out[8]  = ord(" ")
    out[9]  = ord(" ")
    out[10] = ord(" ")
    out[11] = ord("0") + (mm // 10)
    out[12] = ord("0") + (mm % 10)
    out[13] = ord(" ")
    out[14] = ord("0") + (ss // 10)
    out[15] = ord("0") + (ss % 10)
    out[16] = 0x0D
    return bytes(out)


def cpp_build_blank_frame() -> bytes:
    out = bytearray(b" " * 17)
    out[0]  = ord("0")
    out[1]  = ord("0")
    out[2]  = ord("0")
    out[16] = 0x0D
    return bytes(out)


# --- Test vectors --------------------------------------------------------
VECTORS = [
    (0, 0, 0),
    (1, 0, 60),
    (3, 1, 23 * 60 + 45),
    (5, 2, 10 * 60 + 23),
    (9, 9, 99 * 60 + 59),
    (10, 11, 45 * 60 + 0),   # wrap above 9 — board shows 0/1
    (8, 8, 88 * 60 + 88),    # clamp & wrap stress test
    (0, 0, 6000),            # over 99:59 — clamp
]

def main() -> int:
    fails = 0
    for h, g, t in VECTORS:
        py = py_build_frame(h, g, t)
        cpp = cpp_build_match_frame(h, g, t)
        # Python reference doesn't clamp; align for high totals.
        py_t = min(t, 5999)
        py = py_build_frame(h, g, py_t)
        ok = py == cpp
        print(f"  h={h:3d} g={g:3d} t={t:5d}  py={py!r}  cpp={cpp!r}  {'OK' if ok else 'FAIL'}")
        if not ok:
            fails += 1

    blank = cpp_build_blank_frame()
    expected_blank = b"000             \r"
    assert len(blank) == 17 and len(expected_blank) == 17
    print(f"\n  blank  expected={expected_blank!r}")
    print(f"         got     ={blank!r}  {'OK' if blank == expected_blank else 'FAIL'}")
    if blank != expected_blank:
        fails += 1

    if fails:
        print(f"\n{fails} mismatch(es)")
        return 1
    print("\nAll GAZ4 frame vectors match the Python reference.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
