"""Reproducible public illustration, independent of aircraft records and parameters."""
from pathlib import Path
import json,os,subprocess
import numpy as np
from paths import OUTPUT
import argparse
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument("build",type=Path,help="CMake build directory")
build=parser.parse_args().build.resolve()
d=OUTPUT/'synthetic'
d.mkdir(exist_ok=True)
N=6144;J=18;j=np.arange(J)[None,:]
ids=(37*np.arange(N)+3)%N
rng=np.random.default_rng(1907)
def flight(t):
    phase=(t[:,None]*.17 + j*.21)
    readings=np.hstack([np.round(2*(35+7*np.sin(phase)+.28*j))/2,
                      np.round(2*(62+10*np.cos(phase*.8)+.49*j))/2,
                      np.round(2*(79+4*np.sin(phase*.77)+.13*j))/2]).astype('<f4')
    # Two return-only MP positions have six decimal places, exactly representable in FP32.
    readings[:,7] += 0.015625
    readings[:,13] += 0.046875
    return readings
raw=flight(np.arange(N)/95+rng.normal(0,.9,N))
query=flight(np.array([1.2]))[0,np.r_[0:6,18:54]]
parts=[raw[:,a:a+n].astype(float) for a,n in [(0,6),(18,18),(36,18)]]
params=np.array([[v.mean(),v.std(ddof=1)] for v in parts])
params.astype('<f8').tofile(d/'scales')
m,s=np.repeat(params,[6,18,18],axis=0).T
x=((raw[:,np.r_[0:6,18:54]]-m)/s).astype('<f4')
z=((query-m)/s).astype('<f4')
x.tofile(d/'time_route.f32')
start=2208988800000000000;hour=3600*10**9
ends=start+hour*np.arange(N,dtype=np.int64);ends.tofile(d/'times.i64');(ends-17*10**9).tofile(d/'start_times.i64');ids.astype('<u4').tofile(d/'time_ids.u32')
ws=np.repeat([1.,.8,.5],[6,18,18]);ws.tofile(d/'weights')
from tempfile import TemporaryDirectory
with TemporaryDirectory(dir=d) as staging:
    temp_index=Path(staging)/"synthetic.faiss"
    subprocess.run([build/'build_index',d/'time_route.f32','64',temp_index],check=True,
                   env=dict(os.environ,OMP_NUM_THREADS="1",OPENBLAS_NUM_THREADS="1"))
    temp_index.replace(d/'index.faiss')
subprocess.run([build/'inspect_index',d/'index.faiss',d/'centroids',d/'neighbors.tsv'],check=True)
cent=np.fromfile(d/'centroids','<f4').reshape(-1,42)
xd=x.astype(float);qd=z.astype(float)
def cosine(v):
    return ((v*ws)@qd)/(np.sqrt(((v*ws)*v).sum(axis=1))*np.sqrt((ws*qd*qd).sum()))
coss=cosine(xd);dc=np.sqrt((ws*(xd-qd)**2).sum(axis=1));out={}
def order(rank):
    pool=rank[np.lexsort((ids[rank],-coss[rank]))[:100]]
    return pool[np.lexsort((ids[pool],dc[pool]))[:10]]
# Similarity positions use the same greedy chain as the storage layout: begin
# chronologically, then append the nearest unvisited segment.  np.argmin keeps
# chronological order when squared distances tie.
sim=np.empty(N,int)
for block_start in range(0,N,128):
    vectors=raw[block_start:block_start+128].astype(float)
    remaining=np.ones(len(vectors),dtype=bool)
    current=0
    for position in range(len(vectors)):
        sim[block_start+current]=position
        remaining[current]=False
        if position+1<len(vectors):
            offsets=vectors-vectors[current]
            squared_distances=np.einsum('ij,ij->i',offsets,offsets)
            squared_distances[~remaining]=np.inf
            current=int(np.argmin(squared_distances))
for name,a,b in [('half',0,3072),('five-days',24,144)]:
    key='weighted_slice';line=f'{key} {ends[a]} {ends[b]} '+' '.join(format(v,'.9g') for v in query)+'\n'
    env=dict(os.environ,SCALER_FILE=str(d/'scales'),WEIGHT_FILE=str(d/'weights'),LIST_MAP=str(d/'lists'))
    with (d/(name+'.trace')).open('w') as log:
        run=subprocess.run([build/'search',d,d/'index.faiss'],env=env,input=line,text=True,stdout=subprocess.PIPE,stderr=log,check=True)
    reply=json.loads(run.stdout.splitlines()[-1]);lists=np.fromfile(d/'lists','<u4');sel=reply['lists']
    allr=np.arange(a,b);cand=allr[np.isin(lists[allr],sel)]
    rr=order(allr) if name=='half' else order(cand)
    assert list(order(cand))==reply['r'] and len(rr)==10
    records=[dict(id=int(ids[i]),rank=int(i),end=int(ends[i]),values=raw[i].tolist(),loc=[int(i//128),int(i%128),int(sim[i])],dist=float(dc[i]),cos=float(coss[i])) for i in rr]
    chosen=[]
    for l in sel:
        rows=np.flatnonzero(lists==l);left,right=rows.searchsorted([a,b])
        chosen.append(dict(list=l,size=int(len(rows)),hits=int(right-left),left=int(left),right=int(right),centroid_cos=float(cosine(cent[l:l+1].astype(float))[0])))
    out[name]=dict(n=b-a,records=N,lo=int(ends[a]),hi=int(ends[b]),a=a,b=b,scored=len(cand),visited=sum(r['size'] for r in chosen),lists=chosen,winners=records)
(d/'examples.json').write_text(json.dumps(out,indent=2))
