#include "dpd.h"
#include "pairwise.impl.h"
#include "pairwise_interactions/dpd.h"

#include <core/celllist.h>
#include <core/utils/make_unique.h>
#include <core/pvs/particle_vector.h>

#include <memory>

InteractionDPD::InteractionDPD(const YmrState *state, std::string name, float rc, float a, float gamma, float kbt, float power, bool allocateImpl) :
    Interaction(state, name, rc),
    a(a), gamma(gamma), kbt(kbt), power(power)
{
    if (allocateImpl) {
        PairwiseDPD dpd(rc, a, gamma, kbt, state->dt, power);
        impl = std::make_unique<InteractionPair<PairwiseDPD>> (state, name, rc, dpd);
    }
}

InteractionDPD::InteractionDPD(const YmrState *state, std::string name, float rc, float a, float gamma, float kbt, float power) :
    InteractionDPD(state, name, rc, a, gamma, kbt, power, true)
{}

InteractionDPD::~InteractionDPD() = default;

void InteractionDPD::setPrerequisites(ParticleVector *pv1, ParticleVector *pv2, CellList *cl1, CellList *cl2)
{
    impl->setPrerequisites(pv1, pv2, cl1, cl2);
}

std::vector<Interaction::InteractionChannel> InteractionDPD::getFinalOutputChannels() const
{
    return impl->getFinalOutputChannels();
}

void InteractionDPD::local(ParticleVector *pv1, ParticleVector *pv2,
                           CellList *cl1, CellList *cl2,
                           cudaStream_t stream)
{
    impl->local(pv1, pv2, cl1, cl2, stream);
}

void InteractionDPD::halo(ParticleVector *pv1, ParticleVector *pv2,
                          CellList *cl1, CellList *cl2,
                          cudaStream_t stream)
{
    impl->halo(pv1, pv2, cl1, cl2, stream);
}

void InteractionDPD::setSpecificPair(ParticleVector *pv1, ParticleVector *pv2, 
                                     float a, float gamma, float kbt, float power)
{
    if (a     == Default) a     = this->a;
    if (gamma == Default) gamma = this->gamma;
    if (kbt   == Default) kbt   = this->kbt;
    if (power == Default) power = this->power;

    PairwiseDPD dpd(this->rc, a, gamma, kbt, state->dt, power);
    auto ptr = static_cast< InteractionPair<PairwiseDPD>* >(impl.get());
    
    ptr->setSpecificPair(pv1->name, pv2->name, dpd);
}


