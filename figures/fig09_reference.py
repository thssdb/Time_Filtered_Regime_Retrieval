"""Create a single-column editable figure directly from the pooled observations."""
from pathlib import Path
import csv, math, sys
from pptx import Presentation
from pptx.chart.data import CategoryChartData
from pptx.dml.color import RGBColor
from pptx.enum.chart import XL_CHART_TYPE,XL_MARKER_STYLE,XL_TICK_MARK
from pptx.enum.shapes import MSO_CONNECTOR,MSO_SHAPE
from pptx.enum.dml import MSO_LINE_DASH_STYLE
from pptx.enum.text import MSO_ANCHOR,PP_ALIGN
from pptx.oxml.xmlchemy import OxmlElement
from pptx.util import Inches,Pt
from paths import INPUT, OUTPUT
from paper_result_figure_fonts import normalize_result_figure_fonts
with (INPUT/'value_difference_reference_sweep.csv').open() as f:
    data={(row['method'],int(row['interval'])):row for row in csv.DictReader(f) if row["db"]=="PRSOV"}
groups=(2,4,8,16,32); arms=[('first','Group-first segment','3568A8',XL_MARKER_STYLE.CIRCLE),('previous','Previous segment','C86A21',XL_MARKER_STYLE.SQUARE)]
rgb=RGBColor.from_string; font='Linux Biolinum O'
p=Presentation();p.slide_width=Inches(3.48);p.slide_height=Inches(1.80)
s=p.slides.add_slide(p.slide_layouts[6])
def text(x,y,w,h,msg,bold=False,left=False):
    tf=s.shapes.add_textbox(*map(Inches,(x,y,w,h))).text_frame
    tf.margin_top=tf.margin_bottom=tf.margin_left=tf.margin_right=0
    tf.vertical_anchor=MSO_ANCHOR.MIDDLE;tf.word_wrap=False
    para=tf.paragraphs[0];para.alignment=PP_ALIGN.LEFT if left else PP_ALIGN.CENTER
    run=para.add_run();run.text=msg;run.font.name=font;run.font.size=Pt(9);run.font.bold=bold;run.font.color.rgb=rgb('202D35')
for x,(method,label,color,marker) in zip((.14,1.76),arms):
    v=s.shapes.add_connector(MSO_CONNECTOR.STRAIGHT, Inches(x), Inches(.10), Inches(x+.22), Inches(.10));v.line.color.rgb=rgb(color);v.line.width=Pt(1)
    if method=='previous':v.line.dash_style=MSO_LINE_DASH_STYLE.DASH
    mark=s.shapes.add_shape(MSO_SHAPE.OVAL if method=='first' else MSO_SHAPE.RECTANGLE,*map(Inches,(x+.084,.066,.060,.060)))
    mark.fill.solid();mark.fill.fore_color.rgb=rgb(color);mark.line.color.rgb=rgb(color);mark.line.width=Pt(.85)
    text(x+.27,.0,1.48,.20,label,left=True)
for index,(metric,title) in enumerate([('compression_ratio','(a) Compression ratio (×)'),('query_p50_us','(b) Query p50 (µs)')]):
    x=.01+1.73*index
    text(x,.22,1.71,.20,title,True)
    d=CategoryChartData();d.categories=[str(g) for g in groups]
    allvalues=[]
    for method,label,_,_ in arms:
        vals=[float(data[method,g][metric]) for g in groups];allvalues+=vals;d.add_series(label,vals)
    chart=s.shapes.add_chart(XL_CHART_TYPE.LINE_MARKERS,*map(Inches,(x,.44,1.72,1.12)),d).chart
    chart.has_legend=chart.has_title=False
    for ser,(method,_,color,marker) in zip(chart.series,arms):
        ser.smooth=False;ser.format.line.color.rgb=rgb(color);ser.format.line.width=Pt(1.1)
        if method=='previous':ser.format.line.dash_style=MSO_LINE_DASH_STYLE.DASH
        ser.marker.style=marker;ser.marker.size=6 if method=='first' else 4
        ser.marker.format.fill.solid();ser.marker.format.fill.fore_color.rgb=rgb(color)
        ser.marker.format.line.color.rgb=rgb(color);ser.marker.format.line.width=Pt(.8)
    ca,va=chart.category_axis,chart.value_axis
    # Aim for 3--5 labeled ticks and reserve a margin above and below the markers.
    span=max(allvalues)-min(allvalues)
    unit=next(v for v in [a*10**i for i in range(-2,5) for a in (1,2,5)] if v>=span/4)
    va.minimum_scale=max(0,math.floor((min(allvalues)-unit*.08)/unit)*unit)
    va.maximum_scale=math.ceil((max(allvalues)+unit*.08)/unit)*unit;va.major_unit=unit
    va.tick_labels.number_format='0.0' if unit<1 else '0'
    for ax in (ca,va):
        ax.has_major_gridlines=False;ax.major_tick_mark=XL_TICK_MARK.NONE;ax.minor_tick_mark=XL_TICK_MARK.NONE
        ax.tick_labels.font.name=font;ax.tick_labels.font.size=Pt(9);ax.tick_labels.font.color.rgb=rgb('202D35')
        ax.format.line.color.rgb=rgb('69747B');ax.format.line.width=Pt(.5)
    ca.tick_labels.offset=38
    skip=OxmlElement('c:tickLblSkip');skip.set('val','1');ca._element.insert_element_before(skip,'c:tickMarkSkip','c:noMultiLvlLbl','c:extLst')
text(.20,1.59,3.08,.18,'Restart interval (vectors)')
for elem in s._element.xpath('.//a:effectRef'):elem.set('idx','0')
s.notes_slide.notes_text_frame.text='Four PRSOV partitions. Value-only compression ratios use eight bytes per scalar. End-to-end p50 pools 1,152 measurements per point at the 50% window. Both panels use group size in vectors. Original decimal values are differenced against the group-first or previous vector at the same coordinate. Both use the group-first residual key in the second transform. The overlapping ratio markers at two denote identical sizes.'
out=OUTPUT/'fig_rice_reference_by_database.pptx'
p.save(out);normalize_result_figure_fonts(out,911)

# Mark the selected First-32 configuration on both panels.
p=Presentation(out)
for shape in p.slides[0].shapes:
    if shape.has_chart:
        mark=shape.chart.series[0].points[4].marker
        mark.style=XL_MARKER_STYLE.CIRCLE
        mark.size=9
        mark.format.fill.background()
        mark.format.line.color.rgb=RGBColor.from_string('103D4A')
        mark.format.line.width=Pt(1.35)
p.save(out)
