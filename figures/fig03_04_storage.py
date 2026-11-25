"""Editable single-column diagrams using the same synthetic query as Fig. 6."""
from pathlib import Path
import json
from zipfile import ZipFile
from pptx import Presentation
from pptx.util import Inches,Pt
from pptx.dml.color import RGBColor
from pptx.enum.text import PP_ALIGN,MSO_ANCHOR
from pptx.enum.shapes import MSO_SHAPE, MSO_CONNECTOR
from pptx.oxml.xmlchemy import OxmlElement
from paths import OUTPUT as out
data=json.loads((out/'synthetic.json').read_text())
def color(s):return RGBColor.from_string(s)
ink='1D2933';teal='17655F';line='788894'
def new(h):
 global p,s
 p=Presentation();p.slide_width=Inches(3.37);p.slide_height=Inches(h);s=p.slides.add_slide(p.slide_layouts[6])
def text(x,y,w,h,v,bold=False,align=PP_ALIGN.CENTER,c=ink):
 sh=s.shapes.add_textbox(*map(Inches,[x,y,w,h]));tf=sh.text_frame;tf.word_wrap=False
 tf.margin_left=tf.margin_right=tf.margin_top=tf.margin_bottom=0
 tf.vertical_anchor=MSO_ANCHOR.MIDDLE
 par=tf.paragraphs[0];par.alignment=align;r=par.add_run();r.text=v
 r.font.name='STIX Two Text';r.font.size=Pt(9);r.font.bold=bold;r.font.color.rgb=color(c)
def rule(x1,y1,x2,y2,c=line,lw=.45):
 sh=s.shapes.add_connector(MSO_CONNECTOR.STRAIGHT,*map(Inches,[x1,y1,x2,y2]))
 sh.line.color.rgb=color(c);sh.line.width=Pt(lw)
def table(y,widths,rows,rowh=.188):
 x=.035;w=sum(widths)
 for i,row in enumerate(rows):
  if i==0:
   r=s.shapes.add_shape(MSO_SHAPE.RECTANGLE,*map(Inches,[x,y,w,rowh]));r.shadow.inherit=False;r.fill.solid();r.fill.fore_color.rgb=color('EAF1F3');r.line.fill.background()
  xx=x
  for ww,v in zip(widths,row):text(xx,y+i*rowh,ww,rowh,str(v),i==0);xx+=ww
  rule(x,y+(i+1)*rowh,x+w,y+(i+1)*rowh,'BAC4C7',.3)
 return y+len(rows)*rowh
def title(y,label):text(.04,y,3.28,.20,label,True,PP_ALIGN.LEFT,teal)
def save(name,notes):
 for e in s._element.xpath('.//a:effectRef'):e.set('idx','0')
 s.notes_slide.notes_text_frame.text=notes;p.save(out/(name+'.pptx'))
new(2.97)
c='IP';p0=data['64'][c];f=data['65'][c];t=data['92'][c]
title(.015,'1   Read three IP samples per segment')
table(.245,[.48,.67,.72,.71,.72],[['Pos.','Role','j = 0','j = 1','j = 2'],[64,'First',*p0['value'][:3]],[65,'Second',*f['value'][:3]],[92,'Target',*t['value'][:3]]])
title(1.06,'2   Look up the residuals from position 64')
table(1.30,[1.22,.67,.70,.71],[['','j = 0','j = 1','j = 2'],['Target residual',*t['delta'][:3]],['Target key',*t['keys'][:3]],['Reference key',*f['keys'][:3]]])
title(2.09,'3   Subtract reference keys and encode')
table(2.33,[1.22,.67,.70,.71],[['','j = 0','j = 1','j = 2'],['ZigZag',*t['u'][:3]],['Rice bits',*t['bits'][:3]]])
save('fig_temporal_storage_codec','Synthetic example from the five-day query. Positions 64–95 share one group. The residual reference for the keys is taken at position 65. The three Rice parameters in this example are zero.')
new(3.07)
title(.015,'Chronological pages in block 0')
rows=[['Time pos.','ID','End (UTC)','Value pos.']]
for r in sorted(data.values(),key=lambda e:e['h']):rows.append([r['h'],r['id'],r['end'],r['p']])
table(.25,[.76,.65,1.12,.77],rows)
title(1.27,'Value pages in similarity order')
rows=[['Value pos.','Field','MP','IP','N2']]
for k,r in data.items():
 vals=[str(r[c]['value'][2]) if k=='64' else str(r[c]['keys'][2]) if k=='65' else r[c]['bits'][2] for c in ('MP','IP','N2')]
 rows.append([k,'Absolute' if k=='64' else 'First key' if k=='65' else 'Rice bits',*vals])
table(1.50,[.67,.64,.71,.65,.63],rows)
title(2.53,'Lookup for synthetic ID 3185')
text(.05,2.76,3.27,.20,'Time position 86     •     First value 64 + residual at 92')
save('fig_similarity_storage','The four selected segments belong to the same synthetic time block. End times are relative to the synthetic January 2040 data. Bottom table shows sample index two for all three signals. The chronological time page uses TS2DIFF. The ID column maps each segment to its value position.')
