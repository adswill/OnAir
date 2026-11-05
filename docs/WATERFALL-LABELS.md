# Names on the spectrum and the waterfall

Station and service names are drawn over the signals (band plan, frequency database, FM/DAB/DVB scans, what was heard on a channel, your memory
channels). The toggle and the per-source choices are in the waterfall's menu; when the view is wide the labels thin out (the strongest priority
wins and neighbours that would overlap are dropped), and zooming in brings back the detail and the nominal bandwidth bars. A click on a name tunes
to it and opens its mode. Code: `app/gfx/autolabel.cpp`, layout in `app/gfx/autolabel_logic.h`, test `autolabel_logic`.
