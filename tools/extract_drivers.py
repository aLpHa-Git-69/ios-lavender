#!/usr/bin/env python3
"""Mirror the driver sources that live inside the XNU patch series.

The build only ever sees the patched XNU tree, so the canonical copy of a
driver is the patch in xnu/patches/. That makes drivers/ look empty even when
a driver exists. This tool reconstructs each in-tree file from the patches and
stores a read-only mirror under drivers/<domain>/, so the code can be read,
grepped and reviewed without checking out XNU.

    tools/extract_drivers.py            rewrite the mirrors
    tools/extract_drivers.py --check    fail if a mirror is stale

Never edit a mirror: change the patch and re-run the tool. tests/run_tests.sh
runs --check so a patch and its mirror cannot drift apart.
"""

import argparse
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PATCH_DIR = ROOT / "xnu" / "patches"

# domain -> in-tree files that make up that driver
DRIVERS = {
    "storage": ["bsd/dev/santoni_sdhci.c"],
    "display": [
        "iokit/Kernel/arm/SweetFramebuffer.cpp",
        "iokit/Kernel/arm/SweetSurface.cpp",
    ],
    "touch": ["bsd/dev/sweet_touch.c"],
    "wifi": ["iokit/Kernel/arm/TopazModem.cpp"],
    "power": [
        "iokit/Kernel/arm/SweetBattery.cpp",
        "bsd/dev/sweet_i2c1.c",
    ],
}


def patches():
    found = sorted(PATCH_DIR.glob("*.patch"))
    if not found:
        sys.exit(f"no patches under {PATCH_DIR}")
    return found


def extract(path, work, series):
    """Reconstruct one in-tree file by applying every patch that touches it."""
    work.mkdir(parents=True, exist_ok=True)
    # A caller's TMPDIR may live under another Git checkout. Keep patch
    # paths rooted in this fixture instead of inheriting that parent prefix.
    if not (work / ".git").is_dir():
        subprocess.run(["git", "init", "-q", str(work)], check=True)
    cmd = [
        "git", "apply",
        f"--include={path}",
        "--whitespace=nowarn",
    ] + [str(p) for p in series]
    proc = subprocess.run(cmd, cwd=work, capture_output=True, text=True)
    if proc.returncode != 0:
        print(proc.stderr.strip(), file=sys.stderr)
        sys.exit(f"git apply failed for {path}")
    out = work / path
    if not out.exists():
        sys.exit(f"{path} did not appear after the patch series")
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--check", action="store_true",
                    help="only compare; exit 1 if a mirror is stale")
    args = ap.parse_args()
    series = patches()
    stale = []
    tmp = Path(tempfile.mkdtemp(prefix="drv-"))
    try:
        for domain, files in DRIVERS.items():
            dest_dir = ROOT / "drivers" / domain
            dest_dir.mkdir(parents=True, exist_ok=True)
            for path in files:
                src = extract(path, tmp / domain, series)
                dest = dest_dir / Path(path).name
                if args.check:
                    if not dest.exists() or dest.read_bytes() != src.read_bytes():
                        stale.append(str(dest.relative_to(ROOT)))
                    continue
                shutil.copyfile(src, dest)
                print(f"{dest.relative_to(ROOT)} <- {path}")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    if args.check:
        if stale:
            print("stale driver mirrors (run tools/extract_drivers.py):",
                  file=sys.stderr)
            for s in stale:
                print(f"  {s}", file=sys.stderr)
            return 1
        print("ok   drivers/ mirrors match the patch series")
    return 0


if __name__ == "__main__":
    sys.exit(main())
