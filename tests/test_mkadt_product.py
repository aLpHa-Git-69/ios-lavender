#!/usr/bin/env python3
"""Verify explicit Apple product-ADT selection without private Apple inputs."""
from pathlib import Path
import struct
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import adt
import mkadt


with tempfile.TemporaryDirectory() as temporary:
    product = adt.Node()
    product.props = [
        ("name", 0, b"product\0"),
        ("product-name", 0, b"selected-product-fixture\0"),
        ("artwork-scale-factor", 0, struct.pack("<I", 3)),
        ("AAPL,phandle", 0, struct.pack("<I", 99)),
        ("syscfg-fixture", 1, b"not-an-iBoot-value"),
    ]
    source = adt.Node()
    source.props = [("name", 0, b"device-tree\0")]
    source.children = [product]
    path = Path(temporary) / "selected.adt"
    path.write_bytes(adt.serialize(source))
    tree = mkadt.build(seed=bytes(64), native=True, board="lavender", product_adt=path)
    chosen = next(n for n in tree.children if n.name == "product")
    props = {key: value for key, flag, value in chosen.props}
    assert props["product-name"] == b"selected-product-fixture\0"
    assert props["artwork-scale-factor"] == struct.pack("<I", 3)
    assert "syscfg-fixture" not in props
    assert props["AAPL,phandle"] != struct.pack("<I", 99)
    try:
        mkadt.build(product_adt=Path(temporary) / "missing.adt")
    except FileNotFoundError:
        pass
    else:
        raise AssertionError("explicit missing product ADT was silently ignored")
print("test_mkadt_product: explicit product identity and missing-input cases passed")
