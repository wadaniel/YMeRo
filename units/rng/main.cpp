#include <core/logger.h>
#include <core/interactions/utils/step_random_gen.h>

#include <cmath>
#include <cstdio>
#include <gtest/gtest.h>
#include <random>

Logger logger;

template<typename Gen>
static std::vector<float> generateSamples(Gen gen, float dt, long n)
{
    std::vector<float> samples (n);
    YmrState state(DomainInfo(), dt);
    state.currentTime = 0;    

    for (state.currentStep = 0; state.currentStep < n; ++state.currentStep)
    {
        samples[state.currentStep] = gen.generate(&state);
        state.currentTime += state.dt;
    }

    return samples;
}

using real = long double;

template<typename Gen>
static real computeAutoCorrelation(Gen gen, float dt, long n)
{
    auto samples = generateSamples(gen, dt, n);
    
    real sum = 0;
    for (const auto& x : samples) sum += (real) x;

    real mean = sum / n;
    real covariance = 0;
    real mean_sq = mean*mean;

    for (int i = 1; i < n; ++i)
        covariance += samples[i] * samples[i-1] - mean_sq;

    return covariance / n;
}

class GenFromTime
{
public:
    float generate(const YmrState *state)
    {
        float t = state->currentTime;
        int v = *((int*)&t);
        std::mt19937 gen(v);
        std::uniform_real_distribution<float> udistr(0.001, 1);
        return udistr(gen);
    }
};

TEST (RNG, autoCorrelationGenFromTime)
{
    GenFromTime gen;
    float dt = 1e-3;
    
    auto corr = computeAutoCorrelation(gen, dt, 10000);

    printf("from time: %g\n", (double) corr);
                                       
    ASSERT_LE(abs(corr), 1e-3);
}

TEST (RNG, autoCorrelationGenFromMT)
{
    StepRandomGen gen(424242);
    float dt = 1e-3;
    
    auto corr = computeAutoCorrelation(gen, dt, 10000);

    printf("from MT: %g\n", (double) corr);
                                       
    ASSERT_LE(abs(corr), 1e-3);
}

int main(int argc, char **argv)
{
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
