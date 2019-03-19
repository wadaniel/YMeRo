#!/usr/bin/env python

import ymero as ymr
import numpy as np
import argparse

parser = argparse.ArgumentParser()
parser.add_argument('--axes', dest='axes', type=float, nargs=3)
parser.add_argument('--coords', dest='coords', type=str)

parser.add_argument('--constForce', action='store_true')
parser.add_argument('--constTorque', action='store_true')
parser.add_argument('--withMesh', action='store_true')
parser.set_defaults(constForce=False)
parser.set_defaults(constTorque=False)
parser.set_defaults(withMesh=False)

args = parser.parse_args()

dt   = 0.001
axes = tuple(args.axes)

ranks  = (1, 1, 1)
domain = (16, 8, 8)

u = ymr.ymero(ranks, domain, dt, debug_level=3, log_filename='log')

com_q = [[0.5 * domain[0], 0.5 * domain[1], 0.5 * domain[2],   1., 0, 0, 0]]
coords = np.loadtxt(args.coords).tolist()

if args.withMesh:
    import trimesh
    ell = trimesh.creation.icosphere(subdivisions=2, radius = 1.0)
    for i in range(3):
        ell.vertices[:,i] *= axes[i]
    mesh = ymr.ParticleVectors.Mesh(ell.vertices.tolist(), ell.faces.tolist())
    pvEllipsoid = ymr.ParticleVectors.RigidEllipsoidVector('ellipsoid', mass=1, object_size=len(coords), semi_axes=axes, mesh=mesh)
else:
    pvEllipsoid = ymr.ParticleVectors.RigidEllipsoidVector('ellipsoid', mass=1, object_size=len(coords), semi_axes=axes)

icEllipsoid = ymr.InitialConditions.Rigid(com_q=com_q, coords=coords)
vvEllipsoid = ymr.Integrators.RigidVelocityVerlet("ellvv")

u.registerParticleVector(pv=pvEllipsoid, ic=icEllipsoid)
u.registerIntegrator(vvEllipsoid)
u.setIntegrator(vvEllipsoid, pvEllipsoid)

ovStats = ymr.Plugins.createDumpObjectStats("objStats", ov=pvEllipsoid, dump_every=500, path="stats")
u.registerPlugins(ovStats)

if args.constForce:
    addForce = ymr.Plugins.createAddForce("addForce", pvEllipsoid, force=(1., 0., 0.))
    u.registerPlugins(addForce)

if args.constTorque:
    addTorque = ymr.Plugins.createAddTorque("addTorque", pvEllipsoid, torque=(0., 0., 1.0))
    u.registerPlugins(addTorque)

if args.withMesh:
    mdump = ymr.Plugins.createDumpMesh("mesh_dump", pvEllipsoid, 1000, path="ply/")
    u.registerPlugins(mdump)

u.run(10000)


# nTEST: rigids.constForce
# set -eu
# cd rigids
# rm -rf stats rigid.out.txt
# f="pos.txt"
# rho=8.0; ax=2.0; ay=1.0; az=1.0
# cp ../../data/ellipsoid_coords_${rho}_${ax}_${ay}_${az}.txt $f
# ymr.run --runargs "-n 2" ./forceTorque.py --axes $ax $ay $az --coords $f --constForce > /dev/null
# cat stats/ellipsoid.txt | awk '{print $2, $10, $3}' > rigid.out.txt

# nTEST: rigids.constTorque
# set -eu
# cd rigids
# rm -rf stats rigid.out.txt
# f="pos.txt"
# rho=8.0; ax=2.0; ay=1.0; az=1.0
# cp ../../data/ellipsoid_coords_${rho}_${ax}_${ay}_${az}.txt $f
# ymr.run --runargs "-n 2" ./forceTorque.py --axes $ax $ay $az --coords $f --constTorque > /dev/null
# cat stats/ellipsoid.txt | awk '{print $2, $15, $9}' > rigid.out.txt

# sTEST: rigids.constTorque.withMesh
# set -eu
# cd rigids
# rm -rf stats rigid.out.txt
# f="pos.txt"
# rho=8.0; ax=2.0; ay=1.0; az=1.0
# cp ../../data/ellipsoid_coords_${rho}_${ax}_${ay}_${az}.txt $f
# ymr.run --runargs "-n 2" ./forceTorque.py --axes $ax $ay $az --coords $f --constTorque --withMesh > /dev/null
# cat stats/ellipsoid.txt | awk '{print $2, $15, $9}' > rigid.out.txt
