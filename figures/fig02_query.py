from datetime import datetime, timezone
from pathlib import Path
import json
import numpy as np
import subprocess
from fontTools.ttLib import TTFont
from pptx import Presentation
from pptx.dml.color import RGBColor
from pptx.enum.shapes import MSO_SHAPE
from pptx.enum.text import PP_ALIGN, MSO_ANCHOR, MSO_AUTO_SIZE
from pptx.oxml.xmlchemy import OxmlElement
from pptx.util import Pt

from paths import OUTPUT as here
record = json.loads((here/'synthetic/examples.json').read_text())['half']
winner = record['winners'][0]
p = Presentation()
s = p.slides.add_slide(p.slide_layouts[6])
for item in list(s.shapes):
    s.shapes._spTree.remove(item._element)
S = 2
def pos(n): return Pt(n*S)
W = 505
p.slide_width, p.slide_height = pos(W), pos(178)
ink, blue, gray, pale, edge, white = '202F3C','276183','51616B','EAF2F7','BDCDD5','FFFFFF'
font = 'Linux Biolinum O'
numeric_font = font
type_size = 9.00
clr = RGBColor.from_string

def paint(shape, color):
    shape.fill.solid()
    shape.fill.fore_color.rgb = clr(color)
    shape.line.fill.background()

def bar(x,y,w,h,color):
    sh=s.shapes.add_shape(MSO_SHAPE.RECTANGLE,pos(x),pos(y),pos(w),pos(h))
    paint(sh,color)
    sh.shadow.inherit=False
    sh._element.xpath("./p:style/a:effectRef")[0].set("idx","0")
    return sh

def letters(frame, value, bold=False, color=ink, center=False, face=font):
    frame.word_wrap=False
    frame.auto_size=MSO_AUTO_SIZE.NONE
    frame.vertical_anchor=MSO_ANCHOR.MIDDLE
    frame.margin_left=frame.margin_right=frame.margin_top=frame.margin_bottom=0
    paragraph=frame.paragraphs[0]
    paragraph.alignment=PP_ALIGN.CENTER if center else PP_ALIGN.LEFT
    paragraph.space_before=paragraph.space_after=Pt(0)
    run=paragraph.add_run()
    run.text=str(value)
    run.font.name=face
    run.font.size=pos(type_size)
    run.font.bold=bold
    run.font.color.rgb=clr(color)

def label(x,y,w,h,value,**opts):
    shape=s.shapes.add_textbox(pos(x),pos(y),pos(w),pos(h))
    letters(shape.text_frame,value,**opts)
    return shape

def fmt_cell(cell, value, background, face=font, bold=False, color=ink, center=True):
    cell.margin_left=cell.margin_right=cell.margin_top=cell.margin_bottom=0
    letters(cell.text_frame,value,bold,color,center,face)
    cell.fill.solid()
    cell.fill.fore_color.rgb=clr(background)
    pr=cell._tc.get_or_add_tcPr()
    for side in 'LRTB':
        line=OxmlElement('a:ln'+side)
        line.append(OxmlElement('a:noFill'))
        pr.append(line)

id=winner['id']
signals=['MP','IP','N2']
values=[[np.format_float_positional(np.float32(v),unique=True,trim='-') for v in winner['values'][i*18:(i+1)*18]] for i in range(3)]
def utc(ns):
    sec,frac=divmod(ns,10**9)
    return datetime.fromtimestamp(sec,timezone.utc).strftime('%Y-%m-%d %H:%M:%S')+f'.{frac:09d}'
label(0,0,W,12,f'(a) Synthetic record {id} at 18 sample positions',bold=True)
fontpath=subprocess.check_output(['fc-match','-f','%{file}',numeric_font],text=True)
num=TTFont(fontpath)
cm, metrics, unit=num.getBestCmap(),num['hmtx'].metrics,num['head'].unitsPerEm
def length(v):
    return sum(metrics[cm[ord(c)]][0] for c in v)*type_size/unit
widths=[max(length(row[j]) for row in values) for j in range(18)]
stub=16
padding=(W-stub-sum(widths))/18
assert padding>.65
widths=[stub]+[v+padding for v in widths]
ys=15
rowh=12.2
top_table=s.shapes.add_table(4,19,0,pos(ys),pos(W),pos(rowh*4))
top_table.name='Aligned readings at sample positions 0 through 17'
t=top_table.table
t.first_row=t.first_col=t.horz_banding=t.vert_banding=False
for col,width in zip(t.columns,widths): col.width=pos(width)
for i,row in enumerate([['j']+list(range(18))]+[[sig]+vs for sig,vs in zip(signals,values)]):
    t.rows[i].height=pos(rowh)
    for j,text in enumerate(row):
        data=(i>0 and j>0)
        fmt_cell(t.cell(i,j),text,pale if i==0 else white,
                 numeric_font if data else font, bold=not data,
                 color=blue if i==0 else ink,center=(j!=0))
# Quiet vertical separators keep narrow adjacent decimal columns distinct.
x=0
for j,w in enumerate(widths[:-1]):
    x+=w
    bar(x-.11,ys+.3,.22,4*rowh-.3,edge)
bar(0,ys+rowh-.2,W,.4,edge)
bar(0,ys+4*rowh,W,.55,edge)

left=182
right=202
label(0,72,left+7,12,'(b) Search projection (42 of 54 coordinates)',bold=True)
label(right,72,W-right,12,'(c) Weighted cosine followed by weighted distance',bold=True)
bar(191,74,.45,101,edge)
x0=22
dx=8.55
y0=91
for i, sig in enumerate(signals):
    label(0,y0+13*i-1,21,12,sig,color=gray,bold=True)
    for j in range(18):
        use=i>0 or j<6
        shape=bar(x0+j*dx,y0+i*13+1,dx-.9,8,blue if use else white)
        if not use:
            shape.line.color.rgb=clr(blue)
            shape.line.width=pos(.45)
for start,stop in [(0,5),(6,17)]:
    segment_width=(stop-start+1)*dx-1
    bar(x0+start*dx,131,segment_width,.4,edge)
    label(x0+start*dx,133,segment_width,11,f'{start}–{stop}',center=True,color=gray)
def key(x,y,enabled,v,w):
    sh=bar(x,y+2,7,7,blue if enabled else white)
    if not enabled:
        sh.line.color.rgb=clr(blue)
        sh.line.width=pos(.45)
    label(x+10,y,w,11,v,color=gray)
key(0,149,True,'Scored (z-score)',86)
key(100,149,False,'Returned only',74)
# Full nanosecond timestamps share the UTC label.
label(right,89,69,11,'End time ≥',color=gray)
label(right+65,89,235,11,utc(record['lo'])+' UTC')
label(right,102,69,11,'End time <',color=gray)
label(right+65,102,235,11,utc(record['hi'])+' UTC')
labels=['Segments','In window','Cosine shortlist','Returned']
results=[f"{record['records']:,}",f"{record['n']:,}",'100','10']
gridwidths=[60,67,90,52]
t2=s.shapes.add_table(2,4,pos(right),pos(119),pos(sum(gridwidths)),pos(26)).table
t2.first_row=t2.first_col=t2.horz_banding=t2.vert_banding=False
for i,ww in enumerate(gridwidths): t2.columns[i].width=pos(ww)
for i,row in enumerate([labels,results]):
    t2.rows[i].height=pos(13)
    for j,txt in enumerate(row):
        fmt_cell(t2.cell(i,j),txt,pale if i==0 else white,bold=(i==0),color=blue if i==0 else ink)
bar(right,145,sum(gridwidths),.4,edge)
label(right,151,W-right,12,f"Top ID {id}    ·    Weighted distance {winner['dist']:.4f}",bold=True)
enddate=utc(winner['end']).split()[0]
label(right,164,W-right,12,f'Segment end date: {enddate} UTC',color=gray)
s.notes_slide.notes_text_frame.text=(
    f"Synthetic 42D example. (a) Each column belongs to one common sampling position j of record {id}. "
    "The three signal rows use decimal strings that recover the stored FP32 readings exactly. "
    "(b) Filled cells are used for both scores. MP positions 6 through 17 are returned only. "
    "(c) The lower time bound is inclusive and the upper bound is exclusive. The exact query keeps "
    "the 100 highest weighted cosine scores and returns the ten smallest weighted distances. "
    f"Record {id} ranks first.")
p.save(here/'fig_temporal_query_example.pptx')
