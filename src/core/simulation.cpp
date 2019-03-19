#include "simulation.h"

#include <core/bouncers/interface.h>
#include <core/celllist.h>
#include <core/initial_conditions/interface.h>
#include <core/integrators/interface.h>
#include <core/interactions/interface.h>
#include <core/managers/interactions.h>
#include <core/mpi/api.h>
#include <core/object_belonging/interface.h>
#include <core/pvs/object_vector.h>
#include <core/pvs/particle_vector.h>
#include <core/task_scheduler.h>
#include <core/utils/folders.h>
#include <core/utils/make_unique.h>
#include <core/utils/restart_helpers.h>
#include <core/walls/interface.h>
#include <core/ymero_state.h>
#include <plugins/interface.h>

#include <algorithm>
#include <cuda_profiler_api.h>

#define TASK_LIST(_)                                                    \
    _( checkpoint                          , "Checkpoint")              \
    _( cellLists                           , "Build cell-lists")        \
    _( integration                         , "Integration")             \
    _( partClearIntermediate               , "Particle clear intermediate") \
    _( partHaloIntermediateInit            , "Particle halo intermediate init") \
    _( partHaloIntermediateFinalize        , "Particle halo intermediate finalize") \
    _( localIntermediate                   , "Local intermediate")      \
    _( haloIntermediate                    , "Halo intermediate")       \
    _( accumulateInteractionIntermediate   , "Accumulate intermediate") \
    _( gatherInteractionIntermediate       , "Gather intermediate")     \
    _( partClearFinal                      , "Clear forces")            \
    _( partHaloFinalInit                   , "Particle halo final init") \
    _( partHaloFinalFinalize               , "Particle halo final finalize") \
    _( localForces                         , "Local forces")            \
    _( haloForces                          , "Halo forces")             \
    _( accumulateInteractionFinal          , "Accumulate forces")       \
    _( objHaloFinalInit                    , "Object halo final init")  \
    _( objHaloFinalFinalize                , "Object halo final finalize") \
    _( objHaloIntermediateInit             , "Object halo intermediate init")  \
    _( objHaloIntermediateFinalize         , "Object halo intermediate finalize") \
    _( objReverseIntermediateInit          , "Object reverse intermediate: init") \
    _( objReverseIntermediateFinalize      , "Object reverse intermediate: finalize") \
    _( objReverseFinalInit                 , "Object reverse final: init") \
    _( objReverseFinalFinalize             , "Object reverse final: finalize") \
    _( objClearLocalIntermediate           , "Clear local object intermediate") \
    _( objClearHaloIntermediate            , "Clear halo object intermediate") \
    _( objClearHaloForces                  , "Clear object halo forces") \
    _( objClearLocalForces                 , "Clear object local forces") \
    _( objLocalBounce                      , "Local object bounce")     \
    _( objHaloBounce                       , "Halo object bounce")      \
    _( correctObjBelonging                 , "Correct object belonging") \
    _( wallBounce                          , "Wall bounce")             \
    _( wallCheck                           , "Wall check")              \
    _( partRedistributeInit                , "Particle redistribute init") \
    _( partRedistributeFinalize            , "Particle redistribute finalize") \
    _( objRedistInit                       , "Object redistribute init") \
    _( objRedistFinalize                   , "Object redistribute finalize") \
    _( pluginsBeforeCellLists              , "Plugins: before cell lists") \
    _( pluginsBeforeForces                 , "Plugins: before forces")  \
    _( pluginsSerializeSend                , "Plugins: serialize and send") \
    _( pluginsBeforeIntegration            , "Plugins: before integration") \
    _( pluginsAfterIntegration             , "Plugins: after integration") \
    _( pluginsBeforeParticlesDistribution  , "Plugins: before particles distribution")


struct SimulationTasks
{
#define DECLARE(NAME, DESC) TaskScheduler::TaskID NAME ;

    TASK_LIST(DECLARE);

#undef DECLARE    
};

Simulation::Simulation(const MPI_Comm &cartComm, const MPI_Comm &interComm, YmrState *state,
                       int globalCheckpointEvery, std::string checkpointFolder,
                       bool gpuAwareMPI)
    : nranks3D(nranks3D),
      interComm(interComm),
      state(state),
      globalCheckpointEvery(globalCheckpointEvery),
      checkpointFolder(checkpointFolder),
      gpuAwareMPI(gpuAwareMPI),
      scheduler(std::make_unique<TaskScheduler>()),
      tasks(std::make_unique<SimulationTasks>()),
      interactionManager(std::make_unique<InteractionManager>())
{
    int nranks[3], periods[3], coords[3];

    MPI_Check(MPI_Comm_dup(cartComm, &this->cartComm));
    MPI_Check(MPI_Cart_get(cartComm, 3, nranks, periods, coords));
    MPI_Check(MPI_Comm_rank(cartComm, &rank));

    nranks3D = {nranks[0], nranks[1], nranks[2]};
    rank3D   = {coords[0], coords[1], coords[2]};

    createFoldersCollective(cartComm, checkpointFolder);

    state->reinitTime();
    
    info("Simulation initialized, subdomain size is [%f %f %f], subdomain starts "
         "at [%f %f %f]",
         state->domain.localSize.x, state->domain.localSize.y, state->domain.localSize.z,
         state->domain.globalStart.x, state->domain.globalStart.y, state->domain.globalStart.z);    
}

Simulation::~Simulation()
{
    MPI_Check( MPI_Comm_free(&cartComm) );
}


//================================================================================================
// Access for plugins
//================================================================================================


std::vector<ParticleVector*> Simulation::getParticleVectors() const
{
    std::vector<ParticleVector*> res;
    for (auto& pv : particleVectors)
        res.push_back(pv.get());

    return res;
}

ParticleVector* Simulation::getPVbyName(std::string name) const
{
    auto pvIt = pvIdMap.find(name);
    return (pvIt != pvIdMap.end()) ? particleVectors[pvIt->second].get() : nullptr;
}

std::shared_ptr<ParticleVector> Simulation::getSharedPVbyName(std::string name) const
{
    auto pvIt = pvIdMap.find(name);
    return (pvIt != pvIdMap.end()) ? particleVectors[pvIt->second] : std::shared_ptr<ParticleVector>(nullptr);
}

ParticleVector* Simulation::getPVbyNameOrDie(std::string name) const
{
    auto pv = getPVbyName(name);
    if (pv == nullptr)
        die("No such particle vector: %s", name.c_str());
    return pv;
}

ObjectVector* Simulation::getOVbyNameOrDie(std::string name) const
{
    auto pv = getPVbyName(name);
    auto ov = dynamic_cast<ObjectVector*>(pv);
    if (pv == nullptr)
        die("No such particle vector: %s", name.c_str());
    return ov;
}

Wall* Simulation::getWallByNameOrDie(std::string name) const
{
    if (wallMap.find(name) == wallMap.end())
        die("No such wall: %s", name.c_str());

    auto it = wallMap.find(name);
    return it->second.get();
}

CellList* Simulation::gelCellList(ParticleVector* pv) const
{
    auto clvecIt = cellListMap.find(pv);
    if (clvecIt == cellListMap.end())
        die("Particle Vector '%s' is not registered or broken", pv->name.c_str());

    if (clvecIt->second.size() == 0)
        return nullptr;
    else
        return clvecIt->second[0].get();
}

MPI_Comm Simulation::getCartComm() const
{
    return cartComm;
}

float Simulation::getCurrentDt() const
{
    return state->dt;
}

float Simulation::getCurrentTime() const
{
    return state->currentTime;
}

float Simulation::getMaxEffectiveCutoff() const
{
    return interactionManager->getMaxEffectiveCutoff();
}

void Simulation::startProfiler() const
{
    CUDA_Check( cudaProfilerStart() );
}

void Simulation::stopProfiler() const
{
    CUDA_Check( cudaProfilerStop() );
}

//================================================================================================
// Registration
//================================================================================================

void Simulation::registerParticleVector(std::shared_ptr<ParticleVector> pv, std::shared_ptr<InitialConditions> ic, int checkpointEvery)
{
    std::string name = pv->name;

    if (name == "none" || name == "all" || name == "")
        die("Invalid name for a particle vector (reserved word or empty): '%s'", name.c_str());
    
    if (pv->name.rfind("_", 0) == 0)
        die("Identifier of Particle Vectors cannot start with _");

    if (pvIdMap.find(name) != pvIdMap.end())
        die("More than one particle vector is called %s", name.c_str());

    if (restartStatus != RestartStatus::Anew)
        pv->restart(cartComm, restartFolder);
    else
    {
        if (ic != nullptr)
            ic->exec(cartComm, pv.get(), 0);
    }

    pvsCheckPointPrototype.push_back({pv.get(), checkpointEvery});

    auto ov = dynamic_cast<ObjectVector*>(pv.get());
    if(ov != nullptr)
    {
        info("Registered object vector '%s', %d objects, %d particles", name.c_str(), ov->local()->nObjects, ov->local()->size());
        objectVectors.push_back(ov);
    }
    else
        info("Registered particle vector '%s', %d particles", name.c_str(), pv->local()->size());

    particleVectors.push_back(std::move(pv));
    pvIdMap[name] = particleVectors.size() - 1;
}

void Simulation::registerWall(std::shared_ptr<Wall> wall, int every)
{
    std::string name = wall->name;

    if (wallMap.find(name) != wallMap.end())
        die("More than one wall is called %s", name.c_str());

    checkWallPrototypes.push_back({wall.get(), every});

    // Let the wall know the particle vector associated with it
    wall->setup(cartComm);
    if (restartStatus != RestartStatus::Anew)
        wall->restart(cartComm, restartFolder);

    info("Registered wall '%s'", name.c_str());

    wallMap[name] = std::move(wall);
}

void Simulation::registerInteraction(std::shared_ptr<Interaction> interaction)
{
    std::string name = interaction->name;
    if (interactionMap.find(name) != interactionMap.end())
        die("More than one interaction is called %s", name.c_str());

    if (restartStatus != RestartStatus::Anew)
        interaction->restart(cartComm, restartFolder);

    interactionMap[name] = std::move(interaction);
}

void Simulation::registerIntegrator(std::shared_ptr<Integrator> integrator)
{
    std::string name = integrator->name;
    if (integratorMap.find(name) != integratorMap.end())
        die("More than one integrator is called %s", name.c_str());

    if (restartStatus != RestartStatus::Anew)
        integrator->restart(cartComm, restartFolder);
    
    integratorMap[name] = std::move(integrator);
}

void Simulation::registerBouncer(std::shared_ptr<Bouncer> bouncer)
{
    std::string name = bouncer->name;
    if (bouncerMap.find(name) != bouncerMap.end())
        die("More than one bouncer is called %s", name.c_str());

    if (restartStatus != RestartStatus::Anew)
        bouncer->restart(cartComm, restartFolder);
    
    bouncerMap[name] = std::move(bouncer);
}

void Simulation::registerObjectBelongingChecker(std::shared_ptr<ObjectBelongingChecker> checker)
{
    std::string name = checker->name;
    if (belongingCheckerMap.find(name) != belongingCheckerMap.end())
        die("More than one splitter is called %s", name.c_str());

    if (restartStatus != RestartStatus::Anew)
        checker->restart(cartComm, restartFolder);
    
    belongingCheckerMap[name] = std::move(checker);
}

void Simulation::registerPlugin(std::shared_ptr<SimulationPlugin> plugin)
{
    std::string name = plugin->name;

    bool found = false;
    for (auto& pl : plugins)
        if (pl->name == name) found = true;

    if (found)
        die("More than one plugin is called %s", name.c_str());

    if (restartStatus != RestartStatus::Anew)
        plugin->restart(cartComm, restartFolder);
    
    plugins.push_back(std::move(plugin));
}

//================================================================================================
// Applying something to something else
//================================================================================================

void Simulation::setIntegrator(std::string integratorName, std::string pvName)
{
    if (integratorMap.find(integratorName) == integratorMap.end())
        die("No such integrator: %s", integratorName.c_str());
    auto integrator = integratorMap[integratorName].get();

    auto pv = getPVbyNameOrDie(pvName);

    if (pvsIntegratorMap.find(pvName) != pvsIntegratorMap.end())
        die("particle vector '%s' already set to integrator '%s'",
            pvName.c_str(), pvsIntegratorMap[pvName].c_str());

    pvsIntegratorMap[pvName] = integratorName;
    
    integrator->setPrerequisites(pv);

    integratorsStage1.push_back([integrator, pv] (cudaStream_t stream) {
        integrator->stage1(pv, stream);
    });

    integratorsStage2.push_back([integrator, pv] (cudaStream_t stream) {
        integrator->stage2(pv, stream);
    });
}

void Simulation::setInteraction(std::string interactionName, std::string pv1Name, std::string pv2Name)
{
    auto pv1 = getPVbyNameOrDie(pv1Name);
    auto pv2 = getPVbyNameOrDie(pv2Name);

    if (interactionMap.find(interactionName) == interactionMap.end())
        die("No such interaction: %s", interactionName.c_str());
    auto interaction = interactionMap[interactionName].get();    

    float rc = interaction->rc;
    interactionPrototypes.push_back({rc, pv1, pv2, interaction});
}

void Simulation::setBouncer(std::string bouncerName, std::string objName, std::string pvName)
{
    auto pv = getPVbyNameOrDie(pvName);

    auto ov = dynamic_cast<ObjectVector*> (getPVbyName(objName));
    if (ov == nullptr)
        die("No such object vector: %s", objName.c_str());

    if (bouncerMap.find(bouncerName) == bouncerMap.end())
        die("No such bouncer: %s", bouncerName.c_str());
    auto bouncer = bouncerMap[bouncerName].get();

    bouncer->setup(ov);
    bouncer->setPrerequisites(pv);
    bouncerPrototypes.push_back({bouncer, pv});
}

void Simulation::setWallBounce(std::string wallName, std::string pvName)
{
    auto pv = getPVbyNameOrDie(pvName);

    if (wallMap.find(wallName) == wallMap.end())
        die("No such wall: %s", wallName.c_str());
    auto wall = wallMap[wallName].get();

    wall->setPrerequisites(pv);
    wallPrototypes.push_back( {wall, pv} );
}

void Simulation::setObjectBelongingChecker(std::string checkerName, std::string objName)
{
    auto ov = dynamic_cast<ObjectVector*>(getPVbyNameOrDie(objName));
    if (ov == nullptr)
        die("No such object vector %s", objName.c_str());

    if (belongingCheckerMap.find(checkerName) == belongingCheckerMap.end())
        die("No such belonging checker: %s", checkerName.c_str());
    auto checker = belongingCheckerMap[checkerName].get();

    // TODO: do this normal'no blyat!
    checker->setup(ov);
}

//
//
//

void Simulation::applyObjectBelongingChecker(std::string checkerName,
            std::string source, std::string inside, std::string outside,
            int checkEvery, int checkpointEvery)
{
    auto pvSource = getPVbyNameOrDie(source);

    if (inside == outside)
        die("Splitting into same pvs: %s into %s %s",
                source.c_str(), inside.c_str(), outside.c_str());

    if (source != inside && source != outside)
        die("At least one of the split destinations should be the same as source: %s into %s %s",
                source.c_str(), inside.c_str(), outside.c_str());

    if (belongingCheckerMap.find(checkerName) == belongingCheckerMap.end())
        die("No such belonging checker: %s", checkerName.c_str());

    if (getPVbyName(inside) != nullptr && inside != source)
        die("Cannot split into existing particle vector: %s into %s %s",
                source.c_str(), inside.c_str(), outside.c_str());

    if (getPVbyName(outside) != nullptr && outside != source)
        die("Cannot split into existing particle vector: %s into %s %s",
                source.c_str(), inside.c_str(), outside.c_str());


    auto checker = belongingCheckerMap[checkerName].get();

    std::shared_ptr<ParticleVector> pvInside, pvOutside;

    if (inside != "none" && getPVbyName(inside) == nullptr)
    {
        pvInside = std::make_shared<ParticleVector> (state, inside, pvSource->mass);
        registerParticleVector(pvInside, nullptr, checkpointEvery);
    }

    if (outside != "none" && getPVbyName(outside) == nullptr)
    {
        pvOutside = std::make_shared<ParticleVector> (state, outside, pvSource->mass);
        registerParticleVector(pvOutside, nullptr, checkpointEvery);
    }

    splitterPrototypes.push_back({checker, pvSource, getPVbyName(inside), getPVbyName(outside)});

    belongingCorrectionPrototypes.push_back({checker, getPVbyName(inside), getPVbyName(outside), checkEvery});
}

static void sortDescendingOrder(std::vector<float>& v)
{
    std::sort(v.begin(), v.end(), [] (float a, float b) { return a > b; });
}

// assume sorted array (ascending or descending)
static void removeDuplicatedElements(std::vector<float>& v, float tolerance)
{
    auto it = std::unique(v.begin(), v.end(), [=] (float a, float b) { return fabs(a - b) < tolerance; });
    v.resize( std::distance(v.begin(), it) );    
}

void Simulation::prepareCellLists()
{
    info("Preparing cell-lists");

    std::map<ParticleVector*, std::vector<float>> cutOffMap;

    // Deal with the cell-lists and interactions
    for (auto prototype : interactionPrototypes)
    {
        float rc = prototype.rc;
        cutOffMap[prototype.pv1].push_back(rc);
        cutOffMap[prototype.pv2].push_back(rc);
    }

    for (auto& cutoffPair : cutOffMap)
    {
        auto& pv      = cutoffPair.first;
        auto& cutoffs = cutoffPair.second;

        sortDescendingOrder(cutoffs);
        removeDuplicatedElements(cutoffs, rcTolerance);

        bool primary = true;

        // Don't use primary cell-lists with ObjectVectors
        if (dynamic_cast<ObjectVector*>(pv) != nullptr)
            primary = false;

        for (auto rc : cutoffs)
        {
            cellListMap[pv].push_back(primary ?
                    std::make_unique<PrimaryCellList>(pv, rc, state->domain.localSize) :
                    std::make_unique<CellList>       (pv, rc, state->domain.localSize));
            primary = false;
        }
    }

    for (auto& pv : particleVectors)
    {
        auto pvptr = pv.get();
        if (cellListMap[pvptr].empty())
        {
            const float defaultRc = 1.f;
            bool primary = true;

            // Don't use primary cell-lists with ObjectVectors
            if (dynamic_cast<ObjectVector*>(pvptr) != nullptr)
                primary = false;

            cellListMap[pvptr].push_back
                (primary ?
                 std::make_unique<PrimaryCellList>(pvptr, defaultRc, state->domain.localSize) :
                 std::make_unique<CellList>       (pvptr, defaultRc, state->domain.localSize));
        }
    }
}

// Choose a CL with smallest but bigger than rc cell
static CellList* selectBestClist(std::vector<std::unique_ptr<CellList>>& cellLists, float rc, float tolerance)
{
    float minDiff = 1e6;
    CellList* best = nullptr;
    
    for (auto& cl : cellLists) {
        float diff = cl->rc - rc;
        if (diff > -tolerance && diff < minDiff) {
            best    = cl.get();
            minDiff = diff;
        }
    }
    return best;
}

void Simulation::prepareInteractions()
{
    info("Preparing interactions");

    for (auto& prototype : interactionPrototypes)
    {
        auto  rc = prototype.rc;
        auto pv1 = prototype.pv1;
        auto pv2 = prototype.pv2;

        auto& clVec1 = cellListMap[pv1];
        auto& clVec2 = cellListMap[pv2];

        CellList *cl1, *cl2;

        cl1 = selectBestClist(clVec1, rc, rcTolerance);
        cl2 = selectBestClist(clVec2, rc, rcTolerance);
        
        auto inter = prototype.interaction;

        inter->setPrerequisites(pv1, pv2, cl1, cl2);

        interactionManager->add(inter, pv1, pv2, cl1, cl2);
    }
}

void Simulation::prepareBouncers()
{
    info("Preparing object bouncers");

    for (auto& prototype : bouncerPrototypes)
    {
        auto bouncer = prototype.bouncer;
        auto pv      = prototype.pv;

        if (pvsIntegratorMap.find(pv->name) == pvsIntegratorMap.end())
            die("Setting bouncer '%s': particle vector '%s' has no integrator, required for bounce back",
                bouncer->name.c_str(), pv->name.c_str());
        
        auto& clVec = cellListMap[pv];

        if (clVec.empty()) continue;

        CellList *cl = clVec[0].get();

        regularBouncers.push_back([bouncer, pv, cl] (cudaStream_t stream) {
            bouncer->bounceLocal(pv, cl, stream);
        });

        haloBouncers.   push_back([bouncer, pv, cl] (cudaStream_t stream) {
            bouncer->bounceHalo (pv, cl, stream);
        });
    }
}

void Simulation::prepareWalls()
{
    info("Preparing walls");

    for (auto& prototype : wallPrototypes)
    {
        auto wall = prototype.wall;
        auto pv   = prototype.pv;

        auto& clVec = cellListMap[pv];

        if (clVec.empty()) continue;

        CellList *cl = clVec[0].get();

        wall->attach(pv, cl);
    }

    for (auto& wall : wallMap)
    {
        auto wallPtr = wall.second.get();

        // All the particles should be removed from within the wall,
        // even those that do not interact with it
        // Only frozen wall particles will remain
        for (auto& anypv : particleVectors)
            wallPtr->removeInner(anypv.get());
    }
}

void Simulation::preparePlugins()
{
    info("Preparing plugins");
    for (auto& pl : plugins) {
        debug("Setup and handshake of plugin %s", pl->name.c_str());
        pl->setup(this, cartComm, interComm);
        pl->handshake();
    }
    info("done Preparing plugins");
}


std::vector<std::string> Simulation::getExtraDataToExchange(ObjectVector *ov)
{
    std::set<std::string> channels;
    
    for (auto& entry : bouncerMap)
    {
        auto& bouncer = entry.second;
        if (bouncer->getObjectVector() != ov) continue;

        auto extraChannels = bouncer->getChannelsToBeExchanged();
        for (auto channel : extraChannels)
            channels.insert(channel);
    }

    for (auto& entry : belongingCheckerMap)
    {
        auto& belongingChecker = entry.second;
        if (belongingChecker->getObjectVector() != ov) continue;

        auto extraChannels = belongingChecker->getChannelsToBeExchanged();
        for (auto channel : extraChannels)
            channels.insert(channel);
    }

    return {channels.begin(), channels.end()};
}

void Simulation::prepareEngines()
{
    auto partRedistImp                  = std::make_unique<ParticleRedistributor>();
    auto partHaloFinalImp               = std::make_unique<ParticleHaloExchanger>();
    auto partHaloIntermediateImp        = std::make_unique<ParticleHaloExchanger>();
    auto objRedistImp                   = std::make_unique<ObjectRedistributor>();        
    auto objHaloFinalImp                = std::make_unique<ObjectHaloExchanger>();
    auto objHaloIntermediateImp         = std::make_unique<ObjectExtraExchanger>  (objHaloFinalImp.get());
    auto objHaloReverseIntermediateImp  = std::make_unique<ObjectReverseExchanger>(objHaloFinalImp.get());
    auto objHaloReverseFinalImp         = std::make_unique<ObjectReverseExchanger>(objHaloFinalImp.get());

    debug("Attaching particle vectors to halo exchanger and redistributor");
    for (auto& pv : particleVectors)
    {
        auto  pvPtr       = pv.get();
        auto& cellListVec = cellListMap[pvPtr];        

        if (cellListVec.size() == 0) continue;

        CellList *clInt = interactionManager->getLargestCellListNeededForIntermediate(pvPtr);
        CellList *clOut = interactionManager->getLargestCellListNeededForFinal(pvPtr);

        auto extraInt = interactionManager->getExtraIntermediateChannels(pvPtr);
        auto extraOut = interactionManager->getExtraFinalChannels(pvPtr);

        auto cl = cellListVec[0].get();
        auto ov = dynamic_cast<ObjectVector*>(pvPtr);
        
        if (ov == nullptr) {
            partRedistImp->attach(pvPtr, cl);
            
            if (clInt != nullptr)
                partHaloIntermediateImp->attach(pvPtr, clInt, {});

            if (clOut != nullptr)
                partHaloFinalImp->attach(pvPtr, clOut, extraInt);            
        }
        else {
            objRedistImp->attach(ov);

            auto extraToExchange = getExtraDataToExchange(ov);
            
            objHaloFinalImp->attach(ov, cl->rc, extraToExchange); // always active because of bounce back; TODO: check if bounce back is active
            objHaloReverseFinalImp->attach(ov, extraOut);

            objHaloIntermediateImp->attach(ov, extraInt);
            objHaloReverseIntermediateImp->attach(ov, extraInt);
        }
    }
    
    std::function< std::unique_ptr<ExchangeEngine>(std::unique_ptr<ParticleExchanger>) > makeEngine;
    
    // If we're on one node, use a singleNode engine
    // otherwise use MPI
    if (nranks3D.x * nranks3D.y * nranks3D.z == 1)
        makeEngine = [this] (std::unique_ptr<ParticleExchanger> exch) {
            return std::make_unique<SingleNodeEngine> (std::move(exch));
        };
    else
        makeEngine = [this] (std::unique_ptr<ParticleExchanger> exch) {
            return std::make_unique<MPIExchangeEngine> (std::move(exch), cartComm, gpuAwareMPI);
        };
    
    partRedistributor            = makeEngine(std::move(partRedistImp));
    partHaloFinal                = makeEngine(std::move(partHaloFinalImp));
    partHaloIntermediate         = makeEngine(std::move(partHaloIntermediateImp));
    objRedistibutor              = makeEngine(std::move(objRedistImp));
    objHaloFinal                 = makeEngine(std::move(objHaloFinalImp));
    objHaloIntermediate          = makeEngine(std::move(objHaloIntermediateImp));
    objHaloReverseIntermediate   = makeEngine(std::move(objHaloReverseIntermediateImp));
    objHaloReverseFinal          = makeEngine(std::move(objHaloReverseFinalImp));
}

void Simulation::execSplitters()
{
    info("Splitting particle vectors with respect to object belonging");

    for (auto& prototype : splitterPrototypes)
    {
        auto checker = prototype.checker;
        auto src     = prototype.pvSrc;
        auto inside  = prototype.pvIn;
        auto outside = prototype.pvOut;

        checker->splitByBelonging(src, inside, outside, 0);
    }
}

void Simulation::createTasks()
{
#define INIT(NAME, DESC) tasks -> NAME = scheduler->createTask(DESC);
    TASK_LIST(INIT);
#undef INIT

    if (globalCheckpointEvery > 0)
        scheduler->addTask(tasks->checkpoint,
                           [this](cudaStream_t stream) { this->checkpoint(); },
                           globalCheckpointEvery);

    for (auto prototype : pvsCheckPointPrototype)
        if (prototype.checkpointEvery > 0 && globalCheckpointEvery == 0) {
            info("Will save checkpoint of particle vector '%s' every %d timesteps",
                 prototype.pv->name.c_str(), prototype.checkpointEvery);

            scheduler->addTask( tasks->checkpoint, [prototype, this] (cudaStream_t stream) {
                prototype.pv->checkpoint(cartComm, checkpointFolder);
            }, prototype.checkpointEvery );
        }


    for (auto& clVec : cellListMap)
        for (auto& cl : clVec.second)
        {
            auto clPtr = cl.get();
            scheduler->addTask(tasks->cellLists, [clPtr] (cudaStream_t stream) { clPtr->build(stream); } );
        }

    // Only particle forces, not object ones here
    for (auto& pv : particleVectors)
    {
        auto pvPtr = pv.get();
        scheduler->addTask(tasks->partClearIntermediate,
                           [this, pvPtr] (cudaStream_t stream) { interactionManager->clearIntermediates(pvPtr, stream); } );
        scheduler->addTask(tasks->partClearFinal,
                           [this, pvPtr] (cudaStream_t stream) { interactionManager->clearFinal(pvPtr, stream); } );
    }

    for (auto& pl : plugins)
    {
        auto plPtr = pl.get();

        scheduler->addTask(tasks->pluginsBeforeCellLists, [plPtr, this] (cudaStream_t stream) {
            plPtr->beforeCellLists(stream);
        });

        scheduler->addTask(tasks->pluginsBeforeForces, [plPtr, this] (cudaStream_t stream) {
            plPtr->beforeForces(stream);
        });

        scheduler->addTask(tasks->pluginsSerializeSend, [plPtr] (cudaStream_t stream) {
            plPtr->serializeAndSend(stream);
        });

        scheduler->addTask(tasks->pluginsBeforeIntegration, [plPtr] (cudaStream_t stream) {
            plPtr->beforeIntegration(stream);
        });

        scheduler->addTask(tasks->pluginsAfterIntegration, [plPtr] (cudaStream_t stream) {
            plPtr->afterIntegration(stream);
        });

        scheduler->addTask(tasks->pluginsBeforeParticlesDistribution, [plPtr] (cudaStream_t stream) {
            plPtr->beforeParticleDistribution(stream);
        });
    }


    // If we have any non-object vectors
    if (particleVectors.size() != objectVectors.size())
    {
        scheduler->addTask(tasks->partHaloIntermediateInit, [this] (cudaStream_t stream) {
            partHaloIntermediate->init(stream);
        });

        scheduler->addTask(tasks->partHaloIntermediateFinalize, [this] (cudaStream_t stream) {
            partHaloIntermediate->finalize(stream);
        });

        scheduler->addTask(tasks->partHaloFinalInit, [this] (cudaStream_t stream) {
            partHaloFinal->init(stream);
        });

        scheduler->addTask(tasks->partHaloFinalFinalize, [this] (cudaStream_t stream) {
            partHaloFinal->finalize(stream);
        });

        scheduler->addTask(tasks->partRedistributeInit, [this] (cudaStream_t stream) {
            partRedistributor->init(stream);
        });

        scheduler->addTask(tasks->partRedistributeFinalize, [this] (cudaStream_t stream) {
            partRedistributor->finalize(stream);
        });
    }


    scheduler->addTask(tasks->localIntermediate,
                       [this] (cudaStream_t stream) {
                           interactionManager->executeLocalIntermediate(stream);
                       });

    scheduler->addTask(tasks->haloIntermediate,
                       [this] (cudaStream_t stream) {
                           interactionManager->executeHaloIntermediate(stream);
                       });

    scheduler->addTask(tasks->localForces,
                       [this] (cudaStream_t stream) {
                           interactionManager->executeLocalFinal(stream);
                       });

    scheduler->addTask(tasks->haloForces,
                       [this] (cudaStream_t stream) {
                           interactionManager->executeHaloFinal(stream);
                       });
    

    scheduler->addTask(tasks->gatherInteractionIntermediate,
                       [this] (cudaStream_t stream) {
                           interactionManager->gatherIntermediate(stream);
                       });

    scheduler->addTask(tasks->accumulateInteractionIntermediate,
                       [this] (cudaStream_t stream) {
                           interactionManager->accumulateIntermediates(stream);
                       });
            
    scheduler->addTask(tasks->accumulateInteractionFinal,
                       [this] (cudaStream_t stream) {
                           interactionManager->accumulateFinal(stream);
                       });


    for (auto& integrator : integratorsStage2)
        scheduler->addTask(tasks->integration, [integrator, this] (cudaStream_t stream) {
            integrator(stream);
        });


    // As there are no primary cell-lists for objects
    // we need to separately clear real obj forces and forces in the cell-lists
    for (auto ov : objectVectors)
    {
        scheduler->addTask(tasks->objClearLocalIntermediate, [this, ov] (cudaStream_t stream) {
            interactionManager->clearIntermediates(ov, stream);
            interactionManager->clearIntermediatesPV(ov, ov->local(), stream);
        });

        scheduler->addTask(tasks->objClearHaloIntermediate, [this, ov] (cudaStream_t stream) {
            interactionManager->clearIntermediatesPV(ov, ov->halo(), stream);
        });

        scheduler->addTask(tasks->objClearLocalForces, [this, ov] (cudaStream_t stream) {
            interactionManager->clearFinalPV(ov, ov->local(), stream);
            interactionManager->clearFinal(ov, stream);
        });

        scheduler->addTask(tasks->objClearHaloForces, [this, ov] (cudaStream_t stream) {
            interactionManager->clearFinalPV(ov, ov->halo(), stream);
        });
    }

    for (auto& bouncer : regularBouncers)
        scheduler->addTask(tasks->objLocalBounce, [bouncer, this] (cudaStream_t stream) {
            bouncer(stream);
    });

    for (auto& bouncer : haloBouncers)
        scheduler->addTask(tasks->objHaloBounce, [bouncer, this] (cudaStream_t stream) {
            bouncer(stream);
    });

    for (auto& prototype : belongingCorrectionPrototypes)
    {
        auto checker = prototype.checker;
        auto pvIn    = prototype.pvIn;
        auto pvOut   = prototype.pvOut;
        auto every   = prototype.every;

        if (every > 0)
        {
            scheduler->addTask(tasks->correctObjBelonging, [checker, pvIn, pvOut] (cudaStream_t stream) {
                if (pvIn  != nullptr) checker->splitByBelonging(pvIn,  pvIn, pvOut, stream);
                if (pvOut != nullptr) checker->splitByBelonging(pvOut, pvIn, pvOut, stream);
            }, every);
        }
    }

    if (objectVectors.size() > 0)
    {
        scheduler->addTask(tasks->objHaloIntermediateInit, [this] (cudaStream_t stream) {
            objHaloIntermediate->init(stream);
        });

        scheduler->addTask(tasks->objHaloIntermediateFinalize, [this] (cudaStream_t stream) {
            objHaloIntermediate->finalize(stream);
        });

        scheduler->addTask(tasks->objHaloFinalInit, [this] (cudaStream_t stream) {
            objHaloFinal->init(stream);
        });

        scheduler->addTask(tasks->objHaloFinalFinalize, [this] (cudaStream_t stream) {
            objHaloFinal->finalize(stream);
        });

        scheduler->addTask(tasks->objReverseIntermediateInit, [this] (cudaStream_t stream) {
            objHaloReverseIntermediate->init(stream);
        });

        scheduler->addTask(tasks->objReverseIntermediateFinalize, [this] (cudaStream_t stream) {
            objHaloReverseIntermediate->finalize(stream);
        });

        scheduler->addTask(tasks->objReverseFinalInit, [this] (cudaStream_t stream) {
            objHaloReverseFinal->init(stream);
        });

        scheduler->addTask(tasks->objReverseFinalFinalize, [this] (cudaStream_t stream) {
            objHaloReverseFinal->finalize(stream);
        });

        scheduler->addTask(tasks->objRedistInit, [this] (cudaStream_t stream) {
            objRedistibutor->init(stream);
        });

        scheduler->addTask(tasks->objRedistFinalize, [this] (cudaStream_t stream) {
            objRedistibutor->finalize(stream);
        });
    }

    for (auto& wall : wallMap)
    {
        auto wallPtr = wall.second.get();
        scheduler->addTask(tasks->wallBounce, [wallPtr, this] (cudaStream_t stream) {    
            wallPtr->bounce(stream);
        });
    }

    for (auto& prototype : checkWallPrototypes)
    {
        auto wall  = prototype.wall;
        auto every = prototype.every;

        if (every > 0)
            scheduler->addTask(tasks->wallCheck, [this, wall] (cudaStream_t stream) { wall->check(stream); }, every);
    }
}

static void createTasksDummy(TaskScheduler *scheduler, SimulationTasks *tasks)
{
#define INIT(NAME, DESC) tasks -> NAME = scheduler->createTask(DESC);
#define DUMMY_TASK(NAME, DESC) scheduler->addTask(tasks->NAME, [](cudaStream_t) {info("executing " DESC);});

    TASK_LIST(INIT);
    TASK_LIST(DUMMY_TASK);

#undef INIT
#undef DUMMY_TASK
}

static void buildDependencies(TaskScheduler *scheduler, SimulationTasks *tasks)
{
    scheduler->addDependency(tasks->pluginsBeforeCellLists, { tasks->cellLists }, {});
    
    scheduler->addDependency(tasks->checkpoint, { tasks->partClearFinal }, { tasks->cellLists });

    scheduler->addDependency(tasks->correctObjBelonging, { tasks->cellLists }, {});

    scheduler->addDependency(tasks->cellLists, {tasks->partClearFinal, tasks->partClearIntermediate, tasks->objClearLocalIntermediate}, {});

    
    scheduler->addDependency(tasks->pluginsBeforeForces, {tasks->localForces, tasks->haloForces}, {tasks->partClearFinal});
    scheduler->addDependency(tasks->pluginsSerializeSend, {tasks->pluginsBeforeIntegration, tasks->pluginsAfterIntegration}, {tasks->pluginsBeforeForces});

    scheduler->addDependency(tasks->objClearHaloForces, {tasks->objHaloBounce}, {tasks->objHaloFinalFinalize});

    scheduler->addDependency(tasks->objReverseFinalInit, {}, {tasks->haloForces});
    scheduler->addDependency(tasks->objReverseFinalFinalize, {tasks->accumulateInteractionFinal}, {tasks->objReverseFinalInit});

    scheduler->addDependency(tasks->localIntermediate, {}, {tasks->partClearIntermediate, tasks->objClearLocalIntermediate});
    scheduler->addDependency(tasks->partHaloIntermediateInit, {}, {tasks->partClearIntermediate, tasks->cellLists});
    scheduler->addDependency(tasks->partHaloIntermediateFinalize, {}, {tasks->partHaloIntermediateInit});

    scheduler->addDependency(tasks->objClearHaloIntermediate, {}, {tasks->cellLists});
    scheduler->addDependency(tasks->haloIntermediate, {}, {tasks->partHaloIntermediateFinalize, tasks->objClearHaloIntermediate});
    scheduler->addDependency(tasks->objReverseIntermediateInit, {}, {tasks->haloIntermediate});    
    scheduler->addDependency(tasks->objReverseIntermediateFinalize, {}, {tasks->objReverseIntermediateInit});

    scheduler->addDependency(tasks->accumulateInteractionIntermediate, {}, {tasks->localIntermediate, tasks->haloIntermediate});
    scheduler->addDependency(tasks->gatherInteractionIntermediate, {}, {tasks->accumulateInteractionIntermediate, tasks->objReverseIntermediateFinalize});

    scheduler->addDependency(tasks->localForces, {}, {tasks->gatherInteractionIntermediate});

    scheduler->addDependency(tasks->objHaloIntermediateInit, {}, {tasks->gatherInteractionIntermediate});
    scheduler->addDependency(tasks->objHaloIntermediateFinalize, {}, {tasks->objHaloIntermediateInit});
    
    scheduler->addDependency(tasks->partHaloFinalInit, {}, {tasks->pluginsBeforeForces, tasks->gatherInteractionIntermediate});
    scheduler->addDependency(tasks->partHaloFinalFinalize, {}, {tasks->partHaloFinalInit});

    scheduler->addDependency(tasks->haloForces, {}, {tasks->partHaloFinalFinalize, tasks->objHaloIntermediateFinalize});
    scheduler->addDependency(tasks->accumulateInteractionFinal, {tasks->integration}, {tasks->haloForces, tasks->localForces});

    scheduler->addDependency(tasks->pluginsBeforeIntegration, {tasks->integration}, {tasks->accumulateInteractionFinal});
    scheduler->addDependency(tasks->wallBounce, {}, {tasks->integration});
    scheduler->addDependency(tasks->wallCheck, {tasks->partRedistributeInit}, {tasks->wallBounce});

    scheduler->addDependency(tasks->objHaloFinalInit, {}, {tasks->integration, tasks->objRedistFinalize});
    scheduler->addDependency(tasks->objHaloFinalFinalize, {}, {tasks->objHaloFinalInit});

    scheduler->addDependency(tasks->objLocalBounce, {tasks->objHaloFinalFinalize}, {tasks->integration, tasks->objClearLocalForces});
    scheduler->addDependency(tasks->objHaloBounce, {}, {tasks->integration, tasks->objHaloFinalFinalize, tasks->objClearHaloForces});

    scheduler->addDependency(tasks->pluginsAfterIntegration, {tasks->objLocalBounce, tasks->objHaloBounce}, {tasks->integration, tasks->wallBounce});

    scheduler->addDependency(tasks->pluginsBeforeParticlesDistribution, {},
                             {tasks->integration, tasks->wallBounce, tasks->objLocalBounce, tasks->objHaloBounce, tasks->pluginsAfterIntegration});
    scheduler->addDependency(tasks->partRedistributeInit, {}, {tasks->pluginsBeforeParticlesDistribution});
    scheduler->addDependency(tasks->partRedistributeFinalize, {}, {tasks->partRedistributeInit});

    scheduler->addDependency(tasks->objRedistInit, {}, {tasks->integration, tasks->wallBounce, tasks->objReverseFinalFinalize, tasks->pluginsAfterIntegration});
    scheduler->addDependency(tasks->objRedistFinalize, {}, {tasks->objRedistInit});
    scheduler->addDependency(tasks->objClearLocalForces, {tasks->objLocalBounce}, {tasks->integration, tasks->objRedistFinalize});

    scheduler->setHighPriority(tasks->objReverseFinalInit);
    scheduler->setHighPriority(tasks->partHaloIntermediateInit);
    scheduler->setHighPriority(tasks->partHaloIntermediateFinalize);
    scheduler->setHighPriority(tasks->objHaloIntermediateInit);
    scheduler->setHighPriority(tasks->objHaloIntermediateFinalize);
    scheduler->setHighPriority(tasks->objClearHaloIntermediate);
    scheduler->setHighPriority(tasks->objReverseFinalInit);
    scheduler->setHighPriority(tasks->objReverseFinalFinalize);
    scheduler->setHighPriority(tasks->haloIntermediate);
    scheduler->setHighPriority(tasks->partHaloFinalInit);
    scheduler->setHighPriority(tasks->partHaloFinalFinalize);
    scheduler->setHighPriority(tasks->haloForces);
    scheduler->setHighPriority(tasks->pluginsSerializeSend);

    scheduler->setHighPriority(tasks->objClearLocalForces);
    scheduler->setHighPriority(tasks->objLocalBounce);
    
    scheduler->compile();
}

void Simulation::init()
{
    info("Simulation initiated");

    prepareCellLists();

    prepareInteractions();
    prepareBouncers();
    prepareWalls();

    interactionManager->check();

    CUDA_Check( cudaDeviceSynchronize() );

    preparePlugins();
    prepareEngines();

    info("Time-step is set to %f", getCurrentDt());
    
    createTasks();
    buildDependencies(scheduler.get(), tasks.get());
    
    // Initial preparation
    scheduler->forceExec( tasks->objHaloFinalInit,     defaultStream );
    scheduler->forceExec( tasks->objHaloFinalFinalize, defaultStream );
    scheduler->forceExec( tasks->objClearHaloForces,   defaultStream );
    scheduler->forceExec( tasks->objClearLocalForces,  defaultStream );

    execSplitters();
}


void Simulation::run(int nsteps)
{
    int begin = state->currentStep, end = state->currentStep + nsteps;

    info("Will run %d iterations now", nsteps);


    for (state->currentStep = begin; state->currentStep < end; state->currentStep++)
    {
        debug("===============================================================================\n"
                "Timestep: %d, simulation time: %f", state->currentStep, state->currentTime);

        scheduler->run();
        
        state->currentTime += state->dt;
    }

    // Finish the redistribution by rebuilding the cell-lists
    scheduler->forceExec( tasks->cellLists, defaultStream );

    info("Finished with %d iterations", nsteps);
    MPI_Check( MPI_Barrier(cartComm) );

    for (auto& pl : plugins)
        pl->finalize();

    if (interComm != MPI_COMM_NULL)
    {
        int dummy = -1;
        int tag = 424242;

        MPI_Check( MPI_Send(&dummy, 1, MPI_INT, rank, tag, interComm) );
        debug("Sending stopping message to the postprocess");
    }
}


void Simulation::restart(std::string folder)
{
//    bool beginning =  particleVectors    .empty() &&
//                      wallMap            .empty() &&
//                      interactionMap     .empty() &&
//                      integratorMap      .empty() &&
//                      bouncerMap         .empty() &&
//                      belongingCheckerMap.empty() &&
//                      plugins            .empty();
//
//    if (!beginning)
//        die("Tried to restart partially initialized simulation! Please only call restart() before registering anything");
//
//    restartStatus = RestartStatus::RestartStrict;
//    restartFolder = folder;
//
//    TextIO::read(folder + "_simulation.state", state->currentTime, state->currentStep);

	TextIO::read(folder + "_simulation.state", state->currentTime, state->currentStep);
	restartFolder = folder;

	CUDA_Check( cudaDeviceSynchronize() );

	info("Reading simulation state, from folder %s", restartFolder.c_str());

	for (auto& pv : particleVectors)
		pv->restart(cartComm, restartFolder);

	for (auto& handler : bouncerMap)
		handler.second->restart(cartComm, restartFolder);

	for (auto& handler : integratorMap)
		handler.second->restart(cartComm, restartFolder);

	for (auto& handler : interactionMap)
		handler.second->restart(cartComm, restartFolder);

	for (auto& handler : wallMap)
		handler.second->restart(cartComm, restartFolder);

	for (auto& handler : belongingCheckerMap)
		handler.second->restart(cartComm, restartFolder);

	for (auto& handler : plugins)
		handler->restart(cartComm, restartFolder);

	CUDA_Check( cudaDeviceSynchronize() );
}

void Simulation::checkpoint()
{
    if (rank == 0)
        TextIO::write(checkpointFolder + "_simulation.state", state->currentTime, state->currentStep);

    CUDA_Check( cudaDeviceSynchronize() );
    
    info("Writing simulation state, into folder %s", checkpointFolder.c_str());
        
    for (auto& pv : particleVectors)
        pv->checkpoint(cartComm, checkpointFolder);
    
    for (auto& handler : bouncerMap)
        handler.second->checkpoint(cartComm, checkpointFolder);
    
    for (auto& handler : integratorMap)
        handler.second->checkpoint(cartComm, checkpointFolder);
    
    for (auto& handler : interactionMap)
        handler.second->checkpoint(cartComm, checkpointFolder);
    
    for (auto& handler : wallMap)
        handler.second->checkpoint(cartComm, checkpointFolder);
    
    for (auto& handler : belongingCheckerMap)
        handler.second->checkpoint(cartComm, checkpointFolder);
    
    for (auto& handler : plugins)
        handler->checkpoint(cartComm, checkpointFolder);
    
    CUDA_Check( cudaDeviceSynchronize() );
}

void Simulation::saveDependencyGraph_GraphML(std::string fname, bool current) const
{
    if (rank != 0) return;

    if (current)
    {
        scheduler->saveDependencyGraph_GraphML(fname);
    }
    else
    {
        TaskScheduler s;
        SimulationTasks t;
        
        createTasksDummy(&s, &t);
        buildDependencies(&s, &t);

        s.saveDependencyGraph_GraphML(fname);
    }
}
