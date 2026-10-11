# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Pure-Python Flattened Device Tree parser + DTS emitter.

Written for the Android 12 ODIN dump research (notes/cpu-driver-9820.md):
the dump's boot.img/dtbo.img carry the authoritative CPU topology, OPP
tables, clock parents and regulator supplies, and the host has no dtc.
The format is the Devicetree Specification ePAPR FDT section; everything
here is a direct read of that layout, no external dependency.

API:
    tree = fdt.parse(blob)            -> Fdt with .root (Node)
    tree.find(path)                   -> Node or None
    tree.find_all(compatible="...")   -> [Node]
    tree.by_phandle[int]              -> Node
    fdt.to_dts(tree)                  -> str (phandle values annotated)
"""

from __future__ import annotations

import struct

FDT_MAGIC = 0xD00DFEED
BEGIN_NODE, END_NODE, PROP, NOP, END = 1, 2, 3, 4, 9
TOKENS = {BEGIN_NODE: "BEGIN_NODE", END_NODE: "END_NODE", PROP: "PROP",
          NOP: "NOP", END: "END"}


class CorruptFdt(Exception):
    pass


class Node:
    def __init__(self, path: str):
        self.path = path                      # "/", "/cpus/cpu@0000"
        self.props: dict[str, bytes] = {}     # insertion order kept
        self.children: list[Node] = []

    @property
    def name(self) -> str:
        return self.path.rsplit("/", 1)[-1] or "/"

    def prop_str(self, name: str, index: int = 0) -> str | None:
        v = self.props.get(name)
        if v is None:
            return None
        parts = v.split(b"\0")
        if index >= len(parts):
            return None
        return parts[index].decode("utf-8", "replace")

    def prop_cells(self, name: str) -> list[int] | None:
        v = self.props.get(name)
        if v is None or len(v) % 4:
            return None
        return list(struct.unpack(f">{len(v) // 4}I", v))

    def walk(self):
        yield self
        for c in self.children:
            yield from c.walk()


class Fdt:
    def __init__(self):
        self.root = Node("/")
        self.by_phandle: dict[int, Node] = {}
        self.reservations: list[tuple[int, int]] = []
        self.boot_cpuid = 0
        self.version = 17

    def find(self, path: str) -> Node | None:
        for n in self.root.walk():
            if n.path == path:
                return n
        return None

    def find_all(self, compatible: str | None = None,
                 name: str | None = None) -> list[Node]:
        out = []
        for n in self.root.walk():
            if compatible is not None:
                comp = n.props.get("compatible", b"")
                if compatible.encode() not in comp.split(b"\0"):
                    continue
            if name is not None and n.name != name:
                continue
            out.append(n)
        return out


def parse(blob: bytes) -> Fdt:
    if len(blob) < 40:
        raise CorruptFdt("short blob")
    (magic, totalsize, off_struct, off_rsv, version, last_comp, boot_cpu,
     size_strings, size_struct) = struct.unpack_from(">9I", blob, 0)
    if magic != FDT_MAGIC:
        raise CorruptFdt(f"bad magic {magic:#x}")
    if totalsize > len(blob):
        # tolerate a blob that is only the head of a larger container
        totalsize = len(blob)
    tree = Fdt()
    tree.version = version
    tree.boot_cpuid = boot_cpu

    # Samsung downstream dtc variant (seen in the 9820 dt.img, version 40):
    # size_dt_strings == 0 and the field at header word 3 is the STRINGS
    # offset, not a reservation map; the struct block runs to that offset.
    samsung_layout = (size_strings == 0 and off_rsv > off_struct
                      and blob[off_rsv:off_rsv + 12].split(b"\0")[0].isascii()
                      and 0x20 <= blob[off_rsv] < 0x7F)
    if samsung_layout:
        size_strings = totalsize - off_rsv
        end_at = off_rsv
    else:
        # reservation map: (u64 address, u64 size) until a zero-size entry
        p = off_rsv
        while p + 16 <= totalsize:
            addr, size = struct.unpack_from(">QQ", blob, p)
            if size == 0 and addr == 0:
                break
            tree.reservations.append((addr, size))
            p += 16
        end_at = off_struct + size_struct

    strings = blob[off_rsv:off_rsv + size_strings] if samsung_layout \
        else blob[off_struct + size_struct: off_struct + size_struct + size_strings]

    def sname(off: int) -> str:
        end = strings.find(b"\0", off)
        return strings[off:end].decode("utf-8", "replace")

    path_stack: list[str] = [""]
    node_stack: list[Node] = [tree.root]
    p = off_struct
    while p < end_at:
        (tok,) = struct.unpack_from(">I", blob, p)
        p += 4
        if tok == NOP:
            continue
        if tok == END:
            break
        if tok == BEGIN_NODE:
            z = blob.index(b"\0", p)
            name = blob[p:z].decode("utf-8", "replace")
            p = (z + 4) & ~3
            parent = node_stack[-1]
            if parent is tree.root and len(node_stack) == 1 and name == "":
                node = tree.root          # the root node IS tree.root
            else:
                path = parent.path + "/" + name if parent.path != "/" \
                    else "/" + name
                node = Node(path)
                parent.children.append(node)
            node_stack.append(node)
            path_stack.append(node.path)
        elif tok == END_NODE:
            node_stack.pop()
            path_stack.pop()
        elif tok == PROP:
            (length, nameoff) = struct.unpack_from(">2I", blob, p)
            p += 8
            data = blob[p:p + length]
            p += (length + 3) & ~3
            node_stack[-1].props[sname(nameoff)] = data
        else:
            raise CorruptFdt(f"unknown token {tok} at {p - 4:#x}")

    for n in tree.root.walk():
        ph = n.props.get("phandle")
        if ph is not None and len(ph) == 4:
            tree.by_phandle[struct.unpack(">I", ph)[0]] = n
    return tree


# ---------------------------------------------------------------- DTS output

def _is_string_data(v: bytes) -> bool:
    if not v:
        return False
    if not v.endswith(b"\0"):
        return False
    body = v[:-1]
    if b"\0" in body:  # embedded NULs mean it's cells, not a string
        return False
    return all(0x20 <= b < 0x7F or b in (0x09, 0x0A) for b in body)


def _quote(s: str) -> str:
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def _fmt_cells(cells: list[int], tree: Fdt, annotate: bool) -> str:
    out = []
    for c in cells:
        n = tree.by_phandle.get(c) if annotate else None
        if n is not None:
            out.append(f"&{n.name}  /* phandle {c:#x} -> {n.path} */")
        else:
            out.append(f"{c:#x}")
    return "<" + " ".join(out) + ">"


# properties whose first cells are genuinely node references; small integers
# in other properties (latencies, capacities) must NOT be phandle-annotated
REF_PROPS = {
    "clocks", "clock-parent", "cpu-supply", "power-domain", "operating-points-v2",
    "cpu-idle-states", "idle-states", "interrupt-parent", "interrupts",
    "phandle", "linux,phandle", "target", "target-phandle", "regulator",
    "samsung,pmu-shandle", "bts,parent", "sched-energy-data", "thermal-governor",
    "cpu", "device_node", "cooling-device", "power-domains", "iommus",
    "samsung,syscon-phandle", "samsung,pmic-regulator", "vdd-supply",
}


def to_dts(tree: Fdt) -> str:
    lines: list[str] = []

    def emit_node(node: Node, depth: int):
        ind = "\t" * depth
        if node.path != "/":
            lines.append(f"{ind}{node.name} {{")
        inner = "\t" * (depth + 1)
        for name, val in node.props.items():
            if not val:
                lines.append(f"{inner}{name};")
            elif name in ("compatible", "model") or (
                    _is_string_data(val) and len(val) < 200):
                parts = [p for p in val.split(b"\0") if p]
                lines.append(inner + name + " = " +
                             ", ".join(_quote(p.decode("utf-8", "replace"))
                                       for p in parts) + ";")
            elif len(val) % 4 == 0 and (len(val) > 4 or name not in
                                        ("status",)):
                cells = struct.unpack(f">{len(val) // 4}I", val)
                lines.append(inner + name + " = " +
                             _fmt_cells(list(cells), tree,
                                        name in REF_PROPS) + ";")
            else:
                lines.append(inner + name + " = [" +
                             " ".join(f"{b:02x}" for b in val) + "];")
        for c in node.children:
            emit_node(c, depth + (0 if node.path == "/" else 1))
        if node.path != "/":
            lines.append(ind + "};")

    emit_node(tree.root, 0)
    return "\n".join(lines) + "\n"


def split_blobs(blob: bytes) -> list[bytes]:
    """Split a container (Samsung dt blob, DTBO entry) into FDT blobs by magic."""
    out, i = [], 0
    while True:
        j = blob.find(b"\xd0\x0d\xfe\xed", i)
        if j < 0:
            break
        (size,) = struct.unpack_from(">I", blob, j + 4)
        if size < 40 or j + size > len(blob):
            break
        out.append(blob[j:j + size])
        i = j + size
    return out
