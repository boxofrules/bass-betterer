// Bass Better-er — © 2026 Box of Rules. Source-available, proprietary — see LICENSE.
#pragma once
#include <juce_dsp/juce_dsp.h>
#include <array>
#include <cmath>
#include "DriveParams.h"

// Real-time drive character for one drive-capable strip.
// AGC-normalise (so the heavy drive bites regardless of input level)
// -> tightness/bite pre-shaping -> oversampled asym/hard drive
// -> per-band grit (cascade harmonics, scaled by casc) -> casc<0 warmth shelf
// -> tame (transient reduction) -> [cab IR convolved by the host processor]
// -> de-fizz / clean-low blend / presence / body / warmth / low-HPF
// -> level trim.
//
// v0.2.0: the chain is parameterised by the drive:: 10-param set (a fixed
// FUZZ or DIST character per strip) instead of per-mic constants.
// BYTE-IDENTITY CONTRACT: with the FUZZ character (== the locked v1
// constants) the render is bit-for-bit the pre-v0.2.0 fixed fuzz
// (tools/bor_bench.cpp `fnv` prints render checksums as the gate). The
// added stages hold that by construction:
//  - tightness/bite/presence and the casc<0 warmth shelf are always-in-graph
//    biquads designed at unity gain when neutral; JUCE's RBJ shelf/peak
//    designs collapse to b == a at gain 1, which the DF2T filter passes
//    BIT-EXACTLY (its state stays zero);
//  - amount maps to the clipper drive via the power-of-two DRIVE_PER_AMOUNT,
//    so the locked drives (150/84/97) reproduce exactly;
//  - casc >= 0 scales the grit table (x 1.0 at FUZZ = exact); tame and the
//    clean-low blend are branch-skipped at their neutral values.
struct FuzzChain
{
    static constexpr std::array<float, 8> CENTRES { {100.f, 250.f, 470.f, 780.f, 1300.f, 2300.f, 3800.f, 6000.f} };

    // low > 0 blends the clean low end (LR4 @ 200 Hz of the strip input) back
    // in post-cab. The blend source runs much hotter than the post-cab drive
    // signal at the plugin's internal scale, so it is scaled down: low at
    // full-scale (3.0) lands comparable to the drive's own low band. FUZZ
    // ships low <= 0, so this stage is byte-identity-neutral.
    static constexpr float LOW_BLEND_SCALE = 0.125f;

    // Per-strip locked mic-chain shaping (fizz/warmth/level trim/grit table).
    // Cheap no-op when unchanged; a character switch retunes without
    // resetting state.
    void setLocks (const drive::Locks& l)
    {
        // exact float compares throughout: these are change detectors on values
        // copied verbatim from the tables/params, not numeric tolerance checks
        if (locksValid && juce::exactlyEqual (l.fizz, locks.fizz)
            && juce::exactlyEqual (l.warmth, locks.warmth)
            && juce::exactlyEqual (l.levelDb, locks.levelDb) && l.grit == locks.grit)
            return;
        const bool fizzChanged   = ! locksValid || ! juce::exactlyEqual (l.fizz,   locks.fizz);
        const bool warmthChanged = ! locksValid || ! juce::exactlyEqual (l.warmth, locks.warmth);
        locks = l;
        locksValid = true;
        level = juce::Decibels::decibelsToGain (locks.levelDb);
        if (sr > 0.0)
        {
            if (fizzChanged)   designFizz();
            if (warmthChanged) designWarmth();
        }
    }

    // The 10 drive params (drive::ParamIndex order). Scalars update every
    // call (cheap); filters redesign only when their param moved. Filters are
    // never reset here: a running redesign keeps state so character switches
    // glide instead of clicking.
    void setCharacter (const drive::Params& p)
    {
        using namespace drive;
        driveG    = DRIVE_PER_AMOUNT * p[Amount];
        asym      = p[Asym];
        hard      = p[Hard];
        gritScale = juce::jmax (0.0f, p[Casc]);
        tame      = p[Tame];
        lowBlend  = p[Low];
        blendGain = LOW_BLEND_SCALE * p[Low];

        if (sr > 0.0)
        {
            auto moved = [this, &p] (int i) { return ! juce::exactlyEqual (p[(size_t) i], cur[(size_t) i]); };
            if (moved (Tightness)) designTight    (p[Tightness]);
            if (moved (Bite))      designBite     (p[Bite]);
            if (moved (Casc))      designCascWarm (p[Casc]);
            if (moved (Presence))  designPresence (p[Presence]);
            if (moved (Body))      designBody     (p[Body]);
            if (moved (Low))       designHpf      (p[Low]);
        }
        cur = p;
    }

    // base-rate latency the 4x oversampler adds to this path (sub-sample for the IIR halfband)
    float oversamplingLatency() const { return os != nullptr ? os->getLatencyInSamples() : 0.0f; }

    // Reshape the AGC envelope (the RELEASE/SUSTAIN dials). At the locked
    // drive amounts the clipper saturates fully, so after the restore-multiply
    // the strip's output envelope IS the AGC follower: a fast release falls
    // from each pluck's attack peak down to the body level (heard as a
    // sidechain-style duck) and then tracks the DI's decay instead of
    // sustaining. Slower release flattens that dip; restoreExp < 1 restores
    // env^exp so the strip's dynamics compress upward (more even sustain).
    // 5 ms / 250 ms / 1.0 reproduces every pre-v1 render byte-identically.
    void setEnvShape (float atkSeconds, float relSeconds, float restoreExponent)
    {
        if (juce::exactlyEqual (atkSeconds, atkSec) && juce::exactlyEqual (relSeconds, relSec)
            && juce::exactlyEqual (restoreExponent, restoreExp))
            return;
        atkSec = atkSeconds; relSec = relSeconds; restoreExp = restoreExponent;
        if (sr > 0.0)
        {
            atk = 1.0f - std::exp (-1.0f / (float) (sr * atkSec));
            rel = 1.0f - std::exp (-1.0f / (float) (sr * relSec));
        }
    }

    void prepare (double sampleRate, int blockSize)
    {
        sr = sampleRate;
        peak = 0.0f;
        atk = 1.0f - std::exp (-1.0f / (float) (sr * atkSec));   // stock 5 ms
        rel = 1.0f - std::exp (-1.0f / (float) (sr * relSec));   // stock 250 ms
        env.allocate ((size_t) blockSize, true);
        cleanLow.allocate ((size_t) blockSize, true);

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
        for (auto* f : { &tightShelf, &biteShelf, &cascWarm, &defizz, &presenceF,
                         &bodyF, &warm1, &warm2, &hpf })
            f->prepare (mono);

        // clean-low source for the low>0 blend
        cleanLP.setType (juce::dsp::LinkwitzRileyFilterType::lowpass);
        cleanLP.setCutoffFrequency (200.0f);
        cleanLP.prepare (mono);
        cleanLP.reset();

        // full design from the current param/lock state, then reset (prepare is
        // the only place filter state is cleared)
        designTight    (cur[drive::Tightness]);
        designBite     (cur[drive::Bite]);
        designCascWarm (cur[drive::Casc]);
        designPresence (cur[drive::Presence]);
        designBody     (cur[drive::Body]);
        designHpf      (cur[drive::Low]);
        designFizz();
        designWarmth();
        for (auto* f : { &tightShelf, &biteShelf, &cascWarm, &defizz, &presenceF,
                         &bodyF, &warm1, &warm2, &hpf })
            f->reset();

        tameFast = tameSlow = 0.0f;
        tameFC = 1.0f - std::exp (-1.0f / (float) (sr * 0.002));   // 2 ms
        tameSC = 1.0f - std::exp (-1.0f / (float) (sr * 0.055));   // 55 ms
    }

    // Flush the audio state (filters/envelopes/oversampler) WITHOUT touching
    // the design — for a strip re-entering after being skipped: the chain
    // must not replay a stale tail from whenever it was last audible.
    void reset()
    {
        for (auto& f : gritBP) f.reset();
        for (auto* f : { &tightShelf, &biteShelf, &cascWarm, &defizz, &presenceF,
                         &bodyF, &warm1, &warm2, &hpf })
            f->reset();
        cleanLP.reset();
        if (os != nullptr) os->reset();
        peak = 0.0f;
        tameFast = tameSlow = 0.0f;
    }

    static float clip (float x, float dr, float as, float hd) noexcept
    {
        const float gg = x * dr;
        const float soft = gg >= 0.0f ? std::tanh (gg) : as * std::tanh (gg / std::max (as, 1.0e-3f));
        const float hc = juce::jlimit (-1.0f, 1.0f, gg);
        return (1.0f - hd) * soft + hd * hc;
    }

    // pre-cab: clean-low tap -> AGC normalise -> tight/bite pre-shape ->
    // oversampled drive -> per-band grit (+ casc warmth) -> restore level -> tame
    void processPreCab (float* x, int n)
    {
        for (int i = 0; i < n; ++i)
            cleanLow[(size_t) i] = cleanLP.processSample (0, x[i]);

        for (int i = 0; i < n; ++i)
        {
            const float a = std::abs (x[i]);
            peak += (a > peak ? atk : rel) * (a - peak);
            env[(size_t) i] = juce::jmax (peak, 0.03f);
            x[i] /= env[(size_t) i];                          // normalise toward unity
            // tightness: how hard the lows hit the clipper; bite: HF emphasis
            // into the clipper. Both are exact pass-throughs when neutral.
            x[i] = biteShelf.processSample (tightShelf.processSample (x[i]));
        }

        float* ch[1] = { x };
        juce::dsp::AudioBlock<float> blk (ch, 1, (size_t) n);
        auto up = os->processSamplesUp (blk);
        float* u = up.getChannelPointer (0);   // raw pointer: this loop runs at 4x rate
        const int upN = (int) up.getNumSamples();
        for (int s = 0; s < upN; ++s)
            u[s] = clip (u[s], driveG, asym, hard);
        os->processSamplesDown (blk);

        for (int i = 0; i < n; ++i)
        {
            const float d  = x[i];
            const float d2 = clip (d, 3.0f, asym, hard);       // grittier cascade
            const float diff = d2 - d;                          // extra harmonics to sprinkle per band
            float grit = 0.0f;
            for (size_t b = 0; b < 8; ++b)
                grit += gritScale * locks.grit[b] * gritBP[b].processSample (diff);
            // casc < 0 = warmth: tame the grit region. Exact pass-through at casc >= 0.
            // restore the note's dynamics (env^exp when reshaped; exp 1 = stock, bit-exact)
            const float restore = juce::exactlyEqual (restoreExp, 1.0f)
                                ? env[(size_t) i] : std::pow (env[(size_t) i], restoreExp);
            x[i] = cascWarm.processSample (d + grit) * restore;
        }

        if (tame > 0.0f)   // transient reduction, scale-invariant
        {
            for (int i = 0; i < n; ++i)
            {
                const float a = std::abs (x[i]);
                tameFast += tameFC * (a - tameFast);
                tameSlow += tameSC * (a - tameSlow);
                const float trans = juce::jmax (0.0f, tameFast - tameSlow);
                const float g = 1.0f - tame * trans / (tameFast + 1.0e-9f);
                x[i] *= juce::jlimit (0.0f, 1.0f, g);
            }
        }
    }

    // post-cab tone shaping + level trim
    void processPostCab (float* x, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            float v = defizz.processSample (x[i]);
            if (lowBlend > 0.0f) v += blendGain * cleanLow[(size_t) i];   // clean low end back in
            v = presenceF.processSample (v);   // exact pass-through at presence 0
            v = bodyF.processSample (v);
            v = warm1.processSample (v);
            v = warm2.processSample (v);
            if (lowBlend < 0.0f) v = hpf.processSample (v);
            x[i] = v * level;
        }
    }

private:
    using C = juce::dsp::IIR::Coefficients<float>;

    // Exact identity biquad for the always-in-graph stages at their neutral
    // value. A unity-gain shelf design is NOT bit-exact through JUCE's IIR:
    // assignImpl normalises with a multiply by 1/a0, so b0 becomes a0*(1/a0)
    // = 1 +/- 1 ulp. {1,0,0 / 1,0,0} normalises to exactly {1,0,0,0,0} and the
    // DF2T recursion then passes samples through bit-for-bit (state stays 0) —
    // that is the byte-identity contract for FUZZ.
    static C::Ptr identity() { return new C (1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f); }

    // tightness = linear gain on the lows feeding the clipper (240 Hz shelf).
    // 1.0 (the FUZZ value) = exact bypass; floored well above 0 so the shelf
    // stays finite.
    void designTight (float t)
    {
        tightShelf.coefficients = juce::exactlyEqual (t, 1.0f)
            ? identity() : C::makeLowShelf (sr, 240.0f, 0.7f, juce::jmax (t, 0.0316f));
    }

    // bite = HF drive: +3 dB per unit into the clipper above 1600 Hz
    void designBite (float b)
    {
        biteShelf.coefficients = b <= 0.0f
            ? identity() : C::makeHighShelf (sr, 1600.0f, 0.7f, juce::Decibels::decibelsToGain (3.0f * b));
    }

    // casc < 0 = warmth (high shelf at 1200 Hz, casc*4 dB); exact bypass at casc >= 0
    void designCascWarm (float c)
    {
        cascWarm.coefficients = c >= 0.0f
            ? identity() : C::makeHighShelf (sr, 1200.0f, 0.7f, juce::Decibels::decibelsToGain (4.0f * c));
    }

    // presence = post-cab HF lift; exact bypass at 0
    void designPresence (float p)
    {
        presenceF.coefficients = p <= 0.0f
            ? identity() : C::makeHighShelf (sr, 3500.0f, 0.7f, juce::Decibels::decibelsToGain (p));
    }

    void designBody (float b)
    { bodyF.coefficients = C::makePeakFilter (sr, 140.0f, 1.2f, juce::Decibels::decibelsToGain (b * 4.0f)); }

    void designHpf (float low)
    {
        const float hpHz = low < 0.0f ? 20.0f + (-low) * 35.0f : 20.0f;
        hpf.coefficients = C::makeHighPass (sr, juce::jlimit (20.0f, (float) sr * 0.45f, hpHz));
    }

    void designFizz()
    { defizz.coefficients = C::makeHighShelf (sr, 7000.0f, 0.7f, juce::Decibels::decibelsToGain (-13.0f * locks.fizz)); }

    void designWarmth()
    {
        warm1.coefficients = C::makePeakFilter (sr, 75.0f,  0.5f, juce::Decibels::decibelsToGain (locks.warmth * 4.0f));
        warm2.coefficients = C::makePeakFilter (sr, 205.0f, 1.3f, juce::Decibels::decibelsToGain (locks.warmth * 3.5f));
    }

    double sr = 0.0;

    // active state (scalars refreshed by setCharacter; cur tracks filter designs)
    drive::Params cur { { 1.0f, 1.0f, 0.0f, 0.2f, 0.7f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f } };
    drive::Locks  locks { 0.0f, 0.0f, 0.0f, { {} } };
    bool locksValid = false;
    float driveG = 64.0f, asym = 0.2f, hard = 0.7f, gritScale = 1.0f, tame = 0.0f;
    float lowBlend = 0.0f, blendGain = 0.0f;
    float level = 1.0f;

    float atkSec = 0.005f, relSec = 0.250f, restoreExp = 1.0f;
    float peak = 0.0f, atk = 0.0f, rel = 0.0f;
    float tameFast = 0.0f, tameSlow = 0.0f, tameFC = 0.0f, tameSC = 0.0f;
    juce::HeapBlock<float> env, cleanLow;
    std::unique_ptr<juce::dsp::Oversampling<float>> os;
    std::array<juce::dsp::IIR::Filter<float>, 8> gritBP;
    juce::dsp::IIR::Filter<float> tightShelf, biteShelf, cascWarm, defizz, presenceF, bodyF, warm1, warm2, hpf;
    juce::dsp::LinkwitzRileyFilter<float> cleanLP;
};
