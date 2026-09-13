"""Dump the dynamic symbol table (.dynsym) of an ELF64 file — no NDK tools needed.

Usage: python dynsym.py <lib.so> [name-substring]
Prints exported (defined in this file) and undefined symbols.
"""
import sys
import struct


def dump(path, filt=None):
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] != b"\x7fELF":
        print("not an ELF file")
        return
    is64 = data[4] == 2
    if not is64:
        print("only ELF64 supported")
        return
    (e_shoff,) = struct.unpack_from("<Q", data, 0x28)
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", data, 0x3A)

    def shdr(i):
        off = e_shoff + i * e_shentsize
        name, typ, flags, addr, offset, size, link, info, align, entsize = \
            struct.unpack_from("<IIQQQQIIQQ", data, off)
        return dict(name=name, type=typ, offset=offset, size=size,
                    link=link, info=info, entsize=entsize)

    shstr = shdr(e_shstrndx)
    shstr_base = shstr["offset"]

    def shname(n):
        end = data.index(b"\x00", shstr_base + n)
        return data[shstr_base + n:end].decode("utf-8", "replace")

    sections = []
    for i in range(e_shnum):
        s = shdr(i)
        s["sname"] = shname(s["name"])
        sections.append(s)

    symtab = None
    for s in sections:
        if s["type"] == 11 and s["sname"] == ".dynsym":  # SHT_DYNSYM
            symtab = s
            break
    if symtab is None:
        print("no .dynsym")
        return

    strtab = sections[symtab["link"]]
    strtab_base = strtab["offset"]
    # Number of local symbols = sh_info; the rest are global.
    n_local = symtab["info"]
    n_syms = symtab["size"] // symtab["entsize"]

    def sym_name(idx):
        (st_name,) = struct.unpack_from("<I", data,
                                       symtab["offset"] + idx * symtab["entsize"])
        end = data.index(b"\x00", strtab_base + st_name)
        return data[strtab_base + st_name:end].decode("utf-8", "replace")

    print(f"== {path}  (.dynsym: {n_syms} entries, {n_local} local) ==")
    hits = 0
    for i in range(n_syms):
        off = symtab["offset"] + i * symtab["entsize"]
        st_name, st_info, st_other, st_shndx, st_value, st_size = \
            struct.unpack_from("<IBBHQQ", data, off)
        name = sym_name(i)
        if filt and filt.lower() not in name.lower():
            continue
        hits += 1
        if st_shndx == 0:
            kind = "UNDEF (needed from another lib)"
        elif st_shndx == 0xFFF1:
            kind = "ABS"
        else:
            kind = "EXPORTED"
        bind = {0: "LOCAL", 1: "GLOBAL", 2: "WEAK"}.get(st_info >> 4, "?")
        print(f"  {kind:<32} {bind:<6} {name}")
    print(f"-- {hits} matching symbols --")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    dump(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else None)
