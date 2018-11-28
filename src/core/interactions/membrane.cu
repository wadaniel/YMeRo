#include "membrane.h"

#include <core/utils/cuda_common.h>
#include <core/utils/kernel_launch.h>
#include <core/celllist.h>
#include <core/pvs/membrane_vector.h>
#include <core/pvs/views/ov.h>

#include "membrane/interactions_kernels.h"
#include "membrane/bending_juelicher.h"
#include "membrane/bending_kantor.h"

#include <cmath>
#include <random>

/**
 * Provide mapping from the model parameters to the parameters
 * used on GPU to compute the forces.
 *
 * Scaling accorging to the Mesh properties is performed, and some
 * thing are pre-computed.
 *
 * @param p model parameters
 * @param m RBC membrane mesh
 * @return parameters to be passed to GPU kernels
 */
static GPU_RBCparameters setParams(MembraneParameters& p, Mesh *m, float t)
{
    GPU_RBCparameters devP;

    devP.gammaC = p.gammaC;
    devP.gammaT = p.gammaT;

    devP.area0 = p.totArea0 / m->getNtriangles();
    devP.totArea0 = p.totArea0;
    devP.totVolume0 = p.totVolume0;

    devP.x0   = p.x0;
    devP.ks   = p.ks;
    devP.mpow = p.mpow;
    devP.l0 = sqrt(devP.area0 * 4.0 / sqrt(3.0));    

    devP.ka0 = p.ka / p.totArea0;
    devP.kv0 = p.kv / (6.0*p.totVolume0);
    devP.kd0 = p.kd;

    devP.fluctuationForces = p.fluctuationForces;

    if (devP.fluctuationForces) {
        int v = *((int*)&t);
        std::mt19937 gen(v);
        std::uniform_real_distribution<float> udistr(0.001, 1);
        devP.seed = udistr(gen);
        devP.sigma_rnd = sqrt(2 * p.kbT * p.gammaC / p.dt);
    }
    
    return devP;
}

static bendingKantor::GPU_BendingParams setKantorBendingParams(MembraneParameters& p)
{
    bendingKantor::GPU_BendingParams devP;
    
    devP.cost0kb = cos(p.theta / 180.0 * M_PI) * p.kb;
    devP.sint0kb = sin(p.theta / 180.0 * M_PI) * p.kb;

    return devP;
}

static bendingJuelicher::GPU_BendingParams setJuelicherBendingParams(MembraneParameters& p)
{
    bendingJuelicher::GPU_BendingParams devP;

    devP.kb = p.kb;
    devP.H0 = 0.0; // TODO

    return devP;
}

InteractionMembrane::InteractionMembrane(
        std::string name, MembraneParameters parameters, bool stressFree, float growUntil ) :
    Interaction(name, 1.0f), parameters(parameters), stressFree(stressFree),
    scaleFromTime( [growUntil] (float t) { return min(1.0f, 0.5f + 0.5f * (t / growUntil)); } )
{}

InteractionMembrane::~InteractionMembrane() = default;

/**
 * Require that \p pv1 and \p pv2 are the same and are instances
 * of MembraneVector
 */
void InteractionMembrane::setPrerequisites(ParticleVector* pv1, ParticleVector* pv2)
{
    if (pv1 != pv2)
        die("Internal RBC forces can't be computed between two different particle vectors");

    auto ov = dynamic_cast<MembraneVector*>(pv1);
    if (ov == nullptr)
        die("Internal RBC forces can only be computed with RBCs");

    ov->requireDataPerObject<float2>("area_volumes", false);


    if (bendingType == BendingType::Juelicher) {
        ov->requireDataPerObject<float>("lenThetaTot", false);

        ov->requireDataPerParticle<float>("areas", false);
        ov->requireDataPerParticle<float>("lenThetas", false);
        ov->requireDataPerParticle<float>("meanCurvatures", false);
    }
}

void InteractionMembrane::bendingKantor(MembraneParameters parameters, MembraneVector *ov, MembraneMeshView mesh, cudaStream_t stream)
{
    OVview view(ov, ov->local());

    const int nthreads = 128;
    const int blocks = getNblocks(view.size, nthreads);

    auto devParams = setKantorBendingParams(parameters);

    SAFE_KERNEL_LAUNCH(
            bendingKantor::computeBendingForces,
            blocks, nthreads, 0, stream,
            view, mesh, devParams );
}

void InteractionMembrane::bendingJuelicher(MembraneParameters parameters, MembraneVector *ov, MembraneMeshView mesh, cudaStream_t stream)
{
    ov->local()->extraPerObject.getData<float>("lenThetaTot")->clearDevice(stream);

    ov->local()->extraPerParticle.getData<float>("areas")->clearDevice(stream);
    ov->local()->extraPerParticle.getData<float>("lenThetas")->clearDevice(stream);

    OVviewWithJuelicherQuants view(ov, ov->local());
    
    const int nthreads = 128;
    const int blocks = getNblocks(view.size, nthreads);
    
    SAFE_KERNEL_LAUNCH(
            bendingJuelicher::lenThetaPerVertex,
            blocks, nthreads, 0, stream,
            view, mesh );

    SAFE_KERNEL_LAUNCH(
            bendingJuelicher::computeLocalAndGlobalCurvatures,
            view.nObjects, nthreads, 0, stream,
            view, mesh );
    

    auto devParams = setJuelicherBendingParams(parameters);
    
    SAFE_KERNEL_LAUNCH(
            bendingJuelicher::computeBendingForces,
            blocks, nthreads, 0, stream,
            view, mesh, devParams );
}

/**
 * Compute the membrane forces.
 *
 * First call the computeAreaAndVolume() kernel to compute area
 * and volume of each cell, then use these data to calculate the
 * forces themselves by calling computeMembraneForces() kernel
 */
void InteractionMembrane::regular(ParticleVector* pv1, ParticleVector* pv2, CellList* cl1, CellList* cl2, const float t, cudaStream_t stream)
{
    auto ov = dynamic_cast<MembraneVector*>(pv1);

    if (ov->objSize != ov->mesh->getNvertices())
        die("Object size of '%s' (%d) and number of vertices (%d) mismatch",
            ov->name.c_str(), ov->objSize, ov->mesh->getNvertices());

    debug("Computing internal membrane forces for %d cells of '%s'",
          ov->local()->nObjects, ov->name.c_str());

    auto currentParams = parameters;
    float scale = scaleFromTime(t);
    currentParams.totArea0   *= scale*scale;
    currentParams.totVolume0 *= scale*scale*scale;
    currentParams.kbT *= scale*scale;
    currentParams.kb  *= scale*scale;
    currentParams.ks  *= scale*scale;

    currentParams.gammaC *= scale;
    currentParams.gammaT *= scale;

    OVviewWithAreaVolume view(ov, ov->local());
    MembraneMeshView mesh(static_cast<MembraneMesh*>(ov->mesh.get()));
    ov->local()->extraPerObject.getData<float2>("area_volumes")->clearDevice(stream);
    
    const int nthreads = 128;
    SAFE_KERNEL_LAUNCH(
            computeAreaAndVolume,
            view.nObjects, nthreads, 0, stream,
            view, mesh );


    const int blocks = getNblocks(view.size, nthreads);

    auto devParams = setParams(currentParams, ov->mesh.get(), t);
    
    if (stressFree)
        SAFE_KERNEL_LAUNCH(
                computeMembraneForces<true>,
                blocks, nthreads, 0, stream,
                view, mesh, devParams );
    else
        SAFE_KERNEL_LAUNCH(
                computeMembraneForces<false>,
                blocks, nthreads, 0, stream,
                view, mesh, devParams );

    if (bendingType == BendingType::Kantor)
        bendingKantor(currentParams, ov, mesh, stream);
    else if (bendingType == BendingType::Juelicher)
        bendingJuelicher(currentParams, ov, mesh, stream);
}

void InteractionMembrane::halo   (ParticleVector* pv1, ParticleVector* pv2, CellList* cl1, CellList* cl2, const float t, cudaStream_t stream)
{
    debug("Not computing internal RBC forces between local and halo RBCs of '%s'", pv1->name.c_str());
}



