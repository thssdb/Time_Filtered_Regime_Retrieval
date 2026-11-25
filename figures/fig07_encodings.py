from copy import deepcopy
from io import BytesIO
from pathlib import Path
from zipfile import ZipFile
import csv
import xlsxwriter
from lxml import etree as ET
from pptx import Presentation
from pptx.chart.data import CategoryChartData
from pptx.dml.color import RGBColor
from pptx.enum.chart import XL_CHART_TYPE
from pptx.enum.shapes import MSO_SHAPE, MSO_CONNECTOR
from pptx.enum.dml import MSO_PATTERN_TYPE
import json
import sys
from pptx.enum.text import PP_ALIGN, MSO_ANCHOR
from pptx.util import Inches, Pt

from paths import INPUT as DATA, OUTPUT as OUT
from paper_result_figure_fonts import normalize_result_figure_fonts
def records(filename):
    source=DATA/filename
    with source.open() as f:
        return list(csv.DictReader(f))
ratios = {(r['reference'],r['column'],r['codec']): float(r['ratio']) for r in records('fig8_codec_ratios.csv')}
times = {(r['reference'],r['component'],r['codec']): r for r in records('fig8_codec_decode.csv')}
colors = {'PLAIN':'ADB5BF','TS2DIFF':'5A829E','ALP':'E5B567','Camel':'9E8373','LZ4':'87B4A8','Zstd':'9288AB','Rice':'6C7785','Ours':'B32231'}
patterns={'Ours':'light_upward_diagonal','TS2DIFF':'light_downward_diagonal','Camel':'light_upward_diagonal'}
hatchcolor={'Ours':'9A1D28','TS2DIFF':'3C617D','Camel':'765D50'}
fontname = 'STIX Two Text'
font = {'name':fontname,'size':8.5,'color':'#242A30'}
ns = {'c':'http://schemas.openxmlformats.org/drawingml/2006/chart',
      'r':'http://schemas.openxmlformats.org/officeDocument/2006/relationships'}
pres = Presentation()
pres.slide_width=Inches(7.14)
pres.slide_height=Inches(2.10)
slide = pres.slides.add_slide(pres.slide_layouts[6])
def title(x,y,w,h,string,size,bold=False,align=PP_ALIGN.LEFT,color='242A30'):
    text=slide.shapes.add_textbox(Inches(x), Inches(y), Inches(w), Inches(h))
    frame=text.text_frame
    frame.margin_left=frame.margin_right=frame.margin_top=frame.margin_bottom=0
    frame.vertical_anchor=MSO_ANCHOR.MIDDLE
    frame.word_wrap=False
    line=frame.paragraphs[0]
    line.alignment=align
    run=line.add_run();run.text=string
    run.font.size=Pt(size);run.font.name=fontname;run.font.bold=bold
    run.font.color.rgb=RGBColor.from_string(color)
def display(codec): return codec

for (name,color),x in zip(colors.items(), [.10,1.07,2.20,3.04,3.93,4.76,5.56,6.35]):
    swatch=slide.shapes.add_shape(MSO_SHAPE.RECTANGLE, Inches(x), Inches(.067), Inches(.13), Inches(.115))
    swatch.shadow.inherit=False
    swatch.fill.solid();swatch.fill.fore_color.rgb=RGBColor.from_string(color)
    swatch.line.color.rgb=RGBColor.from_string('41464A');swatch.line.width=Pt(.30)
    if name in patterns:
        swatch.fill.patterned()
        swatch.fill.pattern=MSO_PATTERN_TYPE.LIGHT_UPWARD_DIAGONAL if patterns[name]=='light_upward_diagonal' else MSO_PATTERN_TYPE.LIGHT_DOWNWARD_DIAGONAL
        swatch.fill.back_color.rgb=RGBColor.from_string(color)
        swatch.fill.fore_color.rgb=RGBColor.from_string(hatchcolor[name])
    title(x+.175,.012,.82,.22,display(name),9.4,name=='Ours',color=color if name=='Ours' else '242A30')

axisstyle = {'num_font':font,'name_font':dict(font,bold=False),'line':{'color':'#5B6670','width':.55},
             'major_tick_mark':'outside','minor_tick_mark':'none','major_gridlines':{'visible':False},
             'minor_gridlines':{'visible':False}}
def panel_workbook(ref,component,with_plot):
    ts = component == 'timestamps'
    columns = ['Timestamps'] if ts else ['MP','IP','N2']
    groups = ['Ratio','p50','p95'] if ts else ['MP','IP','N2','p50','p95']
    stream = BytesIO()
    book = xlsxwriter.Workbook(stream, {'in_memory':True})
    ws = book.add_worksheet('Data')
    names = list(colors)
    ws.write_row(0,0,['Group'] + [display(m)+' ratio' for m in names] + [display(m)+' time (µs)' for m in names])
    ws.write_column(1,0,groups)
    ws.set_column(0,14,17)
    for j,m in enumerate(names,1):
        for i,col in enumerate(columns,1):
            ws.write_number(i,j,ratios['first32' if m=='Ours' else ref,col,m])
        for row,stat in enumerate(('p50_us','p95_us'),len(columns)+1):
            ws.write_number(row,j+len(names),float(times['first32' if m=='Ours' else ref,component,m][stat]))
    if with_plot:
        chart = book.add_chart({'type':'column'})
        for secondary in (False,True):
            for j,m in enumerate(names,1):
                col=j+(len(names) if secondary else 0)
                opt={'name':['Data',0,col], 'categories':['Data',1,0,len(groups),0],
                     'values':['Data',1,col,len(groups),col], 'y2_axis':secondary, 'gap':100,
                     'fill':{'color':'#'+colors[m]}, 'border':{'color':'#41464A','width':.3}}
                if m in patterns:
                    opt['pattern']={'pattern':patterns[m],'bg_color':'#'+colors[m],'fg_color':'#'+hatchcolor[m]}
                chart.add_series(opt)
        chart.set_chartarea({'border':{'none':True},'fill':{'none':True}})
        chart.set_plotarea({'border':{'color':'#464A4C','width':.55},'fill':{'color':'white'}})
        chart.set_title({'none':True})
        chart.set_legend({'none':True})
        chart.set_x_axis(dict(axisstyle,label_position='low',major_tick_mark='none',position_axis='between',
                              num_font=dict(font,rotation=0)))
        chart.set_x2_axis({'visible':False,'position_axis':'between','major_tick_mark':'none','label_position':'none'})
        chart.set_y_axis(dict(axisstyle,name='Compression ratio',min=0,max=1.5 if ts else 12,
                              major_unit=.5 if ts else 4,num_format='0.0' if ts else '0'))
        chart.set_y2_axis(dict(axisstyle,name='Top-k time (µs)',min=0,max=1250 if ts else 6000,
                               major_unit=250 if ts else 1000,num_format='0'))
        ws.insert_chart('Q1',chart)
    book.close()
    return stream.getvalue()

panels=[]
for k,(component,caption,x,w) in enumerate([('timestamps','Timestamps',.005,3.16),('values','Values',3.18,3.95)]):
    y=.27
    title(x,y,w,.22,f'({"ab"[k]}) {caption}',10.3,True,PP_ALIGN.CENTER)
    data=CategoryChartData();data.categories=['A'];data.add_series('Ratio',[1])
    slide.shapes.add_chart(XL_CHART_TYPE.COLUMN_CLUSTERED, Inches(x), Inches(y+.23), Inches(w), Inches(1.58),data)
    panels.append(('previous',component))
for effect in slide._element.xpath('.//a:effectRef'):
    effect.set('idx','0')
stub=BytesIO()
pres.save(stub)
changes={}
with ZipFile(stub) as ppt:
    for i,(ref,component) in enumerate(panels,1):
        chart_path=f'ppt/charts/chart{i}.xml'
        old=ET.fromstring(ppt.read(chart_path))
        with ZipFile(BytesIO(panel_workbook(ref,component,True))) as xls:
            chart=ET.fromstring(xls.read('xl/charts/chart1.xml'))
        ext=old.find('c:externalData',ns)
        chart.append(deepcopy(ext))
        changes[chart_path]=ET.tostring(chart,encoding='UTF-8',xml_declaration=True,standalone=True)
        rels=ET.fromstring(ppt.read(f'ppt/charts/_rels/chart{i}.xml.rels'))
        target=rels[0].get('Target').split('/')[-1]
        changes['ppt/embeddings/'+target]=panel_workbook(ref,component,False)
    with ZipFile(OUT/'fig_encoding_space.pptx','w') as output:
        for item in ppt.infolist():
            output.writestr(item,changes.get(item.filename,ppt.read(item.filename)))
normalize_result_figure_fonts(OUT/'fig_encoding_space.pptx', 873)
