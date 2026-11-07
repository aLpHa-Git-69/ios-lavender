#!/usr/bin/env python3
"""Compile controller logic from the patches, rather than a copied driver.

Only the platform/MMIO backend is supplied by the C mocks. git apply rebuilds
the real BSD source, including every later patch; malformed/skipped hunks are
fatal. No SDK, network, kernel build, driver-mirror write or device is needed.
--source-tree is for developing against the ignored, prepared XNU sandbox.
"""
import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FUNCTIONS = (
    "sdhci_set_block", "sdhci_v5_standard_fifo", "sdhci_poll_state",
    "sdhci_cmd_stamp", "sdhci_cmd", "sdhci_data_failure",
    "sdhci_wait_data_until", "sdhci_pio_deadline",
    "sdhci_wait_transfer_complete", "sdhci_stop_transmission",
    "sdhci_vendor_core_reset", "sdhci_enable_card_clock", "sdhci_reset_paths", "sdhci_recover",
    "sdhci_drain_read_fifo", "sdhci_prepare_pio", "sdhci_read_pio_once",
    "sdhci_probe_attempt", "sdhci_write_pio", "sdhci_core_check",
    "sdhci_ext_csd_sane", "sdhci_record_cache_state",
    "santoni_sdhci_write_blocks", "sdhci_synchronize_cache",
)


def function(source, name):
    # Blank comments/literals without changing offsets: braces in a diagnostic
    # string must not truncate the C function selected for compilation.
    masked = re.sub(
        r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
        lambda m: re.sub(r"[^\n]", " ", m.group()), source, flags=re.S,
    )
    pattern = re.compile(r"(?m)^static[^\n]*\n" + re.escape(name) + r"\(")
    matches = list(pattern.finditer(masked))
    if len(matches) != 1:
        raise ValueError(f"expected one definition of {name}, found {len(matches)}")
    start = matches[0].start()
    brace = masked.index("{", matches[0].end())
    if ";" in masked[matches[0].end():brace]:
        raise ValueError(f"{name}: found a prototype instead of a definition")
    depth = 1
    end = brace + 1
    while depth and end < len(masked):
        depth += (masked[end] == "{") - (masked[end] == "}")
        end += 1
    if depth:
        raise ValueError(f"{name}: unterminated body")
    return source[start:brace].rstrip() + ";", source[start:end]


def macro(source, name):
    match = re.search(r"(?m)^#define\s+" + re.escape(name) + r"\b[^\n]*", source)
    if not match:
        raise ValueError(f"missing macro {name}")
    return match.group()


def prepare(out, source_tree):
    out.mkdir(parents=True, exist_ok=True)
    if source_tree:
        source = (source_tree / "bsd/dev/santoni_sdhci.c").read_text()
        gic = (source_tree / "pexpert/arm/pe_fiq.c").read_text()
    else:
        with tempfile.TemporaryDirectory(prefix="sdhci-source-", dir=out) as temporary:
            tree = Path(temporary)
            # OUT may be inside the consuming checkout (as on Actions).
            # Give git apply its own repository root instead of inheriting the
            # parent checkout's prefix and silently filtering every patch path.
            subprocess.run(["git", "init", "-q", str(tree)], check=True)
            series = sorted((ROOT / "xnu/patches").glob("*.patch"))
            subprocess.run(
                ["git", "apply", "--include=bsd/dev/santoni_sdhci.c",
                 "--whitespace=nowarn", *map(str, series)], cwd=tree, check=True,
            )
            source = (tree / "bsd/dev/santoni_sdhci.c").read_text()
        sys.path.insert(0, str(ROOT / "tools"))
        from check_patch import parse
        files, errors = parse(ROOT / "xnu/patches/0190-lavender-gic-rwp-and-reset-code-clean.patch")
        if errors:
            raise ValueError("\n".join(errors))
        parts = [f for f in files if f.name == "pexpert/arm/pe_fiq.c"]
        if len(parts) != 1:
            raise ValueError("0190 must contain exactly one pe_fiq.c section")
        gic = "\n".join(parts[0].added)

    # Register constants and their board conditionals, without kernel-only
    # includes or allocation state. Continuation lines remain byte-for-byte.
    begin = source.index("/* ------------------------------------------------- SDHCI standard registers */")
    end = source.index("static struct {", begin)
    lines = source[begin:end].splitlines()
    definitions = []
    continued = False
    for line in lines:
        if line.startswith("#") or continued:
            definitions.append(line)
            continued = line.endswith("\\")
    definitions.extend(macro(source, name) for name in
                       ("SDHCI_PROBE_TIMEOUT_NS", "SANTONI_IO_RETRIES",
                        "SANTONI_IO_RETRY_DELAY_US"))
    (out / "sdhci_mock_regs.h").write_text(
        "/* Generated from the current XNU patch series. */\n" +
        "\n".join(definitions) + "\n"
    )
    selected = [function(source, name) for name in FUNCTIONS]
    (out / "sdhci_mock_impl.h").write_text(
        "/* Actual kernel functions; only MMIO/platform calls are mocked. */\n" +
        "\n".join(prototype for prototype, _ in selected) + "\n\n" +
        "\n\n".join(body for _, body in selected) + "\n"
    )
    _, body = function(gic, "gicd_wait_rwp")
    (out / "gic_wait_impl.h").write_text(body + "\n")
    print(f"ok   compiled mocks use {len(selected)} actual SDHCI functions and gicd_wait_rwp")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("out", type=Path)
    parser.add_argument("--source-tree", type=Path)
    args = parser.parse_args()
    prepare(args.out.resolve(), args.source_tree)


if __name__ == "__main__":
    main()
