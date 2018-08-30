#!/usr/bin/env python

import argparse
import udevicex as udx

parser = argparse.ArgumentParser()
parser.add_argument('--numdensity', dest='numdensity', type=float)
parser.add_argument('--axes', dest='axes', type=float, nargs=3)
args = parser.parse_args()

dt = 0.001
axes = tuple(args.axes)

ranks  = (1, 1, 1)
fact = 3
domain = (fact*axes[0], fact*axes[1], fact*axes[2])

u = udx.udevicex(ranks, domain, debug_level=8, log_filename='log')

pv = udx.ParticleVectors.ParticleVector('pv', mass = 1)
ic = udx.InitialConditions.Uniform(density=args.numdensity)
u.registerParticleVector(pv=pv, ic=ic)

dpd = udx.Interactions.DPD('dpd', 1.0, a=10.0, gamma=10.0, kbt=0.5, dt=dt, power=0.5)
u.registerInteraction(dpd)
u.setInteraction(dpd, pv, pv)

vv = udx.Integrators.VelocityVerlet('vv', dt=dt)
u.registerIntegrator(vv)
u.setIntegrator(vv, pv)


coords = [[-axes[0], -axes[1], -axes[2]],
          [ axes[0],  axes[1],  axes[2]]]
com_q = [[0.5 * domain[0], 0.5 * domain[1], 0.5 * domain[2],   1., 0, 0, 0]]

fakeOV = udx.ParticleVectors.RigidEllipsoidVector('OV', mass=1, object_size=len(coords), semi_axes=axes)
fakeIc = udx.InitialConditions.Rigid(com_q=com_q, coords=coords)
u.registerParticleVector(pv=fakeOV, ic=fakeIc)

belongingChecker = udx.BelongingCheckers.Ellipsoid("ellipsoidChecker")

u.registerObjectBelongingChecker(belongingChecker, fakeOV)

pvEllipsoid = u.applyObjectBelongingChecker(belongingChecker, pv, correct_every=500, inside="frozenEllipsoid")
u.setIntegrator(vv, pvEllipsoid)

xyz = udx.Plugins.createDumpXYZ('xyz', pvEllipsoid, 2, "xyz/")
u.registerPlugins(xyz)

u.run(5)

# sTEST: rigids.createEllipsoid
# cd rigids
# rm -rf xyz
# udx.run --runargs "-n 2" ./createEllipsoid.py --axes 3.0 3.0 3.0 --numdensity 8
# echo TODO > xyz.out.txt
