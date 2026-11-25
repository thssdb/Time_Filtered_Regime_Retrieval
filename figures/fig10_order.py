"""Generate the editable PRSOV storage-order and SQL system comparison figures."""
import csv
from pathlib import Path
from tempfile import TemporaryDirectory
import subprocess

from pptx import Presentation
from pptx.chart.data import CategoryChartData
from pptx.dml.color import RGBColor
from pptx.enum.chart import XL_CHART_TYPE, XL_MARKER_STYLE, XL_TICK_LABEL_POSITION
from pptx.enum.dml import MSO_LINE_DASH_STYLE
from pptx.enum.shapes import MSO_CONNECTOR
from pptx.enum.text import PP_ALIGN
from pptx.oxml.ns import qn
from pptx.oxml.xmlchemy import OxmlElement
from pptx.util import Inches, Pt

from paths import INPUT as HERE, OUTPUT as FIG
WINDOWS = list(range(10, 100, 10))
COLORS = ['334B64','BB363E']
FONT = 'Linux Biolinum O'


def label(slide, x, y, w, h, name, size=8, bold=False):
    box = slide.shapes.add_textbox(Inches(x), Inches(y), Inches(w), Inches(h))
    tf = box.text_frame
    tf.word_wrap = False
    tf.margin_left = tf.margin_right = tf.margin_top = tf.margin_bottom = 0
    p = tf.paragraphs[0]
    p.alignment = PP_ALIGN.CENTER
    p.text = name
    p.font.name = FONT
    p.font.size = Pt(size)
    p.font.bold = bold


def line_style(line, i):
    line.color.rgb = RGBColor.from_string(COLORS[i])
    line.width = Pt(1.2)
    if i == 0: line.dash_style = MSO_LINE_DASH_STYLE.DASH


def figure(filename, datafile, key, groups, titles, axes, compact=False):
    with (HERE/datafile).open() as f:
        records = list(csv.DictReader(f))
    cases = {(r[key], int(r['window'])): r for r in records if r[key] in dict(groups)}
    assert len(cases)==18 and len([r for r in records if r[key] in dict(groups)])==18
    prs = Presentation()
    prs.slide_width, prs.slide_height = Inches(3.45), Inches(1.95)
    slide = prs.slides.add_slide(prs.slide_layouts[6])
    for i,(_,name) in enumerate(groups):
        left = [0.30,1.88][i]
        shape = slide.shapes.add_connector(MSO_CONNECTOR.STRAIGHT, Inches(left), Inches(.11), Inches(left+.22), Inches(.11))
        line_style(shape.line,i)
        if compact:
            shape._element.find('.//' + qn('a:effectRef')).set('idx', '0')
        label(slide,left+.22,.023,1.16,.17,name)
    for j,(field,ytitle,low,high,step) in enumerate(axes):
        x=j*1.735
        label(slide,x,.22,1.71,.16,titles[j],bold=True)
        data = CategoryChartData()
        data.categories = [str(w) if compact or w%20==10 else '' for w in WINDOWS]
        for arm, name in groups:
            data.add_series(name,[float(cases[arm,w][field]) for w in WINDOWS])
        chart = slide.shapes.add_chart(XL_CHART_TYPE.LINE_MARKERS, Inches(x), Inches(.40), Inches(1.73), Inches(1.31),data).chart
        chart.has_legend = False
        chart.font.name, chart.font.size = FONT, Pt(7.4)
        for axis in [chart.category_axis,chart.value_axis]:
            axis.has_major_gridlines = False
            axis.tick_labels.font.name, axis.tick_labels.font.size = FONT,Pt(7.4)
            axis.tick_label_position=XL_TICK_LABEL_POSITION.LOW
            axis.format.line.color.rgb=RGBColor.from_string('616970')
        chart.value_axis.minimum_scale=low
        chart.value_axis.maximum_scale=high
        chart.value_axis.major_unit=step
        if compact:
            tick_skip = OxmlElement('c:tickLblSkip')
            tick_skip.set('val', '1')
            chart.category_axis._element.insert_element_before(tick_skip, 'c:noMultiLvlLbl')
            crossing = OxmlElement('c:crossBetween')
            crossing.set('val', 'midCat')
            chart.value_axis._element.insert_element_before(crossing, 'c:majorUnit')
            if j == 1:
                label(slide, x+.33, .51, .52, .15, ytitle, 7.8)
        else:
            chart.value_axis.has_title=True
            t=chart.value_axis.axis_title.text_frame.paragraphs[0]
            t.text=ytitle
            t.runs[0].font.name,t.runs[0].font.size=FONT,Pt(7.8)
        chart.category_axis.tick_labels.offset=15
        for i,s in enumerate(chart.series):
            line_style(s.format.line,i)
            s.marker.style = (XL_MARKER_STYLE.SQUARE,XL_MARKER_STYLE.CIRCLE)[i]
            s.marker.size=3
            s.marker.format.fill.solid()
            s.marker.format.fill.fore_color.rgb=RGBColor.from_string(('FFFFFF',COLORS[1])[i])
            s.marker.format.line.color.rgb=RGBColor.from_string(COLORS[i])
        label(slide,x+.12,1.71,1.55,.17,'Time window (%)',7.8)
    ppt = FIG/(filename+'.pptx')
    prs.save(ppt)
    with TemporaryDirectory() as tempdir:
        dest=Path(tempdir)
        subprocess.run(['soffice','-env:UserInstallation='+(dest/'profile').as_uri(),'--headless','--convert-to','pdf','--outdir',dest,ppt],check=True,stdout=subprocess.DEVNULL)
        ps=dest/(filename+'.ps')
        subprocess.run(['pdf2ps','-dLanguageLevel=3',ps.with_suffix('.pdf'),ps],check=True)
        subprocess.run(['ps2eps','-f','-l',ps],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
        (FIG/(filename+'.eps')).write_bytes(ps.with_suffix('.eps').read_bytes())


if __name__=='__main__':
    figure('fig_storage_order_ablation','storage_order_windows.csv','layout',
           [('chronological','Time order'),('similarity','Similarity order')],
           ['(a) Compression ratio','(b) Query latency'],
           [('value_cr','Compression ratio',4.8,5.8,.2),('query_p50_us','p50 (µs)',100,300,50)], compact=True)
