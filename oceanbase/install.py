"""Import the graph, posting lists and Group-16 compressed blocks into an empty schema."""
import argparse
import struct
import subprocess
from pathlib import Path
from tempfile import TemporaryDirectory

import numpy as np

from connection import connect
from method import COLS


def install(con, run, binaries):
    source = run / "previous16/collection"
    time = run / "time/collection"
    weights = np.fromfile(run / "weights.f64", "<f8")
    scalers = np.fromfile(run / "scalers.f64", "<f8").reshape(3, 2)
    if weights.shape != (42,) or np.any(weights <= 0) or not np.isfinite(weights).all():
        raise ValueError("invalid coordinate weights")
    if np.any(scalers[:, 1] <= 0) or not np.isfinite(scalers).all():
        raise ValueError("invalid channel scalers")
    ids = np.fromfile(time / "time_ids.u32", "<u4")
    starts = np.fromfile(time / "start_times.i64", "<i8")
    ends = np.fromfile(time / "times.i64", "<i8")
    vectors = np.memmap(time / "time_route.f32", "<f4", "r").reshape(-1, len(COLS))
    with (run / "stores/collection/sim_common/address.bin").open("rb") as f:
        magic, version, nrows, nblocks, reserved = struct.unpack("<8sIIII", f.read(24))
        loc = np.fromfile(f, "<u4")
    if (magic, version, nrows, reserved) != (b"NHADDR1\0", 1, len(ids), 0):
        raise ValueError("invalid locator header")
    with TemporaryDirectory() as folder, con.cursor() as c:
        tmp = Path(folder)
        subprocess.run([binaries/"ob_router", "export", run/"index.faiss", run/"weights.f64",
                        tmp/"graph", tmp/"lists", tmp/"nlist"], check=True)
        subprocess.run([binaries/"ob_pages", "export", source/"data", tmp], check=True)
        lists = np.fromfile(tmp / "lists", "<i4")
        if not all(len(column) == len(ids) for column in (starts, ends, lists, loc, vectors)):
            raise ValueError("inconsistent posting counts")
        nlist = int((tmp / "nlist").read_text())
        if np.any((lists < 0) | (lists >= nlist)):
            raise ValueError("invalid list assignment")
        counts = np.bincount(lists, minlength=nlist)
        limits = np.iinfo(np.int64)
        low = np.full(nlist, limits.max, dtype=np.int64)
        high = np.full(nlist, limits.min, dtype=np.int64)
        np.minimum.at(low, lists, starts)
        np.maximum.at(high, lists, ends)
        bounds = "".join(f"{n} {lo} {hi}\n" for n, lo, hi in zip(counts, low, high))
        metadata = {"graph": (tmp / "graph").read_bytes(), "bounds": bounds.encode(),
                    "weights": weights.tobytes(), "scalers": scalers.tobytes()}
        metadata.update({f"{i}.dict": (source / f"{i}.dict").read_bytes() for i in range(3)})
        blocks = sorted((p for p in tmp.iterdir() if p.name.isdigit()), key=lambda p: int(p.name))
        if len(blocks) != nblocks or np.any((loc >> 14) >= nblocks):
            raise ValueError("inconsistent block directory")
        c.execute("SHOW TABLES")
        if c.fetchone() is not None:
            raise ValueError("install requires an empty database")
        c.execute("CREATE TABLE metadata(name VARCHAR(16) PRIMARY KEY, payload MEDIUMBLOB NOT NULL)")
        c.execute("CREATE TABLE blocks(block_id INT PRIMARY KEY, payload MEDIUMBLOB NOT NULL)")
        c.execute("CREATE TABLE postings(list_id INT NOT NULL,end_ns BIGINT NOT NULL,"
                  "id INT UNSIGNED NOT NULL,addr INT UNSIGNED NOT NULL,inv_norm DOUBLE NOT NULL,"
                  + ",".join(col + " DOUBLE NOT NULL" for col in COLS) + ",PRIMARY KEY(list_id,end_ns,id))")
        c.executemany("INSERT INTO metadata VALUES (%s,%s)", list(metadata.items()))
        for first in range(0, len(blocks), 24):
            c.executemany("INSERT INTO blocks VALUES (%s,%s)",
                          [(int(p.name), p.read_bytes()) for p in blocks[first:first + 24]])
        insert = "INSERT INTO postings VALUES (" + ",".join(["%s"] * (5 + len(COLS))) + ")"
        for a in range(0, len(ids), 384):
            batch = vectors[a:a+384].astype(float)
            squared = np.zeros(len(batch))
            for j in range(len(COLS)):
                squared += (weights[j] * batch[:, j]) * batch[:, j]
            if np.any(squared <= 0) or not np.isfinite(squared).all():
                raise ValueError("invalid weighted norm")
            inverse = 1.0 / np.sqrt(squared)
            rows = []
            for offset, row in enumerate(batch):
                i = a + offset
                rows.append((int(lists[i]), int(ends[i]), int(ids[i]), int(loc[i]),
                             float(inverse[offset]), *row.tolist()))
            c.executemany(insert, rows)
        c.execute("SELECT COUNT(*) FROM postings")
        if c.fetchone()[0] != len(ids):
            raise ValueError("incomplete posting import")
        c.execute("SELECT COUNT(*) FROM blocks")
        if c.fetchone()[0] != nblocks:
            raise ValueError("incomplete block import")
        c.execute("INSERT INTO metadata VALUES (%s,%s)", ("ready", b"group16-v1"))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("database")
    parser.add_argument("run", type=Path)
    parser.add_argument("build", type=Path)
    args = parser.parse_args()
    with connect(args.database) as connection:
        install(connection, args.run.resolve(), args.build.resolve())
