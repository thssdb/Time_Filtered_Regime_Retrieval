"""Encode chronological sample times with a segment start and a two-bit pattern ID."""
import argparse
import struct
from pathlib import Path

import numpy as np


def pack_rows(starts, keys):
    tags = bytearray((len(keys) + 3) // 4)
    for i, key in enumerate(keys):
        tags[i // 4] |= key << (2 * (i % 4))
    return np.asarray(starts, dtype="<i8").tobytes() + tags


def subgroups(patterns):
    begin, table, keys = 0, {}, []
    for i, pattern in enumerate(patterns):
        if pattern not in table and len(table) == 4:
            yield begin, table, keys
            begin, table, keys = i, {}, []
        keys.append(table.setdefault(pattern, len(table)))
    yield begin, table, keys


def write_pages(times, blocks, directory):
    """The file header is copied once into the first time chunk of each output file."""
    samples = times.shape[1]
    if not 1 <= samples <= 255:
        raise ValueError("sample count outside format")
    offsets = (times[:, 1:] - times[:, :1]).astype("<i8")
    patterns = [row.tobytes() for row in offsets]
    distinct = {key: i for i, key in enumerate(dict.fromkeys(patterns))}
    shared = distinct if len(distinct) <= 4 else {}
    header = struct.pack("<4sBBBB", b"TIM1", samples, len(shared), 2, 0)
    ref_bytes = max(1, ((len(distinct)-1).bit_length()+7)//8)
    if not shared:
        header += struct.pack("<IB", len(distinct), ref_bytes)
    header += b"".join(distinct)
    directory.mkdir(parents=True, exist_ok=True)
    (directory / "time.header").write_bytes(header)
    with (directory / "time.pages").open("wb") as stream:
        for ids in blocks:
            n = len(ids)
            if not 0 < n <= 128:
                raise ValueError("invalid block size")
            starts = times[ids, 0]
            selected = [patterns[i] for i in ids]
            body = bytearray((n,))
            if shared:
                body.extend(pack_rows(starts, [shared[p] for p in selected]))
            else:
                for begin, table, keys in subgroups(selected):
                    body.extend((len(keys), len(table)))
                    for pattern in table:
                        body.extend(distinct[pattern].to_bytes(ref_bytes, "little"))
                    body.extend(pack_rows(starts[begin:begin + len(keys)], keys))
            stream.write(struct.pack(">I", len(body)))
            stream.write(body)
    return len(distinct)


def from_locator(root, db, dest):
    times = np.memmap(root / "prepared" / db / "point_time_ns.i64", "<i8", "r").reshape(-1, 18)
    ids = np.fromfile(root / "time" / db / "time_ids.u32", "<u4")
    loc = np.fromfile(root / "stores" / db / "sim_common" / "address.bin", "<u4", offset=24)
    if len(ids) != len(times) or len(ids) != len(loc):
        raise ValueError("inconsistent record counts")
    slots = loc >> 7
    order = np.argsort(slots)
    slots = slots[order]
    split = np.flatnonzero(np.diff(slots >> 7)) + 1
    blocks = np.split(ids[order], split)
    for number, group in enumerate(np.split(slots, split)):
        if not np.array_equal(group, number * 128 + np.arange(len(group))):
            raise ValueError("invalid chronological locator")
    return write_pages(times, blocks, dest)


def varint(number):
    out = bytearray()
    while number >= 128:
        out.append(128 | (number & 127))
        number >>= 7
    out.append(number)
    return out


def read_varint(stream):
    result = 0
    for shift in range(0, 35, 7):
        value = stream.read(1)[0]
        result |= (value & 127) << shift
        if value < 128:
            return result
    raise ValueError("invalid chunk length")


def decode_group(stream, n, width, table):
    starts = np.frombuffer(stream.read(8 * n), "<i8")
    tags = stream.read((n + 3) // 4)
    keys = [(tags[i // 4] >> (2 * (i % 4))) & 3 for i in range(n)]
    out = np.empty((n, width), dtype="<i8")
    out[:, 0] = starts
    out[:, 1:] = starts[:, None] + table[keys]
    return out


def seal_column(times, blocks, output):
    """Write standalone time chunks using the same framing as the aligned store and read them back."""
    from tempfile import TemporaryDirectory
    from io import BytesIO
    with TemporaryDirectory() as tmp:
        work = Path(tmp)
        distinct = write_pages(times, blocks, work)
        with (work / "time.pages").open("rb") as pages, output.open("wb") as dst:
            for i in range(len(blocks)):
                length, = struct.unpack(">I", pages.read(4))
                body = ((work / "time.header").read_bytes() if i == 0 else b"") + pages.read(length)
                page = varint(len(body)) * 2 + body
                dst.write(bytes((0x85, 0)) + varint(len(page)) + bytes((6, 0, 254)) + page)
            if pages.read(1):
                raise ValueError("extra page")
    samples = times.shape[1]
    def table(stream, count):
        return np.frombuffer(stream.read(count * (samples - 1) * 8), "<i8").reshape(count, samples - 1)
    with output.open("rb") as src:
        for i, ids in enumerate(blocks):
            if src.read(2) != bytes((0x85, 0)):
                raise ValueError("chunk marker")
            length = read_varint(src)
            if src.read(3) != bytes((6, 0, 254)):
                raise ValueError("encoding")
            page = BytesIO(src.read(length))
            n = read_varint(page)
            if n != read_varint(page):
                raise ValueError("compressed size")
            body = BytesIO(page.read(n))
            if page.read(1) or body.getbuffer().nbytes != n:
                raise ValueError("page length")
            if i == 0:
                magic, j, count, bits, flags = struct.unpack("<4sBBBB", body.read(8))
                if (magic,j,bits,flags) != (b"TIM1",samples,2,0) or count > 4:
                    raise ValueError("invalid template dictionary")
                if count:
                    shared = table(body, count)
                else:
                    size, ref_bytes = struct.unpack("<IB", body.read(5))
                    if size <= 4 or not 1 <= ref_bytes <= 4:
                        raise ValueError("invalid template pool")
                    pool = table(body, size)
                    shared = []
            rows = body.read(1)[0]
            if rows != len(ids):
                raise ValueError("row count")
            if len(shared):
                decoded = decode_group(body, rows, samples, shared)
            else:
                groups = []
                remaining = rows
                while remaining:
                    count, ntemplates = body.read(2)
                    if not 0 < count <= remaining or not 1 <= ntemplates <= 4:
                        raise ValueError("invalid template group")
                    refs = [int.from_bytes(body.read(ref_bytes), "little") for _ in range(ntemplates)]
                    groups.append(decode_group(body, count, samples, pool[refs]))
                    remaining -= count
                decoded = np.concatenate(groups)
            if body.read(1) or not np.array_equal(decoded, times[ids]):
                raise ValueError("timestamp readback differs")
        if src.read(1):
            raise ValueError("extra chunk")
    return distinct


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path)
    parser.add_argument("db")
    parser.add_argument("dest", type=Path)
    opts = parser.parse_args()
    print("distinct templates:", from_locator(opts.root, opts.db, opts.dest))
