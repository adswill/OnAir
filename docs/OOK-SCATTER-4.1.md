# On / off-time scatter plot (4.1)

Each pulse of an OOK burst is one point: X is the on time (pulse length) and Y is the off time that follows (gap), in microseconds. Line codes show up as clusters, and the spread of a cluster shows the timing jitter. No symbol rate is needed.

## Where

- OOK analyser (x40 "OOK pulse", Decoded tab): the last 64 bursts of 4 or more pulses, decoded or not.
- ISM sensors, Unknown view: the OOK bursts in the list; the selected one is "current". FSK bursts are left out.

## Controls

- Bursts: current, last N (1 to 500) or all.
- Log or linear axes (microseconds).
- Copy CSV: `burst,pulse,on_us,off_us` for the bursts selected.
- Cluster centres are labelled `on/off` in microseconds on the plot and listed below with point count and jitter.

## Method (core/include/dect2/ism/ook_scatter.h, header only)

- The last pulse of every burst is dropped (its off time is the trailing gap). Above 3000 points the set is thinned evenly.
- Clustering is density based in log time: two points are neighbours when both log-times differ by 0.2 or less, a core point has at least 4 neighbours, and a cluster needs at least max(6, 5 % of the points) points and a spread (standard deviation over centre) of 20 % or less on both axes. Wider groups (chains of random points) are discarded.
- Centres are the median on and off of the cluster.
- Encoding guess from the cluster centres:
  - Manchester: on times in two groups about 1:2, off times in two groups about 1:2, on and off unit about equal, 3 or more clusters, period not constant.
  - PWM: two on-time groups at least 1.6 apart, with one off group (fixed gap) or a constant on + off period.
  - PPM: one on-time group, two off-time groups at least 1.6 apart.
  - otherwise unknown (also for no cluster or more than 6).
- A guess names the pattern, not the product: which cluster is 0 or 1 is not decided here.

## Tests

`ook_scatter` (tests/ook41/test_ook_scatter.cpp, simulated pulse trains only, no recorded signals): PWM (fixed period and fixed gap), PPM and Manchester bursts with Gaussian timing jitter of 2, 5 and 8 %, checking cluster count, centres and the guess; 20 noise-only sets plus one heavy set (50 bursts of 60 random pairs) that must give no cluster; a real signal mixed with noise bursts; burst selection, CSV rows and axis maps.

Limits: not checked on recorded signals; heavy jitter above about 15 % or very different pulse sets in one selection may merge or hide clusters.
