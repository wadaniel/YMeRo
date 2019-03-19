#include "stats.h"
#include "utils/simple_serializer.h"

#include <core/datatypes.h>
#include <core/pvs/particle_vector.h>
#include <core/pvs/views/pv.h>
#include <core/simulation.h>
#include <core/utils/cuda_common.h>
#include <core/utils/kernel_launch.h>

namespace StatsKernels
{
using Stats::ReductionType;

__global__ void totalMomentumEnergy(PVview view, ReductionType *momentum, ReductionType *energy, float* maxvel)
{
    int tid = blockIdx.x * blockDim.x + threadIdx.x;

    float3 vel, myMomentum;
    float myEnergy = 0.f, myMaxIvelI;
    vel = myMomentum = make_float3(0.f);

    if (tid < view.size)
    {
        vel        = make_float3(view.particles[2*tid+1]);
        myMomentum = vel * view.mass;
        myEnergy   = dot(vel, vel) * view.mass * 0.5f;
    }
    
    myMomentum = warpReduce(myMomentum, [](float a, float b) { return a+b; });
    myEnergy   = warpReduce(myEnergy,   [](float a, float b) { return a+b; });
    
    myMaxIvelI = warpReduce(length(vel), [](float a, float b) { return max(a, b); });

    if (__laneid() == 0)
    {
        atomicAdd(momentum+0, (ReductionType)myMomentum.x);
        atomicAdd(momentum+1, (ReductionType)myMomentum.y);
        atomicAdd(momentum+2, (ReductionType)myMomentum.z);
        atomicAdd(energy,     (ReductionType)myEnergy);

        atomicMax((int*)maxvel, __float_as_int(myMaxIvelI));
    }
}
} // namespace StatsKernels
    
SimulationStats::SimulationStats(const YmrState *state, std::string name, int fetchEvery) :
    SimulationPlugin(state, name),
    fetchEvery(fetchEvery)
{
    timer.start();
}

SimulationStats::~SimulationStats() = default;

void SimulationStats::setup(Simulation *simulation, const MPI_Comm& comm, const MPI_Comm& interComm)
{
    SimulationPlugin::setup(simulation, comm, interComm);
    pvs = simulation->getParticleVectors();
}

void SimulationStats::afterIntegration(cudaStream_t stream)
{
    if (state->currentStep % fetchEvery != 0) return;

    momentum.clear(stream);
    energy  .clear(stream);
    maxvel  .clear(stream);

    nparticles = 0;
    for (auto& pv : pvs)
    {
        PVview view(pv, pv->local());

        SAFE_KERNEL_LAUNCH(
                StatsKernels::totalMomentumEnergy,
                getNblocks(view.size, 128), 128, 0, stream,
                view, momentum.devPtr(), energy.devPtr(), maxvel.devPtr() );

        nparticles += view.size;
    }

    momentum.downloadFromDevice(stream, ContainersSynch::Asynch);
    energy  .downloadFromDevice(stream, ContainersSynch::Asynch);
    maxvel  .downloadFromDevice(stream);

    needToDump = true;
}

void SimulationStats::serializeAndSend(cudaStream_t stream)
{
    if (needToDump)
    {
        float tm = timer.elapsedAndReset() / (state->currentStep < fetchEvery ? 1.0f : fetchEvery);
        waitPrevSend();
        SimpleSerializer::serialize(sendBuffer, tm, state->currentTime, state->currentStep, nparticles, momentum, energy, maxvel);
        send(sendBuffer);
        needToDump = false;
    }
}

PostprocessStats::PostprocessStats(std::string name, std::string filename) :
        PostprocessPlugin(name)
{
    if (std::is_same<Stats::ReductionType, float>::value)
        mpiReductionType = MPI_FLOAT;
    else if (std::is_same<Stats::ReductionType, double>::value)
        mpiReductionType = MPI_DOUBLE;
    else
        die("Incompatible type");

    if (filename != "")
    {
        fdump = fopen(filename.c_str(), "w");
        if (!fdump) die("Could not open file '%s'", filename.c_str());
        fprintf(fdump, "# time  kBT  vx vy vz  max(abs(v))  simulation_time_per_step(ms)\n");
    }
}

PostprocessStats::~PostprocessStats()
{
    if (fdump != nullptr) fclose(fdump);
}

void PostprocessStats::deserialize(MPI_Status& stat)
{
    TimeType currentTime;
    float realTime;
    int nparticles, currentTimeStep;
    int maxNparticles, minNparticles;

    std::vector<Stats::ReductionType> momentum, energy;
    std::vector<float> maxvel;

    SimpleSerializer::deserialize(data, realTime, currentTime, currentTimeStep, nparticles, momentum, energy, maxvel);

    MPI_Check( MPI_Reduce(&nparticles, &minNparticles, 1, MPI_INT, MPI_MIN, 0, comm) );
    MPI_Check( MPI_Reduce(&nparticles, &maxNparticles, 1, MPI_INT, MPI_MAX, 0, comm) );
    
    MPI_Check( MPI_Reduce(rank == 0 ? MPI_IN_PLACE : &nparticles,     &nparticles,     1, MPI_INT,          MPI_SUM, 0, comm) );
    MPI_Check( MPI_Reduce(rank == 0 ? MPI_IN_PLACE : energy.data(),   energy.data(),   1, mpiReductionType, MPI_SUM, 0, comm) );
    MPI_Check( MPI_Reduce(rank == 0 ? MPI_IN_PLACE : momentum.data(), momentum.data(), 3, mpiReductionType, MPI_SUM, 0, comm) );

    MPI_Check( MPI_Reduce(rank == 0 ? MPI_IN_PLACE : maxvel.data(),   maxvel.data(),   1, MPI_FLOAT,        MPI_MAX, 0, comm) );

    MPI_Check( MPI_Reduce(rank == 0 ? MPI_IN_PLACE : &realTime,       &realTime,       1, MPI_FLOAT,        MPI_MAX, 0, comm) );

    if (rank == 0)
    {
        momentum[0] /= (double)nparticles;
        momentum[1] /= (double)nparticles;
        momentum[2] /= (double)nparticles;
        const Stats::ReductionType temperature = energy[0] / ( (3/2.0)*nparticles );

        printf("Stats at timestep %d (simulation time %f):\n", currentTimeStep, currentTime);
        printf("\tOne timestep takes %.2f ms", realTime);
        printf("\tNumber of particles (total, min/proc, max/proc): %d,  %d,  %d\n", nparticles, minNparticles, maxNparticles);
        printf("\tAverage momentum: [%e %e %e]\n", momentum[0], momentum[1], momentum[2]);
        printf("\tMax velocity magnitude: %f\n", maxvel[0]);
        printf("\tTemperature: %.4f\n\n", temperature);

        if (fdump != nullptr)
        {
            fprintf(fdump, "%g %g %g %g %g %g %g\n", currentTime,
                    temperature, momentum[0], momentum[1], momentum[2], maxvel[0], realTime);
            fflush(fdump);
        }
    }
}


