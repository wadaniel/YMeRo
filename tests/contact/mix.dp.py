#!/usr/bin/env python

import sys, argparse
import numpy as np
import ymero as ymr

sys.path.append("..")
from common.membrane_params import lina_parameters

parser = argparse.ArgumentParser()
parser.add_argument('--density', dest='density', type=float)
parser.add_argument('--axes', dest='axes', type=float, nargs=3)
parser.add_argument('--coords', dest='coords', type=str)
parser.add_argument('--bounceBack', dest='bounceBack', action='store_true')
parser.add_argument('--substep', dest='substep', action='store_true')
parser.set_defaults(bounceBack=False)
parser.set_defaults(substep=False)
args = parser.parse_args()

tend = 10.0
dt = 0.001
a = 1.0

if args.substep:
    substeps = 10
    dt = dt * substeps

ranks  = (1, 1, 1)
domain = (12, 8, 10)

u = ymr.ymero(ranks, domain, dt, debug_level=3, log_filename='log')

pv_flu = ymr.ParticleVectors.ParticleVector('solvent', mass = 1)
ic_flu = ymr.InitialConditions.Uniform(density=args.density)
u.registerParticleVector(pv=pv_flu, ic=ic_flu)


mesh_rbc = ymr.ParticleVectors.MembraneMesh("rbc_mesh.off")
pv_rbc   = ymr.ParticleVectors.MembraneVector("rbc", mass=1.0, mesh=mesh_rbc)

com_q_rbc = [[2.0, 5.0, 5.0,   1.0, np.pi/2, np.pi/3, 0.0],
             [6.0, 3.0, 5.0,   1.0, np.pi/2, np.pi/3, 0.0]]

com_q_rig = [[4.0, 4.0, 5.0,   1.0, np.pi/2, np.pi/3, 0.0]]

ic_rbc   = ymr.InitialConditions.Membrane(com_q_rbc)
u.registerParticleVector(pv_rbc, ic_rbc)

dpd = ymr.Interactions.DPD('dpd', 1.0, a=10.0, gamma=10.0, kbt=0.01, power=0.25)
cnt = ymr.Interactions.LJ('cnt', 1.0, epsilon=0.35, sigma=0.8, max_force=400.0, object_aware=False)

prm_rbc = lina_parameters(1.0)    
int_rbc = ymr.Interactions.MembraneForces("int_rbc", "wlc", "Kantor", **prm_rbc, stress_free=True)

coords = np.loadtxt(args.coords).tolist()
pv_ell = ymr.ParticleVectors.RigidEllipsoidVector('ellipsoid', mass=1, object_size=len(coords), semi_axes=args.axes)
ic_ell = ymr.InitialConditions.Rigid(com_q=com_q_rig, coords=coords)
vv_ell = ymr.Integrators.RigidVelocityVerlet("ellvv")

u.registerParticleVector(pv=pv_ell, ic=ic_ell)
u.registerIntegrator(vv_ell)
u.setIntegrator(vv_ell, pv_ell)

u.registerInteraction(dpd)
u.registerInteraction(cnt)

if args.substep:
    integrator = ymr.Integrators.SubStepMembrane('substep_membrane', substeps, int_rbc)
    u.registerIntegrator(integrator)
    u.setIntegrator(integrator, pv_rbc)
else:
    vv = ymr.Integrators.VelocityVerlet('vv')
    u.registerInteraction(int_rbc)
    u.setInteraction(int_rbc, pv_rbc, pv_rbc)
    u.registerIntegrator(vv)
    u.setIntegrator(vv, pv_rbc)

u.setInteraction(dpd, pv_flu, pv_flu)
u.setInteraction(dpd, pv_flu, pv_rbc)
u.setInteraction(dpd, pv_flu, pv_ell)
u.setInteraction(cnt, pv_rbc, pv_rbc)
u.setInteraction(cnt, pv_rbc, pv_ell)
u.setInteraction(cnt, pv_ell, pv_ell)

vv_dp = ymr.Integrators.VelocityVerlet_withPeriodicForce('vv_dp', force=a, direction='x')
u.registerIntegrator(vv_dp)
u.setIntegrator(vv_dp, pv_flu)

belongingChecker = ymr.BelongingCheckers.Ellipsoid("ellipsoidChecker")

u.registerObjectBelongingChecker(belongingChecker, pv_ell)
u.applyObjectBelongingChecker(belongingChecker, pv=pv_flu, correct_every=0, inside="none", outside="")

if args.bounceBack:
    bb = ymr.Bouncers.Ellipsoid("bounceEllipsoid")
    u.registerBouncer(bb)
    u.setBouncer(bb, pv_ell, pv_flu)


debug = 0
if debug:
    dump_every=(int)(0.15/dt)

    dump_mesh = ymr.Plugins.createDumpMesh("mesh_dump", pv_rbc, dump_every, "ply/")
    u.registerPlugins(dump_mesh)

    ovStats = ymr.Plugins.createDumpObjectStats("objStats", ov=pv_ell, dump_every=dump_every, path="stats")
    u.registerPlugins(ovStats)

    xyz = ymr.Plugins.createDumpXYZ('xyz', pv_ell, dump_every, "xyz/")
    u.registerPlugins(xyz)


nsteps = (int) (tend/dt)
u.run(nsteps)

if pv_rbc is not None:
    rbc_pos = pv_rbc.getCoordinates()
    np.savetxt("pos.rbc.txt", rbc_pos)


# nTEST: contact.mix.dp
# cd contact
# rm -rf pos.rbc.out.txt pos.rbc.txt
# f="pos.txt"
# rho=8.0; ax=2.0; ay=1.0; az=1.0
# cp ../../data/ellipsoid_coords_${rho}_${ax}_${ay}_${az}.txt $f
# cp ../../data/rbc_mesh.off .
# ymr.run --runargs "-n 2" ./mix.dp.py --density $rho --axes $ax $ay $az --coords $f > /dev/null
# mv pos.rbc.txt pos.rbc.out.txt 
