# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Round-trip test: build a synthetic FDT shaped like the 9820 cpus/opp
nodes, parse it with fdt.py, and check every query dtsift performs.

Run:  python3 test_fdt.py
"""

from __future__ import annotations

import struct

import fdt


def build() -> bytes:
    strings = bytearray()
    str_off: dict[str, int] = {}

    def s(name: str) -> int:
        if name not in str_off:
            str_off[name] = len(strings)
            strings.extend(name.encode() + b"\0")
        return str_off[name]

    struct_buf = bytearray()

    def tok(t: int):
        struct_buf.extend(struct.pack(">I", t))

    def node(name: str):
        tok(1)
        struct_buf.extend(name.encode() + b"\0")
        while len(struct_buf) % 4:
            struct_buf.extend(b"\0")

    def prop(name: str, val: bytes):
        tok(3)
        struct_buf.extend(struct.pack(">2I", len(val), s(name)))
        struct_buf.extend(val)
        while len(struct_buf) % 4:
            struct_buf.extend(b"\0")

    def cells(*vals: int):
        return struct.pack(f">{len(vals)}I", *vals)

    # phandles: 1=cmu_core0 clock ctrl, 2=pd_little, 3=opp_little, 4=vdd_buck0
    node("")
    prop("compatible", b"test\0")
    node("cpus")
    prop("#address-cells", cells(1))
    prop("#size-cells", cells(0))
    for i in range(4):
        node(f"cpu@{i:04d}")
        prop("device_type", b"cpu\0")
        prop("reg", cells(i))
        prop("compatible", b"arm,ananke\0arm,armv8\0")
        prop("enable-method", b"psci\0")
        prop("clocks", cells(1, 0x12))
        prop("power-domain", cells(2))
        prop("operating-points-v2", cells(3))
        prop("cpu-supply", cells(4))
        prop("phandle", cells(0x10 + i))
        node("idle-states")
        prop("entry-wfi", cells(0x80000000))
        tok(2)  # end idle-states
        tok(2)  # end cpu
    tok(2)  # end cpus
    node("clock-controller@10c20000")
    prop("compatible", b"samsung,exynos9820-clock-controller\0")
    prop("reg", cells(0x10C20000, 0x8000))
    prop("phandle", cells(1))
    tok(2)
    node("power-domain@10090100")
    prop("compatible", b"samsung,exynos9820-pd\0")
    prop("phandle", cells(2))
    tok(2)
    node("opp-table-little")
    prop("compatible", b"operating-points-v2\0")
    prop("phandle", cells(3))
    for f, v in ((0x3B9ACA00, 850000), (0x55AE0000, 900000)):
        node(f"opp-{f}")
        prop("opp-hz", cells(0, f))
        prop("opp-microvolt", cells(v, v, v))
        tok(2)
    tok(2)
    node("regulator@0")
    prop("regulator-name", b"vdd_arm_buck0\0")
    prop("phandle", cells(4))
    tok(2)
    tok(9)  # FDT_END

    # FDT header is 9 big-endian words = 36 B; then two 16 B reservation
    # entries (one real, one terminator)
    off_rsv = 36
    struct_off = off_rsv + 32
    strings_off = struct_off + len(struct_buf)
    total = strings_off + len(strings)
    header = struct.pack(">9I", fdt.FDT_MAGIC, total, struct_off, off_rsv,
                         17, 16, 0, len(strings), len(struct_buf))
    rsv = struct.pack(">QQ", 0x80000000, 0x10000) + struct.pack(">QQ", 0, 0)
    return header + rsv + bytes(struct_buf) + bytes(strings)


def main():
    blob = build()
    tree = fdt.parse(blob)

    cpus = [n for n in tree.root.walk() if n.name.startswith("cpu@")]
    assert tree.root.prop_str("compatible") == "test", tree.root.props
    assert len(tree.root.children) and tree.root.children[0].path == "/cpus"
    assert len(cpus) == 4, cpus
    assert cpus[0].prop_str("compatible", 0) == "arm,ananke"
    assert cpus[0].prop_cells("reg") == [0]
    assert cpus[3].prop_cells("reg") == [3]
    assert tree.by_phandle[1].path == "/clock-controller@10c20000"
    assert tree.by_phandle[3].path == "/opp-table-little"
    assert tree.reservations == [(0x80000000, 0x10000)]
    opp = tree.by_phandle[3]
    assert len([c for c in opp.children if c.name.startswith("opp-")]) == 2
    hz = opp.children[0].prop_cells("opp-hz")
    assert hz == [0, 0x3B9ACA00], hz

    dts = fdt.to_dts(tree)
    assert "cpu@0000" in dts and "&clock-controller@10c20000" in dts
    assert 'compatible = "arm,ananke", "arm,armv8"' in dts

    # split_blobs on a container of two
    assert len(fdt.split_blobs(blob + blob)) == 2

    print("test_fdt: all assertions passed "
          f"({len(blob)} B synthetic FDT, dts {len(dts)} B)")


if __name__ == "__main__":
    main()
