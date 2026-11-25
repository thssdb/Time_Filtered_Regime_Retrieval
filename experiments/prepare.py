"""Prepare a three-channel collection or a deterministic synthetic fixture."""
import argparse
import csv
import json
from decimal import Decimal, getcontext
import random
from pathlib import Path
import numpy as np


def read_segments(path):
    segments = {}
    with path.open() as stream:
        for row in csv.DictReader(stream):
            segments.setdefault(row["segment"], []).append(
                (int(row["timestamp"]), [Decimal(row[c]) for c in ("mp", "ip", "n2")]))
    clocks, signals = [], []
    for entries in segments.values():
        entries.sort(key=lambda item: item[0])
        if len(entries) != 18 or len({t for t, _ in entries}) != 18:
            raise ValueError("Each segment needs 18 distinct sample times")
        clocks.append([t for t, _ in entries])
        signals.append([v for _, v in entries])
    return np.array(clocks, dtype="<i8"), np.asarray(signals, dtype=object)


def synthetic():
    rng = np.random.default_rng(2031)
    n = 6144
    t = np.arange(n)[:, None] * 100 + np.arange(18)
    phase = rng.uniform(0, 2*np.pi, (n, 1, 1))
    samples = np.arange(18)[None, :, None] / 12
    offsets = np.array([0.0, 0.6, 1.1])
    codes = np.rint(50*np.sin(phase + samples + offsets) +
                    rng.normal(0, 2, (n, 18, 3))).astype(int)
    decimals = np.array([Decimal(int(i)).scaleb(-1) for i in range(-100,101)])
    return t.astype("<i8"), decimals[codes+100]


def prepare(root, clocks, signals, legacy_decimal=False):
    db = "collection"
    source = root/"prepared"/db
    time = root/"time"/db
    inputs = root/"inputs"/db
    for directory in (source, time, inputs, root/"templates"):
        directory.mkdir(parents=True)
    raw = np.asarray(signals, dtype="<f4").transpose(0,2,1).copy()
    if not np.isfinite(raw).all():
        raise ValueError("Expected finite FP32 input")
    order = np.argsort(clocks[:,-1], kind="stable").astype("<u4")
    order.tofile(source/"business_order.u32")
    order.tofile(time/"time_ids.u32")
    clocks.tofile(source/"point_time_ns.i64")
    clocks[order,0].tofile(time/"start_times.i64")
    clocks[order,-1].tofile(time/"times.i64")
    raw.tofile(source/"raw54.f32")
    channels = [raw[:,0,:6], raw[:,1], raw[:,2]]
    scalers = np.array([(np.mean(x,dtype=float),np.std(x,dtype=float,ddof=1))
                        for x in channels], dtype="<f8")
    if not np.all(np.isfinite(scalers)) or np.any(scalers[:,1] <= 0):
        raise ValueError("Every retrieval channel needs positive finite sample variance")
    scalers.tofile(root/"scalers.f64")
    z = np.hstack([(x.astype(float)-mean)/sd for x,(mean,sd) in zip(channels,scalers)])
    z[order].astype("<f4").tofile(time/"time_route.f32")
    if legacy_decimal:
        specs=[]
        for c in range(3):
            v = signals[:,:,c]
            getcontext().prec = max(40, max(len(x.as_tuple().digits) for x in v.flat))
            exponent = max(0,-min(x.normalize().as_tuple().exponent for x in v.flat))
            table, inverse = np.unique(v, return_inverse=True)
            integers = [int(x.scaleb(exponent)) for x in table]
            width = next(w for w in (1,2,4,8,16) if min(integers)>=-(1<<(8*w-1))
                         and max(integers)<(1<<(8*w-1)))
            (inputs/f"{c}.integers").write_bytes(b"".join(x.to_bytes(width,"little",signed=True) for x in integers))
            np.save(inputs/f"{c}.input_keys.npy",inverse.reshape(v.shape))
            specs.append(dict(db=db,column=c,decimal_exponent=exponent,signed_bytes=width))
        (root/"inputs/grid.json").write_text(json.dumps(specs))
    blocks = []
    begin = 0
    for i in range(1,len(order)+1):
        if i==len(order) or i-begin==128 or clocks[order[i],0] <= clocks[order[i-1],-1]:
            blocks.append((len(blocks),db,f"b{len(blocks)}",begin,i-begin))
            begin=i
    with (root/"blocks.tsv").open("w") as out:
        csv.writer(out,delimiter="\t",lineterminator="\n").writerows(blocks)


if __name__ == "__main__":
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument("out",type=Path)
    inputs=ap.add_mutually_exclusive_group(required=True)
    inputs.add_argument("--synthetic",action="store_true")
    inputs.add_argument("--csv",type=Path)
    ap.add_argument("--weights",type=Path,help="42 little-endian float64 coordinate weights fitted on a knowledge base")
    ap.add_argument("--legacy-decimal",action="store_true",help="also prepare the scaled-decimal inputs for the historical encoder")
    args=ap.parse_args()
    values=synthetic() if args.synthetic else read_segments(args.csv)
    prepare(args.out,*values,legacy_decimal=args.legacy_decimal)
    hist=np.linspace(.5,1.,6)
    cov=np.r_[hist,np.ones(7),np.linspace(1.,.5,5)]
    weights=np.r_[hist,cov,cov] if not args.weights else np.fromfile(args.weights,"<f8")
    if args.csv and not args.weights:
        from sklearn.feature_selection import mutual_info_regression
        data=np.asarray(values[1],dtype="<f4")
        chosen=random.Random(42).sample(range(len(data)),min(len(data),3000))
        points=data[chosen].reshape(-1,3)
        chosen_points=np.random.RandomState(42).choice(len(points),min(10000,len(points)),replace=False)
        train=points[chosen_points].astype(float)
        mi=np.array([mutual_info_regression(train[:,[j]],train[:,0],random_state=42)[0] for j in (1,2)])
        if np.any(mi<=0):
            raise ValueError("Zero mutual information; supply explicit positive weights with --weights")
        weights*=np.repeat(np.r_[1.,mi/mi.max()],[6,18,18])
    if weights.shape!=(42,) or not np.all(np.isfinite(weights)) or np.any(weights<=0):
        raise ValueError("Expected 42 finite positive weights")
    weights.astype("<f8").tofile(args.out/"weights.f64")
