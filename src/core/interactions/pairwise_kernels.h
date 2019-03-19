#pragma once

#include "pairwise_interactions/type_traits.h"

#include <core/celllist.h>
#include <core/utils/cuda_common.h>
#include <core/pvs/views/pv.h>

#include <cassert>
#include <type_traits>

enum class InteractionWith
{
    Self, Other
};

enum class InteractionOut
{
    NeedAcc, NoAcc
};

enum class InteractionMode
{
    RowWise, Dilute
};

/**
 * Compute interactions between one destination particle and
 * all source particles in a given cell, defined by range of ids:
 *
 * \code
 * for (id = pstart; id < pend; id++)
 * F += interaction(dstP, Particle(cinfo.particles, id));
 * \endcode
 *
 * Also update forces for the source particles in process.
 *
 * Source particles may be from the same ParticleVector or from
 * different ones.
 *
 * @param pstart lower bound of id range of the particles to be worked on
 * @param pend  upper bound of id range
 * @param dstP destination particle
 * @param dstId destination particle local id, may be used to fetch extra
 *        properties associated with the given particle
 * @param dstFrc target force
 * @param cinfo cell-list data for source particles
 * @param rc2 squared interaction cut-off distance
 * @param interaction interaction implementation, see computeSelfInteractions()
 *
 * @tparam NeedDstAcc whether to update \p dstFrc or not. One out of
 * \p NeedDstAcc or \p NeedSrcAcc should be true.
 * @tparam NeedSrcAcc whether to update forces for source particles.
 * One out of \p NeedDstAcc or \p NeedSrcAcc should be true.
 * @tparam Self true if we're computing self interactions, meaning
 * that destination particle is one of the source particles.
 * In that case only half of the interactions contribute to the
 * forces, such that either p1 \<-\> p2 or p2 \<-\> p1 is ignored
 * based on particle ids
 */
template<InteractionOut NeedDstAcc, InteractionOut NeedSrcAcc, InteractionWith InteractWith,
         typename Interaction, typename Accumulator>
__device__ inline void computeCell(
        int pstart, int pend,
        typename Interaction::ParticleType dstP, int dstId, typename Interaction::ViewType srcView, float rc2,
        Interaction& interaction, Accumulator& accumulator)
{
    for (int srcId = pstart; srcId < pend; srcId++)
    {
        typename Interaction::ParticleType srcP;
        interaction.readCoordinates(srcP, srcView, srcId);

        bool interacting = interaction.withinCutoff(srcP, dstP);

        if (InteractWith == InteractionWith::Self)
            if (dstId <= srcId) interacting = false;

        if (interacting)
        {
            interaction.readExtraData(srcP, srcView, srcId);

            auto val = interaction(dstP, dstId, srcP, srcId);

            if (NeedDstAcc == InteractionOut::NeedAcc)
                accumulator.add(val);

            if (NeedSrcAcc == InteractionOut::NeedAcc)
                accumulator.atomicAddToSrc(val, srcView, srcId);
        }
    }
}

/**
 * Compute interactions within a single ParticleVector.
 *
 * Mapping is one thread per particle. The thread will traverse half
 * of the neighbouring cells and compute all the interactions between
 * original destination particle and all the particles in the cells.
 *
 * @param np number of particles
 * @param cinfo cell-list data
 * @param rc2 squared cut-off distance
 * @param interaction is a \c \_\_device\_\_ callable that computes
 *        the force between two particles. It has to have the following
 *        signature:
 *        \code float3 interaction(const Particle dst, int dstId, const Particle src, int srcId) \endcode
 *        The return value is the force acting on the first particle.
 *        The second one experiences the opposite force.
 */
template<typename Interaction>
__launch_bounds__(128, 16)
__global__ void computeSelfInteractions(
        CellListInfo cinfo, typename Interaction::ViewType view,
        const float rc2, Interaction interaction)
{
    const int dstId = blockIdx.x*blockDim.x + threadIdx.x;
    if (dstId >= view.size) return;

    const auto dstP = interaction.read(view, dstId);

    auto accumulator = interaction.getZeroedAccumulator();

    const int3 cell0 = cinfo.getCellIdAlongAxes(interaction.getPosition(dstP));

    for (int cellZ = cell0.z-1; cellZ <= cell0.z+1; cellZ++)
    {
        for (int cellY = cell0.y-1; cellY <= cell0.y; cellY++)
        {
            if ( !(cellY >= 0 && cellY < cinfo.ncells.y && cellZ >= 0 && cellZ < cinfo.ncells.z) ) continue;
            if (cellY == cell0.y && cellZ > cell0.z) continue;
            
            const int midCellId = cinfo.encode(cell0.x, cellY, cellZ);
            int rowStart  = max(midCellId-1, 0);
            int rowEnd    = min(midCellId+2, cinfo.totcells);
            
            if ( cellY == cell0.y && cellZ == cell0.z ) rowEnd = midCellId + 1; // this row is already partly covered
            
            const int pstart = cinfo.cellStarts[rowStart];
            const int pend   = cinfo.cellStarts[rowEnd];
            
            if (cellY == cell0.y && cellZ == cell0.z)
                computeCell<InteractionOut::NeedAcc, InteractionOut::NeedAcc, InteractionWith::Self>
                    (pstart, pend, dstP, dstId, view, rc2, interaction, accumulator);
            else
                computeCell<InteractionOut::NeedAcc, InteractionOut::NeedAcc, InteractionWith::Other>
                    (pstart, pend, dstP, dstId, view, rc2, interaction, accumulator);
        }
    }

    if (needSelfInteraction<Interaction>::value)
        accumulator.add(interaction(dstP, dstId, dstP, dstId));
    
    accumulator.atomicAddToDst(accumulator.get(), view, dstId);
}


/**
 * Compute interactions between particle of two different ParticleVector.
 *
 * Mapping is one thread per particle. The thread will traverse all
 * of the neighbouring cells and compute all the interactions between
 * original destination particle and all the particles of the second
 * kind residing in the cells.
 *
 * @param dstView view of the destination particles. They are accessed
 *        by the threads in a completely coalesced manner, no cell-list
 *        is needed for them.
 * @param srcCinfo cell-list data for the source particles
 * @param rc2 squared cut-off distance
 * @param interaction is a \c \_\_device\_\_ callable that computes
 *        the force between two particles. It has to have the following
 *        signature:
 *        \code float3 interaction(const Particle dst, int dstId, const Particle src, int srcId) \endcode
 *        The return value is the force acting on the first particle.
 *        The second one experiences the opposite force.
 *
 * @tparam NeedDstAcc if true, compute forces for destination particles.
 *         One out of \p NeedDstAcc or \p NeedSrcAcc should be true.
 * @tparam NeedSrcAcc if true, compute forces for source particles.
 *         One out of \p NeedDstAcc or \p NeedSrcAcc should be true.
 * @tparam Variant performance related parameter. \e true is better for
 * densely mixed stuff, \e false is better for halo
 */
template<InteractionOut NeedDstAcc, InteractionOut NeedSrcAcc, InteractionMode Variant, typename Interaction>
__launch_bounds__(128, 16)
__global__ void computeExternalInteractions_1tpp(
        typename Interaction::ViewType dstView, CellListInfo srcCinfo,
        typename Interaction::ViewType srcView,
        const float rc2, Interaction interaction)
{
    static_assert(NeedDstAcc == InteractionOut::NeedAcc || NeedSrcAcc == InteractionOut::NeedAcc,
                  "External interactions should return at least some accelerations");

    const int dstId = blockIdx.x*blockDim.x + threadIdx.x;
    if (dstId >= dstView.size) return;

    const auto dstP = interaction.readNoCache(dstView, dstId);

    auto accumulator = interaction.getZeroedAccumulator();

    const int3 cell0 = srcCinfo.getCellIdAlongAxes<CellListsProjection::NoClamp>(interaction.getPosition(dstP));

    for (int cellZ = cell0.z-1; cellZ <= cell0.z+1; cellZ++)
        for (int cellY = cell0.y-1; cellY <= cell0.y+1; cellY++)
            if (Variant == InteractionMode::RowWise)
            {
                if ( !(cellY >= 0 && cellY < srcCinfo.ncells.y && cellZ >= 0 && cellZ < srcCinfo.ncells.z) ) continue;

                const int midCellId = srcCinfo.encode(cell0.x, cellY, cellZ);
                int rowStart  = max(midCellId-1, 0);
                int rowEnd    = min(midCellId+2, srcCinfo.totcells);

                if (rowStart >= rowEnd) continue;
                
                const int pstart = srcCinfo.cellStarts[rowStart];
                const int pend   = srcCinfo.cellStarts[rowEnd];

                computeCell<NeedDstAcc, NeedSrcAcc, InteractionWith::Other>
                    (pstart, pend, dstP, dstId, srcView, rc2, interaction, accumulator);
            }
            else
            {
                if ( !(cellY >= 0 && cellY < srcCinfo.ncells.y && cellZ >= 0 && cellZ < srcCinfo.ncells.z) ) continue;

                for (int cellX = max(cell0.x-1, 0); cellX <= min(cell0.x+1, srcCinfo.ncells.x-1); cellX++)
                {
                    const int cid = srcCinfo.encode(cellX, cellY, cellZ);
                    const int pstart = srcCinfo.cellStarts[cid];
                    const int pend   = srcCinfo.cellStarts[cid+1];

                    computeCell<NeedDstAcc, NeedSrcAcc, InteractionWith::Other>
                        (pstart, pend, dstP, dstId, srcView, rc2, interaction, accumulator);
                }
            }

    if (NeedDstAcc == InteractionOut::NeedAcc)
        accumulator.atomicAddToDst(accumulator.get(), dstView, dstId);
}

/**
 * Compute interactions between particle of two different ParticleVector.
 *
 * Mapping is three threads per particle. The rest is similar to
 * computeExternalInteractions_1tpp()
 */
template<InteractionOut NeedDstAcc, InteractionOut NeedSrcAcc, InteractionMode Variant, typename Interaction>
__launch_bounds__(128, 16)
__global__ void computeExternalInteractions_3tpp(
        typename Interaction::ViewType dstView, CellListInfo srcCinfo,
        typename Interaction::ViewType srcView,
        const float rc2, Interaction interaction)
{
    static_assert(NeedDstAcc == InteractionOut::NeedAcc || NeedSrcAcc == InteractionOut::NeedAcc,
                  "External interactions should return at least some accelerations");

    const int gid = blockIdx.x*blockDim.x + threadIdx.x;

    const int dstId = gid / 3;
    const int dircode = gid % 3 - 1;

    if (dstId >= dstView.size) return;

    const auto dstP = interaction.readNoCache(dstView, dstId);

    auto accumulator = interaction.getZeroedAccumulator();

    const int3 cell0 = srcCinfo.getCellIdAlongAxes<CellListsProjection::NoClamp>(interaction.getPosition(dstP));

    int cellZ = cell0.z + dircode;

    for (int cellY = cell0.y-1; cellY <= cell0.y+1; cellY++)
        if (Variant == InteractionMode::RowWise)
        {
            if ( !(cellY >= 0 && cellY < srcCinfo.ncells.y && cellZ >= 0 && cellZ < srcCinfo.ncells.z) ) continue;

            const int midCellId = srcCinfo.encode(cell0.x, cellY, cellZ);
            int rowStart  = max(midCellId-1, 0);
            int rowEnd    = min(midCellId+2, srcCinfo.totcells);

            if (rowStart >= rowEnd) continue;
            
            const int pstart = srcCinfo.cellStarts[rowStart];
            const int pend   = srcCinfo.cellStarts[rowEnd];

            computeCell<NeedDstAcc, NeedSrcAcc, InteractionWith::Other>
                (pstart, pend, dstP, dstId, srcView, rc2, interaction, accumulator);
        }
        else
        {
            if ( !(cellY >= 0 && cellY < srcCinfo.ncells.y && cellZ >= 0 && cellZ < srcCinfo.ncells.z) ) continue;

            for (int cellX = max(cell0.x-1, 0); cellX <= min(cell0.x+1, srcCinfo.ncells.x-1); cellX++)
            {
                const int cid = srcCinfo.encode(cellX, cellY, cellZ);
                const int pstart = srcCinfo.cellStarts[cid];
                const int pend   = srcCinfo.cellStarts[cid+1];

                computeCell<NeedDstAcc, NeedSrcAcc, InteractionWith::Other>
                    (pstart, pend, dstP, dstId, srcView, rc2, interaction, accumulator);
            }
        }

    if (NeedDstAcc == InteractionOut::NeedAcc)
        accumulator.atomicAddToDst(accumulator.get(), dstView, dstId);
}

/**
 * Compute interactions between particle of two different ParticleVector.
 *
 * Mapping is nine threads per particle. The rest is similar to
 * computeExternalInteractions_1tpp()
 */
template<InteractionOut NeedDstAcc, InteractionOut NeedSrcAcc, InteractionMode Variant, typename Interaction>
__launch_bounds__(128, 16)
__global__ void computeExternalInteractions_9tpp(
        typename Interaction::ViewType dstView, CellListInfo srcCinfo,
        typename Interaction::ViewType srcView,
        const float rc2, Interaction interaction)
{
    static_assert(NeedDstAcc == InteractionOut::NeedAcc || NeedSrcAcc == InteractionOut::NeedAcc,
                  "External interactions should return at least some accelerations");

    const int gid = blockIdx.x*blockDim.x + threadIdx.x;

    const int dstId = gid / 9;
    const int dircode = gid % 9;

    if (dstId >= dstView.size) return;

    const auto dstP = interaction.readNoCache(dstView, dstId);

    auto accumulator = interaction.getZeroedAccumulator();

    const int3 cell0 = srcCinfo.getCellIdAlongAxes<CellListsProjection::NoClamp>(interaction.getPosition(dstP));

    int cellZ = cell0.z + dircode / 3 - 1;
    int cellY = cell0.y + dircode % 3 - 1;

    if (Variant == InteractionMode::RowWise)
    {
        if ( !(cellY >= 0 && cellY < srcCinfo.ncells.y && cellZ >= 0 && cellZ < srcCinfo.ncells.z) ) return;

        const int midCellId = srcCinfo.encode(cell0.x, cellY, cellZ);
        int rowStart  = max(midCellId-1, 0);
        int rowEnd    = min(midCellId+2, srcCinfo.totcells);

        if (rowStart >= rowEnd) return;
        
        const int pstart = srcCinfo.cellStarts[rowStart];
        const int pend   = srcCinfo.cellStarts[rowEnd];

        computeCell<NeedDstAcc, NeedSrcAcc, InteractionWith::Other>
            (pstart, pend, dstP, dstId, srcView, rc2, interaction, accumulator);
    }
    else
    {
        if ( !(cellY >= 0 && cellY < srcCinfo.ncells.y && cellZ >= 0 && cellZ < srcCinfo.ncells.z) ) return;

        for (int cellX = max(cell0.x-1, 0); cellX <= min(cell0.x+1, srcCinfo.ncells.x-1); cellX++)
        {
            const int cid = srcCinfo.encode(cellX, cellY, cellZ);
            const int pstart = srcCinfo.cellStarts[cid];
            const int pend   = srcCinfo.cellStarts[cid+1];

            computeCell<NeedDstAcc, NeedSrcAcc, InteractionWith::Other>
                (pstart, pend, dstP, dstId, srcView, rc2, interaction, accumulator);
        }
    }

    if (NeedDstAcc == InteractionOut::NeedAcc)
        accumulator.atomicAddToDst(accumulator.get(), dstView, dstId);
}

/**
 * Compute interactions between particle of two different ParticleVector.
 *
 * Mapping is 27 threads per particle. The rest is similar to
 * computeExternalInteractions_1tpp()
 */
template<InteractionOut NeedDstAcc, InteractionOut NeedSrcAcc, InteractionMode Variant, typename Interaction>
__launch_bounds__(128, 16)
__global__ void computeExternalInteractions_27tpp(
        typename Interaction::ViewType dstView, CellListInfo srcCinfo,
        typename Interaction::ViewType srcView,
        const float rc2, Interaction interaction)
{
    static_assert(NeedDstAcc == InteractionOut::NeedAcc || NeedSrcAcc == InteractionOut::NeedAcc,
                  "External interactions should return at least some accelerations");

    const int gid = blockIdx.x*blockDim.x + threadIdx.x;

    const int dstId = gid / 27;
    const int dircode = gid % 27;    
    
    if (dstId >= dstView.size) return;

    const auto dstP = interaction.readNoCache(dstView, dstId);

    auto accumulator = interaction.getZeroedAccumulator();

    const int3 cell0 = srcCinfo.getCellIdAlongAxes<CellListsProjection::NoClamp>(interaction.getPosition(dstP));

    int cellZ = cell0.z +  dircode / 9      - 1;
    int cellY = cell0.y + (dircode / 3) % 3 - 1;
    int cellX = cell0.x +  dircode % 3      - 1;

    if ( !( cellX >= 0 && cellX < srcCinfo.ncells.x &&
            cellY >= 0 && cellY < srcCinfo.ncells.y &&
            cellZ >= 0 && cellZ < srcCinfo.ncells.z) ) return;

    const int cid = srcCinfo.encode(cellX, cellY, cellZ);
    const int pstart = srcCinfo.cellStarts[cid];
    const int pend   = srcCinfo.cellStarts[cid+1];

    computeCell<NeedDstAcc, NeedSrcAcc, InteractionWith::Other>
        (pstart, pend, dstP, dstId, srcView, rc2, interaction, accumulator);

    if (NeedDstAcc == InteractionOut::NeedAcc)
        accumulator.atomicAddToDst(accumulator.get(), dstView, dstId);
}
