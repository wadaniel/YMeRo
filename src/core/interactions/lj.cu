#include "lj.h"
#include "pairwise.impl.h"
#include "pairwise_interactions/lj.h"
#include "pairwise_interactions/lj_object_aware.h"

#include <core/celllist.h>

#include <memory>

InteractionLJ::InteractionLJ(const YmrState *state, std::string name, float rc, float epsilon, float sigma, float maxForce, bool objectAware, bool allocate) :
    Interaction(state, name, rc),
    objectAware(objectAware)
{
    if (!allocate) return;

    if (objectAware) {
        PairwiseLJObjectAware lj(rc, epsilon, sigma, maxForce);
        impl = std::make_unique<InteractionPair<PairwiseLJObjectAware>> (state, name, rc, lj);
    }
    else {
        PairwiseLJ lj(rc, epsilon, sigma, maxForce);
        impl = std::make_unique<InteractionPair<PairwiseLJ>> (state, name, rc, lj);
    }
}

InteractionLJ::InteractionLJ(const YmrState *state, std::string name, float rc, float epsilon, float sigma, float maxForce, bool objectAware) :
    InteractionLJ(state, name, rc, epsilon, sigma, maxForce, objectAware, true)
{}

InteractionLJ::~InteractionLJ() = default;

void InteractionLJ::setPrerequisites(ParticleVector *pv1, ParticleVector *pv2, CellList *cl1, CellList *cl2)
{
    impl->setPrerequisites(pv1, pv2, cl1, cl2);
}

std::vector<Interaction::InteractionChannel> InteractionLJ::getFinalOutputChannels() const
{
    return impl->getFinalOutputChannels();
}

void InteractionLJ::local(ParticleVector *pv1, ParticleVector *pv2,
                          CellList *cl1, CellList *cl2,
                          cudaStream_t stream)
{
    impl->local(pv1, pv2, cl1, cl2, stream);
}

void InteractionLJ::halo(ParticleVector *pv1, ParticleVector *pv2,
                         CellList *cl1, CellList *cl2,
                         cudaStream_t stream)
{
    impl->halo(pv1, pv2, cl1, cl2, stream);
}

void InteractionLJ::setSpecificPair(ParticleVector* pv1, ParticleVector* pv2, 
                                    float epsilon, float sigma, float maxForce)
{
    if (objectAware) {
        PairwiseLJObjectAware lj(rc, epsilon, sigma, maxForce);
        auto ptr = static_cast< InteractionPair<PairwiseLJObjectAware>* >(impl.get());
        ptr->setSpecificPair(pv1->name, pv2->name, lj);
    }
    else {
        PairwiseLJ lj(rc, epsilon, sigma, maxForce);
        auto ptr = static_cast< InteractionPair<PairwiseLJ>* >(impl.get());
        ptr->setSpecificPair(pv1->name, pv2->name, lj);
    }
}

