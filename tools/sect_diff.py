"""Compare two PE files section by section.

    python -X utf8 tools/sect_diff.py <a.dll> <b.dll>

Linker output is never bit-reproducible: LLD writes a randomised PE TimeDateStamp
and build-ID into every link, and it embeds the output file name, so a rebuild in a
different folder (or under a different name) also shifts the string table.  What has
to match for a rebuild to be "the same code" is the instruction section.  Exit status
is 0 when .text is identical, 1 otherwise, so this can be used as a gate.

Measured against the shipped files (see README, "Verifying that a build matches the
release"): the core built as unepic_mod_core_new.dll gives .text and .rdata identical
(exit 0); the loader gives .text 29 / .rdata 388 because of the embedded path string,
so exit 1 there is expected and is not a code difference.
"""
import struct
import sys


def sections(path):
    d = open(path, "rb").read()
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    if d[pe:pe + 4] != b"PE\0\0":
        raise SystemExit("%s: not a PE file" % path)
    nsec = struct.unpack_from("<H", d, pe + 6)[0]
    optsize = struct.unpack_from("<H", d, pe + 20)[0]
    out = {}
    for i in range(nsec):
        o = pe + 24 + optsize + i * 40
        name = d[o:o + 8].rstrip(b"\x00").decode(errors="replace")
        vsz, va, rawsz, raw = struct.unpack_from("<IIII", d, o + 8)
        out[name] = (raw, rawsz)
    return d, out


def main(a, b):
    da, sa = sections(a)
    db, sb = sections(b)
    hdr = sum(1 for i in range(min(0x400, len(da), len(db))) if da[i] != db[i])
    print("%-34s %s" % (a.split("\\")[-1], len(da)))
    print("%-34s %s" % (b.split("\\")[-1], len(db)))
    print("  %-12s %8s %8s %8s  %s" % ("section", "len A", "len B", "differ", "note"))
    for name in sorted(set(sa) | set(sb)):
        if name not in sa or name not in sb:
            print("  %-12s %8s %8s %8s  %s" % (name, "present" if name in sa else "missing",
                                               "present" if name in sb else "missing", "-",
                                               "section set differs"))
            continue
        ra, wa = sa[name]
        rb, wb = sb[name]
        x, y = da[ra:ra + wa], db[rb:rb + wb]
        n = min(len(x), len(y))
        diff = sum(1 for i in range(n) if x[i] != y[i]) + abs(len(x) - len(y))
        note = ""
        if name == ".text":
            note = "instructions"
        elif name in (".buildid",):
            note = "expected to differ (randomised GUID)"
        elif name in (".rdata", ".data") and diff:
            note = "string/data layout; check whether it is only pointer shifts"
        print("  %-12s %8d %8d %8d  %s" % (name, len(x), len(y), diff, note))
    print("  %-12s %8s %8s %8d  PE header (TimeDateStamp lives here)" % ("header", "-", "-", hdr))
    same_text = (".text" in sa and ".text" in sb
                 and da[sa[".text"][0]:sa[".text"][0] + sa[".text"][1]]
                 == db[sb[".text"][0]:sb[".text"][0] + sb[".text"][1]])
    print("  .text identical:", "YES" if same_text else "NO")
    return 0 if same_text else 1


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    sys.exit(main(sys.argv[1], sys.argv[2]))
