"""Build the First-32 value layout for public datasets."""
import argparse
import ctypes
import hashlib
import json
import struct
from pathlib import Path

import numpy as np


DATABASES = ("NASA_battery", "Hydraulic_system", "metropt3")


def integer_width(values, signed=True):
    lo, hi = int(np.min(values)), int(np.max(values))
    for width in (1, 2, 4, 8, 16):
        lower = -(1 << (8 * width - 1)) if signed else 0
        upper = 1 << (8 * width - int(signed))
        if lower <= lo and hi < upper:
            return width
    raise OverflowError((lo, hi))


def integer_bytes(values, width, signed=True):
    if width <= 8:
        code = "i" if signed else "u"
        return np.asarray(values, dtype=f"<{code}{width}").tobytes()
    return b"".join(int(value).to_bytes(width, "little", signed=signed)
                    for value in np.ravel(values))


def physical_layout(source):
    raw = np.fromfile(source / "raw54.f32", "<f4")
    columns = 4 if source.name == "Hydraulic_system" else 3
    raw = raw.reshape(-1, columns, 60)
    time = np.fromfile(source / "point_time_ns.i64", "<i8").reshape(-1, 60)
    business = np.fromfile(source / "business_order.u32", "<u4")
    tails = np.fromfile(source / "tail_offsets.u64", "<u8").astype(int)
    blocks = []
    for start, end in zip(tails, tails[1:]):
        begin = start
        for index in range(start + 1, end + 1):
            if index < end and index - begin < 128 and time[business[index], 0] > time[business[index - 1], -1]:
                continue
            blocks.append(business[begin:index])
            begin = index
    vectors = raw.reshape(len(raw), -1).astype("f8")
    ordered = []
    for ids in blocks:
        remaining = np.ones(len(ids), dtype=bool)
        current = 0
        permutation = []
        for _ in ids:
            permutation.append(ids[current])
            remaining[current] = False
            if not remaining.any():
                break
            delta = vectors[ids] - vectors[ids[current]]
            distance = np.cumsum(delta * delta, axis=1)[:, -1]
            distance[~remaining] = np.inf
            current = int(distance.argmin())
        ordered.append(np.asarray(permutation, dtype="<u4"))
    return raw, ordered


def load_decimal_column(directory, spec, record_order):
    width = spec["signed_bytes"]
    blob = (directory / f"{spec['column']}.integers").read_bytes()
    table = np.array([int.from_bytes(blob[i:i + width], "little", signed=True)
                      for i in range(0, len(blob), width)], dtype=object)
    keys = np.load(directory / f"{spec['column']}.input_keys.npy", mmap_mode="r")
    return table, table[keys[record_order]]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--public-input", type=Path, required=True)
    parser.add_argument("--decimal-input", type=Path, required=True)
    parser.add_argument("--grid", type=Path, required=True)
    parser.add_argument("--truth", type=Path, required=True)
    parser.add_argument("--pack-library", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    specifications = json.loads(args.grid.read_text())
    pack = ctypes.CDLL(str(args.pack_library)).pack_codes
    pack.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                     ctypes.c_void_p, ctypes.c_int]
    receipts = []
    for database in DATABASES:
        source = args.public_input / database
        destination = args.output / database
        destination.mkdir(parents=True, exist_ok=True)
        raw, blocks = physical_layout(source)
        record_order = np.concatenate(blocks)
        assert sorted(map(int, record_order)) == list(range(len(raw)))
        address = np.empty(len(raw), dtype="<u4")
        for block_id, ids in enumerate(blocks):
            assert len(ids) <= 128 and block_id < (1 << 18)
            address[ids] = (block_id << 14) | np.arange(len(ids), dtype="<u4")
        address.tofile(destination / "address.u32")
        exact_columns = []
        database_specs = sorted((item for item in specifications if item["db"] == database),
                                key=lambda item: item["column"])
        assert len(database_specs) == raw.shape[1]
        for spec in database_specs:
            table, original = load_decimal_column(args.decimal_input / database, spec,
                                                   np.arange(len(raw)))
            exact_columns.append((table, np.load(args.decimal_input / database /
                                                 f"{spec['column']}.input_keys.npy", mmap_mode="r")))
            values = load_decimal_column(args.decimal_input / database, spec, record_order)[1]
            if int(values.max()) - int(values.min()) >= 2**63:
                values = values.astype(object)
            positions = np.concatenate([np.arange(len(ids), dtype=np.int64) for ids in blocks])
            within = positions % 32
            residual_rows = np.flatnonzero(within > 0)
            coded_rows = np.flatnonzero(within >= 2)
            references = residual_rows - within[residual_rows]
            differences = values[residual_rows] - values[references]
            dictionary, inverse, frequency = np.unique(differences, return_inverse=True,
                                                       return_counts=True)
            permutation = np.argsort(-frequency, kind="stable")
            remap = np.empty_like(permutation)
            remap[permutation] = np.arange(len(permutation))
            keys = np.zeros(values.shape, dtype="<i4")
            keys[residual_rows] = remap[inverse].reshape(-1, 60)
            dictionary = dictionary[permutation]
            assert np.array_equal(values[references] + dictionary[keys[residual_rows]],
                                  values[residual_rows])
            anchor_width = integer_width(values)
            dictionary_width = integer_width(dictionary)
            key_width = integer_width([0, len(dictionary) - 1], signed=False)
            key_reference = coded_rows - within[coded_rows] + 1
            key_delta = keys[coded_rows].astype(np.int64) - keys[key_reference].astype(np.int64)
            codes = np.zeros(values.shape, dtype="<u4")
            codes[coded_rows] = ((key_delta << 1) ^ (key_delta >> 31)).astype("<u4")
            header = struct.pack("<8sBBBBHIII", b"PFR32V1\0", spec["decimal_exponent"],
                                 anchor_width, dictionary_width, key_width, 60, len(raw),
                                 len(blocks), len(dictionary))
            output = destination / f"c{spec['column']}.r32"
            with output.open("wb") as stream:
                stream.write(header)
                stream.write(integer_bytes(dictionary, dictionary_width))
                offset = 0
                for ids in blocks:
                    rows = len(ids)
                    low, high = offset, offset + rows
                    group_starts = np.arange(low, high, 32)
                    anchors = integer_bytes(values[group_starts], anchor_width)
                    first = np.minimum(group_starts + 1, high - 1)
                    first_keys = integer_bytes(keys[first], key_width, signed=False)
                    buffer = ctypes.create_string_buffer(4 << 20)
                    size = pack(codes[low:high].ctypes.data, rows, 60, 32,
                                buffer, len(buffer))
                    if size <= 0:
                        raise RuntimeError((database, spec["column"], rows, size))
                    page = b"VDR1" + bytes((0, 32)) + anchors + first_keys + buffer.raw[:size]
                    stream.write(struct.pack("<HI", rows, len(page)))
                    stream.write(page)
                    offset = high
                assert offset == len(raw)
            receipts.append({
                "dataset": database,
                "codec": "Ours",
                "column": f"c{spec['column']}",
                "scalars": len(raw) * 60,
                "bytes": output.stat().st_size,
                "ratio": 8 * len(raw) * 60 / output.stat().st_size,
                "dictionary_entries": len(dictionary),
                "dictionary_bytes": len(dictionary) * dictionary_width,
                "sha256": hashlib.sha256(output.read_bytes()).hexdigest(),
            })
        with (destination / "golden.i128").open("wb") as stream:
            key_arrays = [keys for _, keys in exact_columns]
            tables = [table for table, _ in exact_columns]
            for record in range(len(raw)):
                for table, keys in zip(tables, key_arrays):
                    for key in keys[record]:
                        stream.write(int(table[int(key)]).to_bytes(16, "little", signed=True))
        selected = [row for row in json.loads((args.truth / f"{database}.truth.json").read_text())
                    if row["window"] == 50]
        assert len(selected) == 24 and all(row["truth"] == row["exact"]["ids"] and
                                           len(row["truth"]) == 10 for row in selected)
        with (destination / "winners.tsv").open("w") as stream:
            for row in selected:
                stream.write(str(row["q"]) + "\t" + ",".join(map(str, row["truth"])) + "\n")
        (destination / "receipt.json").write_text(json.dumps({
            "dataset": database,
            "records": len(raw),
            "columns": raw.shape[1],
            "samples_per_column": 60,
            "blocks": len(blocks),
            "queries": len(selected),
            "group_size": 32,
            "reference": "first vector",
            "source_order_sha256": hashlib.sha256(record_order.tobytes()).hexdigest(),
        }, indent=2))
        print(database, "R32 packs built", flush=True)
    (args.output / "ours_ratios.json").write_text(json.dumps(receipts, indent=2))


if __name__ == "__main__":
    main()
