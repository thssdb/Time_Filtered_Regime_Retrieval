"""Verify every reconstructed timestamp and every FP32 bit, in chronological-rank order."""
import argparse
from pathlib import Path
import numpy as np
from worker import Workers


if __name__ == "__main__":
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("run",type=Path)
    parser.add_argument("build",type=Path)
    args=parser.parse_args()
    base=args.run/"prepared/collection"
    ids=np.fromfile(args.run/"time/collection/time_ids.u32","<u4")
    clocks=np.memmap(base/"point_time_ns.i64","<i8","r").reshape(-1,18)
    values=np.memmap(base/"raw54.f32","<f4","r").reshape(-1,54)
    with Workers(args.run,args.build) as native:
        for start in range(0,len(ids),10):
            ranks=list(range(start,min(start+10,len(ids))))
            _,actual=native.fetch(ranks)
            expected=b"".join(clocks[ids[i]].tobytes()+values[ids[i]].tobytes() for i in ranks)
            if actual!=expected:
                raise RuntimeError(f"bit mismatch in ranks {ranks[0]} to {ranks[-1]}")
    print(f"Verified {len(ids):,} complete records, {18*len(ids):,} timestamps and {54*len(ids):,} FP32 readings.")
