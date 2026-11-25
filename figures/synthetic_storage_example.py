"""Reconstruct the public synthetic example, including its true First-32 keys and bits."""
import numpy as np
from pathlib import Path
from datetime import datetime,timedelta,timezone
from collections import Counter
import json
from paths import OUTPUT as out
J=18; n=6144
j=np.arange(J)[None,:]
t=np.arange(n)/95 + np.random.default_rng(1907).normal(0,.9,n)
phase=t[:,None]*.17+j*.21
v=np.hstack([np.round(2*(35+7*np.sin(phase)+.28*j))/2,
             np.round(2*(62+10*np.cos(phase*.8)+.49*j))/2,
             np.round(2*(79+4*np.sin(phase*.77)+.13*j))/2]).astype('<f4')
v[:,7]+=.015625;v[:,13]+=.046875
store=[]
for lo in range(0,n,128):
 block=v[lo:lo+128].astype(float); ok=np.ones(len(block),bool); h=0
 for p in range(len(block)):
  store.append(lo+h);ok[h]=False
  if ok.any():
   delta=block-block[h];sq=np.einsum('ij,ij->i',delta,delta);sq[~ok]=np.inf;h=int(sq.argmin())
o=np.array(store)
assert o[92]==86 and (37*o[92]+3)%n==3185
target=[64,65,80,92]
selected={}
for ci,col in enumerate(['MP','IP','N2']):
 a=v[o,18*ci:18*ci+18]; scale=10**(6 if ci==0 else 1)
 X=np.rint(a.astype(float)*scale).astype(np.int64)
 d=X-X[np.arange(n)//32*32]
 counter=Counter(d[np.arange(n)%32>0].ravel().tolist())
 dictionary=sorted(counter,key=lambda x:(-counter[x],x));keys={x:k for k,x in enumerate(dictionary)}
 K=np.vectorize(lambda x: keys.get(x,0))(d)
 F=K[np.arange(n)//32*32+1]
 h=K-F;u=np.where(h>=0,2*h,-2*h-1)
 block=u[:128][np.arange(128)%32>=2]
 kk=np.array([np.argmin([int((block[:,j]>>k).sum())+len(block)*(1+k) for k in range(25)]) for j in range(J)])
 def code(v,k):
  v=int(v);k=int(k);return '0'*(v>>k)+'1'+''.join(str((v>>i)&1) for i in range(k))
 for p in target:
  r=selected.setdefault(p,dict(p=p,h=int(o[p]),id=int((37*o[p]+3)%n),end=(datetime(2040,1,1,tzinfo=timezone.utc)+timedelta(hours=int(o[p]))).strftime('%b %d %H:%M')))
  r[col]=dict(value=a[p].tolist(),delta=(d[p].astype(float)/scale).tolist(),keys=K[p].tolist(),F=F[p].tolist(),u=u[p].tolist(),param=kk.tolist(),bits=[code(x,k) for x,k in zip(u[p],kk)])
(out/'synthetic.json').write_text(json.dumps(selected,indent=2))
for p,r in selected.items():
 c=r['IP'];print(p,r['h'],r['id'],r['end'],'IP',[(c['value'][i],c['delta'][i],c['keys'][i],c['F'][i],c['u'][i],c['param'][i],c['bits'][i]) for i in [0,1,2]])
