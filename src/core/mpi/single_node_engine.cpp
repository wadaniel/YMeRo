#include "single_node_engine.h"

#include <core/logger.h>
#include <algorithm>

SingleNodeEngine::SingleNodeEngine(std::unique_ptr<ParticleExchanger> exchanger) :
        exchanger(std::move(exchanger))
{   }

void SingleNodeEngine::init(cudaStream_t stream)
{
    auto& helpers = exchanger->helpers;
    
    for (int i = 0; i < helpers.size(); ++i)
        if (!exchanger->needExchange(i)) debug("Exchange of PV '%s' is skipped", helpers[i]->name.c_str());
    
    // Derived class determines what to send
    for (int i = 0; i < helpers.size(); ++i)
        if (exchanger->needExchange(i)) exchanger->prepareSizes(i, stream);
        
    CUDA_Check( cudaStreamSynchronize(stream) );

    // Derived class determines what to send
    for (int i = 0; i < helpers.size(); ++i)
        if (exchanger->needExchange(i)) exchanger->prepareData(i, stream);
}

void SingleNodeEngine::finalize(cudaStream_t stream)
{
    auto& helpers = exchanger->helpers;

    for (int i = 0; i < helpers.size(); ++i)
        if (exchanger->needExchange(i)) copySend2Recv(helpers[i].get(), stream);
        
    for (int i = 0; i < helpers.size(); ++i)
        if (exchanger->needExchange(i)) exchanger->combineAndUploadData(i, stream);
}


void SingleNodeEngine::copySend2Recv(ExchangeHelper *helper, cudaStream_t stream)
{
    auto bulkId = helper->bulkId;
    
    if (helper->sendSizes[bulkId] != 0)
        error("Non-empty message to itself detected, this may fail with the Single node engine, "
            "working with particle vector '%s'", helper->name.c_str());
        
    helper->recvSizes   .copy(helper->sendSizes,   stream); // copy as we may need sizes from other classes
    helper->recvOffsets .copy(helper->sendOffsets, stream);
    std::swap(helper->recvBuf,     helper->sendBuf);
}

