"""A data-independent rendering of the forecasting workflow with synthetic curves."""
import matplotlib.pyplot as plt
import numpy as np
from paths import OUTPUT

x=np.arange(1,19)
fig, axes=plt.subplots(1,2,figsize=(6.8,2.15),gridspec_kw={"width_ratios":[1.4,1]})
for shift,label,style in [(0.2,"MP","-"),(0.7,"IP","--"),(1.1,"N2",":")]:
    y=1.5*np.sin(x/6+shift)+x/14
    axes[0].plot(x,(y-y.min())/np.ptp(y),style,label=label,linewidth=1.35)
axes[0].set(title="Synthetic takeoff segment",xlabel="Sample position",ylabel="Scaled reading")
axes[0].legend(frameon=False,ncols=3,fontsize=8)
axes[1].set_axis_off()
steps=["Restrict history to the time window","Retrieve the top-k similar segments",
       "Read all three signals of each match","Predict the remaining MP readings"]
for i,step in enumerate(steps):
    y=.92-i*.265
    axes[1].text(.5,y,step,ha="center",va="center",fontsize=8,transform=axes[1].transAxes)
    if i<len(steps)-1:
        axes[1].annotate("",xy=(.5,y-.19),xytext=(.5,y-.07),xycoords="axes fraction",
                         arrowprops={"arrowstyle":"->","lw":.7})
fig.tight_layout()
fig.savefig(OUTPUT/"fig_seasonal_motivation.eps")
