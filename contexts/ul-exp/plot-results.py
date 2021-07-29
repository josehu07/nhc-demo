#!/usr/bin/python3

#
# Plot over time all the .txt benchmark results under result/.
#
# Usage: python3 plot-results.py
#


import matplotlib
matplotlib.use('Agg')

import matplotlib.pyplot as plt
import os


resfiles = []
for filename in os.listdir("./result/"):
    if filename.startswith("bench-") and filename.endswith(".txt"):
        resfiles.append("result/" + filename)


for filename in resfiles:
    intensity = filename[filename.find("-int")+4:filename.find("-read")]
    read_percentage = filename[filename.find("-read")+5:filename.find("-hit")]
    hit_ratio = filename[filename.find("-hit")+4:filename.rfind("-")]
    mode = filename[filename.rfind("-")+1:filename.find(".txt")]

    num_reqs = []
    times = []
    miss_ratios = []
    load_admits = []
    cache_tps = []
    core_tps = []
    workload_change_time = None

    with open(filename) as resfile:
        for line in resfile.readlines():
            line = line.strip()

            if line.startswith("..."):
                num_req = int(line[line.find("#")+1:line.find(" @")])
                time = float(line[line.find("@ ")+2:line.find(" ms")])
                miss_ratio = float(line[line.find("miss_ratio = ")+13:line.find(", load_admit")])
                load_admit = float(line[line.find("load_admit = ")+13:line.find(", cache")])
                cache_tp = float(line[line.find("cache_tp = ")+11:line.find(", core")])
                core_tp = float(line[line.find("core_tp = ")+10:])

                num_reqs.append(num_req)
                times.append(time)
                miss_ratios.append(miss_ratio)
                load_admits.append(load_admit)
                cache_tps.append(cache_tp / 1024.0)     # MiB/s
                core_tps.append(core_tp / 1024.0)       # MiB/s

            # elif line.startswith("Workload #2"):
            #     workload_change_time = times[-1]

    # print(num_reqs)
    # print(times)
    # print(miss_ratios)
    # print(load_admits)
    # print(cache_tps)
    # print(core_tps)
    # print(workload_change_time)


    #
    # Set these according to experiment setup.
    #
    
    # miss_ratio_ylim = (-0.1, 0.7)
    # load_admit_ylim = (0.1, 1.1)
    # throughput_ylim = (-1., 100.)


    fig = plt.figure(filename, constrained_layout=True)
    gs = fig.add_gridspec(5, 1)

    ax0 = fig.add_subplot(gs[0,:])
    ax0.plot(times, num_reqs, color='y', label="#4K-Reqs")
    ax0.set_title("#4K-Reqs")

    ax1 = fig.add_subplot(gs[1,:])
    ax1.plot(times, miss_ratios, color='k', label="miss_ratio")
    # ax1.set_ylim(miss_ratio_ylim)
    ax1.set_title("miss_ratio")

    ax2 = fig.add_subplot(gs[2,:])
    ax2.plot(times, load_admits, color='r', label="load_admit")
    # ax2.set_ylim(load_admit_ylim)
    ax2.set_title("load_admit")

    ax3 = fig.add_subplot(gs[3:5,:])
    ax3.plot(times, [tp1+tp2 for tp1, tp2 in zip(cache_tps, core_tps)], color='g', label="Total")
    ax3.plot(times, cache_tps, color='c', label="Cache")
    ax3.plot(times, core_tps,  color='b', label="Core")
    # ax3.set_ylim(throughput_ylim)
    ax3.set_title("Throughput")
    ax3.set_ylabel("(MiB/s)")
    ax3.legend(loc='center left')

    ax3.set_xlabel("Time (ms)")

    fig.suptitle("Intensity = "+intensity+" #4K-Reqs/s,\nread percentage = "+read_percentage
                 +"%, hit ratio = "+hit_ratio+"%, mode = "+mode)
    
    plt.savefig("result/bench-int"+intensity+"-read"+read_percentage+"-hit"+hit_ratio+"-"+mode+".png")
    plt.close()
