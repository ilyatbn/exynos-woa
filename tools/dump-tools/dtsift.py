# SPDX-License-Identifier: BSD-2-Clause-Patent
"""dtsift — query a decompiled device tree for CPU-driver facts.

Commands (all take one or more .dtb files):
  decompile DTB [-o OUT.dts]     full DTS text, phandles annotated
  cpus DTB                       per-core: MPIDR, compatible, clocks,
                                 opp-table, power-domain, cpu-supply
  opp DTB                        every operating-points-v2 table:
                                 consumers + freq/uV pairs
  clocks DTB                     clock-controller nodes: compatible + reg
  nodes DTB [--match RE]         node paths + reg windows
  phandle DTB N                  resolve a phandle to its node path

Used by notes/cpu-driver-9820.md; every number it prints is a read from
the dump's own DTB, not from a source tree.
"""

from __future__ import annotations

import argparse
import re
import struct
import sys

import fdt


def load(path: str) -> fdt.Fdt:
    with open(path, "rb") as f:
        blob = f.read()
    blobs = fdt.split_blobs(blob)
    if not blobs:
        raise SystemExit(f"{path}: no FDT magic found")
    if len(blobs) > 1:
        print(f"# {path}: {len(blobs)} concatenated FDTs, using the first",
              file=sys.stderr)
    return fdt.parse(blobs[0])


def cells(node: fdt.Node, name: str) -> list[int]:
    return node.prop_cells(name) or []


def resolve(tree: fdt.Fdt, ph: int) -> str:
    n = tree.by_phandle.get(ph)
    return n.path if n else f"<dangling {ph:#x}>"


def cmd_decompile(a):
    tree = load(a.dtb)
    text = fdt.to_dts(tree)
    if a.o:
        with open(a.o, "w") as f:
            f.write(text)
        print(f"wrote {a.o} ({len(text)} B)")
    else:
        print(text)


def opp_table_of(tree: fdt.Fdt, cpu: fdt.Node):
    phs = cells(cpu, "operating-points-v2")
    if not phs:
        return None
    return tree.by_phandle.get(phs[0])


def cmd_cpus(a):
    tree = load(a.dtb)
    for cpu in sorted(tree.find_all(name=None, compatible=None),
                      key=lambda n: n.path):
        if not re.match(r"^cpu@[0-9a-f]+$", cpu.name):
            continue
        reg = cells(cpu, "reg")
        mpidr = (reg[0] << 32 | reg[1]) if len(reg) >= 2 else (reg[0] if reg else 0)
        comp = cpu.prop_str("compatible")
        clk = cells(cpu, "clocks")
        # Samsung clocks = <&controller clock-spec...>: only cell 0 is a phandle
        clocks = (resolve(tree, clk[0]) + " spec=" +
                  " ".join(f"{c:#x}" for c in clk[1:])) if clk else "-"
        pd = " ".join(resolve(tree, p) for p in cells(cpu, "power-domain"))
        supply = " ".join(resolve(tree, p) for p in cells(cpu, "cpu-supply"))
        opp = opp_table_of(tree, cpu)
        print(f"{cpu.path}")
        print(f"    MPIDR            {mpidr:#x}")
        print(f"    compatible       {comp}")
        print(f"    enable-method    {cpu.prop_str('enable-method')}")
        print(f"    clocks           {clocks}")
        print(f"    power-domain     {pd or '-'}")
        print(f"    cpu-supply       {supply or '-'}")
        print(f"    opp-table        {opp.path if opp else '-'}")
        idle = cells(cpu, "idle-states")
        if idle:
            print(f"    idle-states      " +
                  " ".join(resolve(tree, p) for p in idle))


def cmd_opp(a):
    tree = load(a.dtb)
    # consumers per opp table
    consumers: dict[int, list[str]] = {}
    for n in tree.root.walk():
        for pname, val in n.props.items():
            if len(val) % 4:
                continue
            for ph in struct.unpack(f">{len(val) // 4}I", val):
                t = tree.by_phandle.get(ph)
                if t and n is not t and any(
                        c.name.startswith("opp-") for c in t.children):
                    consumers.setdefault(ph, []).append(n.path)
    tables = sorted(consumers)
    for ph in tables:
        t = tree.by_phandle[ph]
        print(f"\n== opp-table {t.path} (phandle {ph:#x})")
        print(f"   consumers: {', '.join(sorted(set(consumers[ph])))}")
        for c in t.children:
            if not c.name.startswith("opp-"):
                continue
            hz = cells(c, "opp-hz")
            uv = cells(c, "opp-microvolt")
            hzv = (hz[0] << 32 | hz[1]) if len(hz) >= 2 else (hz[0] if hz else 0)
            micro = " ".join(f"{v}uV" for v in uv) if uv else "-"
            support = c.prop_str("opp-supported-hw")
            tag = f" [supported-hw {support.hex()}]" if support else ""
            print(f"   {c.name:<24} {hzv:>10} Hz   {micro}{tag}")


def cmd_clocks(a):
    tree = load(a.dtb)
    for n in tree.root.walk():
        comp = n.props.get("compatible", b"")
        if b"clock" in comp and b"controller" in comp:
            reg = cells(n, "reg")
            win = ", ".join(f"{reg[i]:#x}+{reg[i + 1]:#x}"
                            for i in range(0, len(reg) - 1, 2))
            print(f"{n.path}\n    compatible {n.prop_str('compatible')}"
                  f"\n    reg {win}")


def cmd_nodes(a):
    tree = load(a.dtb)
    rx = re.compile(a.match)
    for n in tree.root.walk():
        if rx.search(n.name) or rx.search(n.prop_str("compatible") or ""):
            reg = cells(n, "reg")
            win = ", ".join(f"{reg[i]:#x}+{reg[i + 1]:#x}"
                            for i in range(0, len(reg) - 1, 2))
            print(f"{n.path}" + (f"   reg {win}" if win else ""))


def cmd_phandle(a):
    tree = load(a.dtb)
    n = tree.by_phandle.get(int(a.n, 0))
    print(n.path if n else "not found")


def main():
    ap = argparse.ArgumentParser(prog="dtsift")
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name, fn in (("decompile", cmd_decompile), ("cpus", cmd_cpus),
                     ("opp", cmd_opp), ("clocks", cmd_clocks),
                     ("nodes", cmd_nodes)):
        s = sub.add_parser(name)
        s.add_argument("dtb")
        if name == "decompile":
            s.add_argument("-o")
        if name == "nodes":
            s.add_argument("--match", default=".")
    s = sub.add_parser("phandle")
    s.add_argument("dtb")
    s.add_argument("n")
    a = ap.parse_args()
    {"decompile": cmd_decompile, "cpus": cmd_cpus, "opp": cmd_opp,
     "clocks": cmd_clocks, "nodes": cmd_nodes, "phandle": cmd_phandle}[a.cmd](a)


if __name__ == "__main__":
    main()
