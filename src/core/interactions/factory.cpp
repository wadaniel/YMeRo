#include "factory.h"

#include "membrane/parameters.h"

#include "membrane_WLC_Kantor.h"
#include "membrane_WLC_Juelicher.h"
#include "membrane_Lim_Kantor.h"
#include "membrane_Lim_Juelicher.h"

#include "pairwise_interactions/density_kernels.h"
#include "pairwise_interactions/pressure_EOS.h"

#include "sdpd.h"
#include "sdpd_with_stress.h"

#include <core/logger.h>

static bool hasKey(const std::map<std::string, float>& desc, const std::string& key)
{
    return desc.find(key) != desc.end();
}

static float readFloat(const std::map<std::string, float>& desc, const std::string& key)
{
    auto it = desc.find(key);
    
    if (it == desc.end())
        die("missing parameter '%s'", key.c_str());
    
    return it->second;
}

static CommonMembraneParameters readCommonParameters(const std::map<std::string, float>& desc)
{
    CommonMembraneParameters p;

    p.totArea0    = readFloat(desc, "tot_area");
    p.totVolume0  = readFloat(desc, "tot_volume");

    p.ka = readFloat(desc, "ka_tot");
    p.kv = readFloat(desc, "kv_tot");

    p.gammaC = readFloat(desc, "gammaC");
    p.gammaT = readFloat(desc, "gammaT");
    p.kBT    = readFloat(desc, "kBT");

    p.fluctuationForces = (p.kBT > 1e-6);
    
    return p;
}

static WLCParameters readWLCParameters(const std::map<std::string, float>& desc)
{
    WLCParameters p;

    p.x0   = readFloat(desc, "x0");
    p.ks   = readFloat(desc, "ks");
    p.mpow = readFloat(desc, "mpow");

    p.kd = readFloat(desc, "ka");
    p.totArea0 = readFloat(desc, "tot_area");
    
    return p;
}

static LimParameters readLimParameters(const std::map<std::string, float>& desc)
{
    LimParameters p;

    p.ka = readFloat(desc, "ka");
    p.a3 = readFloat(desc, "a3");
    p.a4 = readFloat(desc, "a4");
    
    p.mu = readFloat(desc, "mu");
    p.b1 = readFloat(desc, "b1");
    p.b2 = readFloat(desc, "b2");

    p.totArea0 = readFloat(desc, "tot_area");
    
    return p;
}

static KantorBendingParameters readKantorParameters(const std::map<std::string, float>& desc)
{
    KantorBendingParameters p;

    p.kb    = readFloat(desc, "kb");
    p.theta = readFloat(desc, "theta");
    
    return p;
}

static JuelicherBendingParameters readJuelicherParameters(const std::map<std::string, float>& desc)
{
    JuelicherBendingParameters p;

    p.kb = readFloat(desc, "kb");
    p.C0 = readFloat(desc, "C0");

    p.kad = readFloat(desc, "kad");
    p.DA0 = readFloat(desc, "DA0");
    
    return p;
}

static bool isWLC(const std::string& desc)
{
    return desc == "wlc";
}

static bool isLim(const std::string& desc)
{
    return desc == "Lim";
}

static bool isKantor(const std::string& desc)
{
    return desc == "Kantor";
}

static bool isJuelicher(const std::string& desc)
{
    return desc == "Juelicher";
}


std::shared_ptr<InteractionMembrane>
InteractionFactory::createInteractionMembrane(const YmrState *state, std::string name,
                                              std::string shearDesc, std::string bendingDesc,
                                              const std::map<std::string, float>& parameters,
                                              bool stressFree, float growUntil)
{
    auto commonPrms = readCommonParameters(parameters);
    
    if (isWLC(shearDesc))
    {
        auto shPrms = readWLCParameters(parameters);
            
        if (isKantor(bendingDesc))
        {
            auto bePrms = readKantorParameters(parameters);
            return std::make_shared<InteractionMembraneWLCKantor>
                (state, name, commonPrms, shPrms, bePrms, stressFree, growUntil);
        }

        if (isJuelicher(bendingDesc))
        {
            auto bePrms = readJuelicherParameters(parameters);
            return std::make_shared<InteractionMembraneWLCJuelicher>
                (state, name, commonPrms, shPrms, bePrms, stressFree, growUntil);
        }            
    }

    if (isLim(shearDesc))
    {
        auto shPrms = readLimParameters(parameters);

        if (isKantor(bendingDesc))
        {
            auto bePrms = readKantorParameters(parameters);
            return std::make_shared<InteractionMembraneLimKantor>
                (state, name, commonPrms, shPrms, bePrms, stressFree, growUntil);
        }

        if (isJuelicher(bendingDesc))
        {
            auto bePrms = readJuelicherParameters(parameters);
            return std::make_shared<InteractionMembraneLimJuelicher>
                (state, name, commonPrms, shPrms, bePrms, stressFree, growUntil);
        }
    }
    
    die("argument combination of shearDesc = '%s' and bendingDesc = '%s' is incorrect",
        shearDesc.c_str(), bendingDesc.c_str());

    return nullptr;
}


static bool isSimpleMDPDDensity(const std::string& desc)
{
    return desc == "MDPD";
}


static bool isWendlandC2Density(const std::string& desc)
{
    return desc == "WendlandC2";
}

std::shared_ptr<BasicInteractionDensity>
InteractionFactory::createPairwiseDensity(const YmrState *state, std::string name, float rc,
                                          const std::string& density)
{
    if (isSimpleMDPDDensity(density))
    {
        SimpleMDPDDensityKernel densityKernel;
        return std::make_shared<InteractionDensity<SimpleMDPDDensityKernel>>
                                (state, name, rc, densityKernel);
    }
    
    if (isWendlandC2Density(density))
    {
        WendlandC2DensityKernel densityKernel;
        return std::make_shared<InteractionDensity<WendlandC2DensityKernel>>
                                (state, name, rc, densityKernel);
    }

    die("Invalid density '%s'", density.c_str());
    return nullptr;
}


static LinearPressureEOS readLinearPressureEOS(const std::map<std::string, float>& desc)
{
    float c = readFloat(desc, "sound_speed");
    return LinearPressureEOS(c);
}

static QuasiIncompressiblePressureEOS readQuasiIncompressiblePressureEOS(const std::map<std::string, float>& desc)
{
    float p0   = readFloat(desc, "p0");
    float rhor = readFloat(desc, "rho_r");
    
    return QuasiIncompressiblePressureEOS(p0, rhor);
}

static bool isLinearEOS(const std::string& desc)
{
    return desc == "Linear";
}

static bool isQuasiIncompressibleEOS(const std::string& desc)
{
    return desc == "QuasiIncompressible";
}


template <typename PressureKernel, typename DensityKernel>
static std::shared_ptr<BasicInteractionSDPD>
allocatePairwiseSDPD(const YmrState *state, std::string name, float rc,
                     PressureKernel pressure, DensityKernel density,
                     float viscosity, float kBT,
                     bool stress, float stressPeriod)
{
    if (stress)
        return std::make_shared<InteractionSDPDWithStress<PressureKernel, DensityKernel>>
            (state, name, rc, pressure, density, viscosity, kBT, stressPeriod);
    else
        return std::make_shared<InteractionSDPD<PressureKernel, DensityKernel>>
            (state, name, rc, pressure, density, viscosity, kBT);
}


std::shared_ptr<BasicInteractionSDPD>
InteractionFactory::createPairwiseSDPD(const YmrState *state, std::string name, float rc, float viscosity, float kBT,
                                       const std::string& EOS, const std::string& density, bool stress,
                                       const std::map<std::string, float>& parameters)
{
    float stressPeriod = 0.f;

    if (stress)
        stressPeriod = readFloat(parameters, "stress_period");
    
    if (!isWendlandC2Density(density))
        die("Invalid density '%s'", density.c_str());

    WendlandC2DensityKernel densityKernel;
    
    if (isLinearEOS(EOS))
    {
        auto pressure = readLinearPressureEOS(parameters);
        return allocatePairwiseSDPD(state, name, rc, pressure, densityKernel, viscosity, kBT, stress, stressPeriod);
    }

    if (isQuasiIncompressibleEOS(EOS))
    {
        auto pressure = readQuasiIncompressiblePressureEOS(parameters);
        return allocatePairwiseSDPD(state, name, rc, pressure, densityKernel, viscosity, kBT, stress, stressPeriod);
    }

    die("Invalid pressure parameter: '%s'", EOS.c_str());
    return nullptr;
}
