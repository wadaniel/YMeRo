#!/usr/bin/env python

import ymero as ymr

dt = 0.001

ranks  = (1, 1, 1)
domain = (4, 4, 4)

u = ymr.ymero(ranks, domain, dt, debug_level=3, log_filename='log')

u.save_dependency_graph_graphml("tasks.full", current=False)

# nTEST: dump.graph.full
# cd dump
# rm -rf tasks.graphml
# ymr.run --runargs "-n 1" ./graph.full.py > /dev/null
# cat tasks.full.graphml > tasks.out.txt

