"""Plot a real L0 neighborhood and trace of the procedurally generated synthetic query."""
import csv
import json
from datetime import datetime, timezone
import matplotlib.pyplot as plt
import numpy as np
from paths import OUTPUT

directory=OUTPUT/"synthetic"
trace=json.loads((directory/"examples.json").read_text())["five-days"]
lists=np.fromfile(directory/"lists","<u4")
ends=np.fromfile(directory/"times.i64","<i8")
starts=np.fromfile(directory/"start_times.i64","<i8")
with (directory/"neighbors.tsv").open() as source:
    edges={int(row[0]):list(map(int,row[1:])) for row in csv.reader(source,delimiter="\t")}
envelopes={}
for node,targets in edges.items():
    members=np.isin(lists,[node,*targets])
    if members.any():
        envelopes[node]=(int(min(starts[members])),int(max(ends[members])))
def overlaps(node):
    a,b=envelopes[node]
    return a<trace["hi"] and b>=trace["lo"]
def date(ns):
    return datetime.fromtimestamp(ns/1e9,timezone.utc).strftime("%b %d")
def title(ax,label):
    ax.set_axis_off()
    ax.text(.02,.96,label,ha="left",va="top",weight="bold",fontsize=9,transform=ax.transAxes)
def text(ax,x,y,item,**kwargs):
    ax.text(x,y,item,transform=ax.transAxes,fontsize=8,**kwargs)
def table(ax,head,rows):
    artist=ax.table(cellText=rows,colLabels=head,cellLoc="center",bbox=[.02,.02,.96,.67])
    artist.auto_set_font_size(False)
    artist.set_fontsize(8)

node,admit,prune=next((n,yes,no) for n,neighbors in edges.items()
                        for yes in neighbors if yes in envelopes and overlaps(yes)
                        for no in neighbors if no in envelopes and not overlaps(no))
chosen=next(item for item in trace["lists"] if 0<item["hits"]<item["size"])
members=np.flatnonzero(lists==chosen["list"])
fig,(ax,post)=plt.subplots(2,1,figsize=(5.2,3.2))
title(ax,"HNSW time pruning · synthetic neighborhood")
text(ax,.06,.46,f"L0 node {node}",va="center")
for dst,y,label,color in [(admit,.67,"keep","#216870"),(prune,.2,"prune","#a54334")]:
    ax.annotate("",xy=(.48,y),xytext=(.25,.46),xycoords="axes fraction",
                arrowprops={"arrowstyle":"->","color":color})
    lo,hi=envelopes[dst]
    text(ax,.50,y,f"{dst}    {date(lo)} – {date(hi)}    {label}",color=color,va="center")
title(post,f"IVF binary search · list {chosen['list']} · [{date(trace['lo'])}, {date(trace['hi'])})")
table(post,["Postings","First offset","Stop offset","In window"],
      [[len(members),chosen["left"],chosen["right"],chosen["hits"]]])
fig.tight_layout()
fig.savefig(OUTPUT/"fig_hnsw_ivf_index.eps")
plt.close(fig)

fig,axes=plt.subplots(4,1,figsize=(4.6,5.7))
title(axes[0],"1   Route by weighted centroid cosine")
table(axes[0],["List","Centroid cosine","Postings"],
      [[a["list"],f"{a['centroid_cos']:.4f}",a["size"]] for a in trace["lists"][:3]])
title(axes[1],"2   Locate the window in each selected list")
table(axes[1],["List","First offset","Stop offset","Hits"],
      [[a["list"],a["left"],a["right"],a["hits"]] for a in trace["lists"][:3]])
title(axes[2],"3   Shortlist by cosine and rerank by distance")
table(axes[2],["Synthetic ID","Cosine","Distance"],
      [[w["id"],f"{w['cos']:.5f}",f"{w['dist']:.4f}"] for w in trace["winners"][:3]])
w=trace["winners"][0]
block,chron,phys=w["loc"]
title(axes[3],f"4   Read 18 timestamps and 54 values for synthetic ID {w['id']}")
table(axes[3],["Block","Time position","Group first","Value position"],
      [[block,chron,phys//32*32,phys]])
fig.tight_layout()
fig.savefig(OUTPUT/"fig_weighted_hnsw_ivf_query.eps")
