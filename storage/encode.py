"""Vary only the reference for the first, exact decimal difference. Fit each stream with the same dictionary and key/Rice rules."""
import csv, ctypes, json, struct
from pathlib import Path
import numpy as np
import argparse
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("frozen", type=Path)
parser.add_argument("inputs", type=Path)
parser.add_argument("library", type=Path)
parser.add_argument("output", type=Path)
parser.add_argument("--group", type=int, choices=[2,4,8,16,32], default=32)
parser.add_argument("--reference", choices=["first", "previous"], default="first")
args = parser.parse_args()
frozen, inputs, root = args.frozen, args.inputs, args.output
specs=json.loads((inputs/'grid.json').read_text())
pack=ctypes.CDLL(str(args.library.resolve())).pack_codes
pack.argtypes=[ctypes.c_void_p,ctypes.c_int,ctypes.c_int,ctypes.c_int,ctypes.c_void_p,ctypes.c_int]
def width(a,signed=True):
    lo,hi=int(np.min(a)),int(np.max(a))
    return next(w for w in (1,2,4,8,16) if (-(1<<(8*w-1)) if signed else 0)<=lo and hi<(1<<(8*w-int(signed))))
def encode_bytes(arr,w,signed=True):
    if w<=8:return arr.astype(f'<{"i" if signed else "u"}{w}',copy=False).tobytes()
    return b''.join(int(x).to_bytes(w,'little',signed=signed) for x in arr.ravel())
for db in dict.fromkeys(row['db'] for row in specs):
    loc=np.fromfile(frozen/'stores'/db/'sim_common/address.bin','<u4',offset=24)
    ids=np.fromfile(frozen/'time'/db/'time_ids.u32','<u4')
    order=np.lexsort((loc&127,loc>>14));orig=ids[order]
    lengths=np.bincount(loc>>14);edges=np.r_[0,np.cumsum(lengths)]
    pos=(loc[order]&127).astype(np.int64)
    assert np.array_equal(pos,np.arange(len(ids))-np.repeat(edges[:-1],lengths))
    for col,spec in enumerate([r for r in specs if r['db']==db]):
        w=spec['signed_bytes'];blob=(inputs/db/f'{col}.integers').read_bytes()
        integer_grid=np.array([int.from_bytes(blob[i:i+w],'little',signed=True) for i in range(0,len(blob),w)],dtype=np.int64 if w<=8 else object)
        v=integer_grid[np.load(inputs/db/f'{col}.input_keys.npy',mmap_mode='r')[orig]]
        if int(v.max())-int(v.min())>=2**63:v=v.astype(object)
        aw=width(v)
        for interval in (args.group,):
            within=pos%interval
            residual_rows=np.flatnonzero(within>0)
            coded_rows=np.flatnonzero(within>=2)
            for method in (args.reference,):
                reference=residual_rows-within[residual_rows] if method=='first' else residual_rows-1
                increments=v[residual_rows]-v[reference]
                distinct,inv,freq=np.unique(increments,return_inverse=True,return_counts=True)
                perm=np.argsort(-freq,kind='stable')
                remap=np.empty_like(perm);remap[perm]=np.arange(len(perm))
                keys=np.zeros(v.shape,dtype=np.int32)
                keys[residual_rows]=remap[inv].reshape(-1,18)
                dictionary=distinct[perm]
                assert np.array_equal(v[reference]+dictionary[keys[residual_rows]],v[residual_rows])
                dw=width(dictionary) if dictionary.size else 1;kw=width([0,max(0,len(dictionary)-1)],False)
                ds=struct.pack('<4sBBBBI',b'KDG1',spec['decimal_exponent'],aw,dw,kw,len(dictionary))+encode_bytes(dictionary,dw)
                # Both arms subtract the group first residual key in the second transform.
                key_reference=coded_rows-within[coded_rows]+1
                key_delta=keys[coded_rows]-keys[key_reference]
                codes=np.zeros(v.shape,'<u4');codes[coded_rows]=(key_delta<<1)^(key_delta>>31)
                folder=root/'packs'/f'{method}{interval}'/db;folder.mkdir(parents=True,exist_ok=True)
                (folder/f'{col}.dict').write_bytes(ds)
                with (folder/f'{col}.pages').open('wb') as out:
                    for lo,hi in zip(edges[:-1],edges[1:]):
                        base=np.arange(lo,hi,interval)
                        anchors=encode_bytes(v[base],aw)
                        fixed=encode_bytes(keys[np.minimum(base+1,hi-1)],kw,False)
                        buf=ctypes.create_string_buffer(65536)
                        size=pack(codes[lo:hi].ctypes.data,int(hi-lo),18,interval,buf,len(buf));assert size>0
                        payload=b'VDR1'+bytes([method=='previous',interval])+anchors+fixed+buf.raw[:size]
                        out.write(struct.pack('>I',len(payload)));out.write(payload)
    print(db,'packs complete',flush=True)
