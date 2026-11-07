#!/usr/bin/env python3
"""Darwin 22 PE discovery contract, using encoded/parsed board ADTs.

pe_identify_machine.c matches interrupt-controller="master" and
device_type="timer"; pe_fiq.c also requires /arm-io/gic's four u64 reg fields.
"""
from pathlib import Path
import struct
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import adt
import mkadt


def walk(node):
    yield node
    for child in node.children:
        yield from walk(child)


def matching(tree, key, value):
    return [node for node in walk(tree) if any(k == key and v == value for k, f, v in node.props)]


with tempfile.TemporaryDirectory() as temporary:
    product = mkadt.node("product", [("product-name", b"fixture\0")])
    path = Path(temporary) / "product.adt"
    path.write_bytes(adt.serialize(mkadt.node("device-tree", children=[product])))
    for board in ("lavender", "whyred"):
        data = adt.serialize(mkadt.build(seed=bytes(64), native=True, board=board, product_adt=path))
        tree, used = adt.parse(data)
        assert used == len(data)
        soc = next(n for n in tree.children if n.name == "arm-io")
        bus = struct.unpack("<3Q", soc.prop("ranges"))
        assert bus[:2] == (0, 0x01000000)
        controllers = matching(tree, "interrupt-controller", b"master\0")
        assert len(controllers) == 1 and controllers[0].name == "gic"
        assert controllers[0] in soc.children
        registers = struct.unpack("<4Q", controllers[0].prop("reg"))
        assert (bus[1] + registers[0], registers[1], bus[1] + registers[2], registers[3]) == (
            0x17a00000, 0x10000, 0x17b00000, 0x100000)
        assert struct.unpack("<I", controllers[0].prop("virtual-timer-intid"))[0] == 19
        timers = matching(tree, "device_type", b"timer\0")
        assert len(timers) == 1 and timers[0] in soc.children
        frame, size = struct.unpack("<2Q", timers[0].prop("reg"))
        assert (bus[1] + frame, size) == (0x17920000, 0x1000)
        assert timers[0].prop("compatible") == b"arm,armv7-timer-mem\0"
        # The previous empty property is not what SecureDTFindEntry matches.
        controllers[0].props = [(k, f, b"" if k == "interrupt-controller" else v)
                                for k, f, v in controllers[0].props]
        assert not matching(tree, "interrupt-controller", b"master\0")
print("test_mkadt_interrupts: native PE discovery, GIC64/timer ranges and old-empty-property negative cases passed")
