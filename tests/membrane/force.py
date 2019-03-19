#!/usr/bin/env python

import numpy as np
import ymero as ymr
from common.membrane_params import lina_parameters

dt = 0.001

ranks  = (1, 1, 1)
domain = (12, 8, 10)

u = ymr.ymero(ranks, domain, dt, debug_level=3, log_filename='log')

mesh_rbc = ymr.ParticleVectors.MembraneMesh("rbc_mesh.off")
pv_rbc   = ymr.ParticleVectors.MembraneVector("rbc", mass=1.0, object_size=498, mesh=mesh_rbc)
ic_rbc   = ymr.InitialConditions.Membrane([[6.0, 4.0, 5.0,   1.0, 0.0, 0.0, 0.0]])
u.registerParticleVector(pv_rbc, ic_rbc)

prm_rbc = ymr.Interactions.MembraneParameters()
prm_bending_rbc = ymr.Interactions.KantorBendingParameters()

prm_rbc = lina_parameters(1.0, args.fluctuations)    
int_rbc = ymr.Interactions.MembraneForces("int_rbc", "wlc", "Kantor", **prm_rbc)

vv = ymr.Integrators.VelocityVerlet('vv')
u.registerIntegrator(vv)
u.setIntegrator(vv, pv_rbc)
u.registerInteraction(int_rbc)
u.setInteraction(int_rbc, pv_rbc, pv_rbc)

u.run(2)

if pv_rbc:
    rbc_forces = pv_rbc.getForces()
    np.savetxt("forces.rbc.txt", rbc_forces)

# sTEST: membrane.force
# cd membrane
# cp ../../data/rbc_mesh.off .
# ymr.run --runargs "-n 1" ./force.py > /dev/null
# mv forces.rbc.txt forces.rbc.out.txt 
