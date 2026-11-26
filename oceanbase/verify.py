"""Compare SQL routing, ranking and complete records with the standalone retrieval worker."""
import argparse
import json
import os
import subprocess
from contextlib import ExitStack
from pathlib import Path

import numpy as np

from connection import connect
from method import RegimeSearch


def verify(db, run, build):
    time = run / "time/collection"
    source = run / "prepared/collection"
    ids = np.fromfile(time / "time_ids.u32", "<u4")
    ends = np.fromfile(time / "times.i64", "<i8")
    values = np.memmap(source / "raw54.f32", "<f4", "r").reshape(-1, 54)
    clocks = np.memmap(source / "point_time_ns.i64", "<i8", "r").reshape(-1, 18)
    rng = np.random.default_rng(42)
    env = dict(os.environ, OMP_NUM_THREADS="1", OPENBLAS_NUM_THREADS="1",
               SCALER_FILE=str(run/"scalers.f64"), WEIGHT_FILE=str(run/"weights.f64"))
    checked = total = 0
    with ExitStack() as stack:
        engine = RegimeSearch(db, build, stack)
        reference = stack.enter_context(subprocess.Popen(
            [build/"search", time, run/"index.faiss"], env=env, text=True,
            stdin=subprocess.PIPE, stdout=subprocess.PIPE))
        stack.callback(reference.stdin.close)
        if json.loads(reference.stdout.readline())["status"] != "ready":
            raise RuntimeError("reference worker failed to start")
        for percent in range(10, 100, 10):
            width = max(1, len(ids) * percent // 100)
            for _ in range(8):
                begin = int(rng.integers(len(ids) - width + 1))
                lo, hi = int(ends[begin]), int(ends[begin + width - 1]) + 1
                raw = values[rng.integers(len(values)), np.r_[0:6, 18:54]]
                print("weighted_slice", lo, hi, *(float(v) for v in raw), file=reference.stdin, flush=True)
                reference_result = json.loads(reference.stdout.readline())
                lists, hits, records = engine.query(lo, hi, raw)
                expected = ids[reference_result["r"]].tolist()
                if lists != reference_result["lists"] or hits != expected:
                    raise ValueError(f"routing or ranking mismatch at {percent}%")
                if records != [clocks[i].tobytes() + values[i].tobytes() for i in expected]:
                    raise ValueError(f"payload mismatch at {percent}%")
                checked += 1
                total += len(records)
        _, hits, records = engine.query(int(ends[0])-1, int(ends[0]), raw)
        if hits or records:
            raise ValueError("expected an empty half-open window")
    print(f"Verified {checked} queries across nine windows, {total} full records and an empty window.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("database")
    parser.add_argument("run", type=Path)
    parser.add_argument("build", type=Path)
    args = parser.parse_args()
    with connect(args.database) as con:
        verify(con, args.run.resolve(), args.build.resolve())
