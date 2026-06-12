#pragma once
#include <juce_dsp/juce_dsp.h>
#include <array>
#include <cmath>

// Real-time fuzz character for one LO FX channel.
// AGC-normalise (so the heavy drive bites regardless of input level)
// -> oversampled asym/hard drive -> per-band grit (cascade harmonics added per band)
// -> [cab IR convolved by the host processor] -> de-fizz / body / warmth / low-HPF.
struct FuzzChain
{
    // locked params (set per channel via configure)
    float drive = 120.0f, asym = 0.2f, hard = 0.7f, fizz = 0.8f, body = 2.8f, warmth = 0.6f, lo = 0.0f;
    float level = 1.0f;              // output trim — matches fuzz loudness to the clean voicing
    std::array<float, 8> g { {} };   // per-band grit amounts (centres below)

    static constexpr std::array<float, 8> CENTRES { {100.f, 250.f, 470.f, 780.f, 1300.f, 2300.f, 3800.f, 6000.f} };

    void configure (float dr, float as, float hd, float fz, float bd, float wm, float low,
                    float levelDb, std::array<float, 8> grit)
    { drive = dr; asym = as; hard = hd; fizz = fz; body = bd; warmth = wm; lo = low;
      level = juce::Decibels::decibelsToGain (levelDb); g = grit; }

    // base-rate latency the 4x oversampler adds to this path (sub-sample for the IIR halfband)
    float oversamplingLatency() const { return os != nullptr ? os->getLatencyInSamples() : 0.0f; }

    void prepare (double sampleRate, int blockSize)
    {
        sr = sampleRate;
        peak = 0.0f;
        atk = 1.0f - std::exp (-1.0f / (float) (sr * 0.005));   // 5 ms
        rel = 1.0f - std::exp (-1.0f / (float) (sr * 0.250));   // 250 ms
        env.allocate ((size_t) blockSize, true);

        os = std::make_unique<juce::dsp::Oversampling<float>> (
                 1, 2, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR);  // 4x
        os->initProcessing ((size_t) blockSize);

        juce::dsp::ProcessSpec mono { sampleRate, (juce::uint32) blockSize, 1 };
        for (size_t b = 0; b < 8; ++b)
        {
            gritBP[b].prepare (mono);
            gritBP[b].coefficients = juce::dsp::IIR::Coefficients<float>::makeBandPass (sr, CENTRES[b], 1.4f);
            gritBP[b].reset();
        }
        for (auto* f : { &defizz, &bodyF, &warm1, &warm2, &hpf }) f->prepare (mono);
        designPostEQ();
    }

    void designPostEQ()
    {
        using C = juce::dsp::IIR::Coefficients<float>;
        defizz.coefficients = C::makeHighShelf (sr, 7000.0f, 0.7f, juce::Decibels::decibelsToGain (-13.0f * fizz));
        bodyF.coefficients  = C::makePeakFilter (sr, 140.0f, 1.2f, juce::Decibels::decibelsToGain (body * 4.0f));
        warm1.coefficients  = C::makePeakFilter (sr, 75.0f,  0.5f, juce::Decibels::decibelsToGain (warmth * 4.0f));
        warm2.coefficients  = C::makePeakFilter (sr, 205.0f, 1.3f, juce::Decibels::decibelsToGain (warmth * 3.5f));
        const float hpHz = lo < 0.0f ? 20.0f + (-lo) * 35.0f : 20.0f;
        hpf.coefficients = C::makeHighPass (sr, juce::jlimit (20.0f, (float) sr * 0.45f, hpHz));
        defizz.reset(); bodyF.reset(); warm1.reset(); warm2.reset(); hpf.reset();
    }

    static float clip (float x, float dr, float as, float hd) noexcept
    {
        const float gg = x * dr;
        const float soft = gg >= 0.0f ? std::tanh (gg) : as * std::tanh (gg / std::max (as, 1.0e-3f));
        const float hc = juce::jlimit (-1.0f, 1.0f, gg);
        return (1.0f - hd) * soft + hd * hc;
    }

    // pre-cab: AGC normalise -> oversampled drive -> per-band grit -> restore level
    void processPreCab (float* x, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            const float a = std::abs (x[i]);
            peak += (a > peak ? atk : rel) * (a - peak);
            env[(size_t) i] = juce::jmax (peak, 0.03f);
            x[i] /= env[(size_t) i];                          // normalise toward unity
        }

        float* ch[1] = { x };
        juce::dsp::AudioBlock<float> blk (ch, 1, (size_t) n);
        auto up = os->processSamplesUp (blk);
        const int upN = (int) up.getNumSamples();
        for (int s = 0; s < upN; ++s)
            up.setSample (0, s, clip (up.getSample (0, s), drive, asym, hard));
        os->processSamplesDown (blk);

        for (int i = 0; i < n; ++i)
        {
            const float d  = x[i];
            const float d2 = clip (d, 3.0f, asym, hard);       // grittier cascade
            const float diff = d2 - d;                          // extra harmonics to sprinkle per band
            float grit = 0.0f;
            for (size_t b = 0; b < 8; ++b)
                grit += g[b] * gritBP[b].processSample (diff);
            x[i] = (d + grit) * env[(size_t) i];                // restore the note's dynamics
        }
    }

    // post-cab tone shaping
    void processPostCab (float* x, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            float v = defizz.processSample (x[i]);
            v = bodyF.processSample (v);
            v = warm1.processSample (v);
            v = warm2.processSample (v);
            if (lo < 0.0f) v = hpf.processSample (v);
            x[i] = v * level;
        }
    }

private:
    double sr = 48000.0;
    float peak = 0.0f, atk = 0.0f, rel = 0.0f;
    juce::HeapBlock<float> env;
    std::unique_ptr<juce::dsp::Oversampling<float>> os;
    std::array<juce::dsp::IIR::Filter<float>, 8> gritBP;
    juce::dsp::IIR::Filter<float> defizz, bodyF, warm1, warm2, hpf;
};
