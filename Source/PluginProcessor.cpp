#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "BinaryData.h"

// Channel table — id (param prefix), display name, isFX (clean/fuzz switch),
// isRoom, default gain (dB), default mute.
const std::array<BoRBassEnhancerProcessor::ChanDef, BoRBassEnhancerProcessor::NUM_CH>
BoRBassEnhancerProcessor::channels = {{
    { "sub",     "SUB",                false, false,   0.0f, false },
    { "lowcln1", "LOW CLEAN BAND 1",   false, false,  -2.0f, false },
    { "lowcln2", "LOW CLEAN BAND 2",   false, false,  -5.0f, false },
    // FX defaults sit |fuzz trim| above the old -4/-8/-14 so the shipped fuzz-on
    // tone is unchanged now the fuzz path is loudness-matched to clean (see below)
    { "lofx57",  "LOW FX 57",          true,  false,   7.2f, false },
    { "lofx421", "LOW FX 421",         true,  false,   8.1f, false },
    { "lofxtwt", "LOW FX TWEETER",     true,  false,  -2.6f, false },
    // Rooms ship unmuted but at the fader floor (-60 dB = silent in the sum), so
    // bringing a room in is one fader move, not unmute-then-raise. Default tone is
    // unchanged. (Producer feedback: "1 click to use them, rather than 2.")
    { "roomnear","ROOM NEAR",          false, true,  -60.0f, false },
    { "roomfar", "ROOM FAR",           false, true,  -60.0f, false },
}};

// channel -> embedded clean/room voicing IR
static const char* irForChannel (int i, int& size)
{
    switch (i)
    {
        case 0: size = BinaryData::ir_sub_wavSize;          return BinaryData::ir_sub_wav;
        case 1: size = BinaryData::ir_lowcln1_wavSize;      return BinaryData::ir_lowcln1_wav;
        case 2: size = BinaryData::ir_lowcln2_wavSize;      return BinaryData::ir_lowcln2_wav;
        case 3: size = BinaryData::ir_lofx57_clean_wavSize; return BinaryData::ir_lofx57_clean_wav;
        case 4: size = BinaryData::ir_lofx421_clean_wavSize;return BinaryData::ir_lofx421_clean_wav;
        case 5: size = BinaryData::ir_lofxtwt_clean_wavSize;return BinaryData::ir_lofxtwt_clean_wav;
        case 6: size = BinaryData::ir_roomnear_wavSize;     return BinaryData::ir_roomnear_wav;
        case 7: size = BinaryData::ir_roomfar_wavSize;      return BinaryData::ir_roomfar_wav;
        default: size = 0; return nullptr;
    }
}

// FX channel (0..2) -> fuzz cab IR (FuzzChain does the drive/grit around it)
static const char* fuzzIrFor (int fx, int& size)
{
    switch (fx)
    {
        case 0: size = BinaryData::ir_lofx57_fuzz_wavSize;  return BinaryData::ir_lofx57_fuzz_wav;
        case 1: size = BinaryData::ir_lofx421_fuzz_wavSize; return BinaryData::ir_lofx421_fuzz_wav;
        case 2: size = BinaryData::ir_lofxtwt_fuzz_wavSize; return BinaryData::ir_lofxtwt_fuzz_wav;
        default: size = 0; return nullptr;
    }
}

BoRBassEnhancerProcessor::BoRBassEnhancerProcessor()
    : AudioProcessor (BusesProperties()
        .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
        .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMS", createLayout())
{
    auto P = [this](const juce::String& id){ return apvts.getRawParameterValue(id); };
    for (int c = 0; c < NUM_CH; ++c)
    {
        juce::String id (channels[(size_t) c].id);
        pGain [(size_t) c] = P (id + "_gain");
        pMute [(size_t) c] = P (id + "_mute");
        pSolo [(size_t) c] = P (id + "_solo");
        pPan  [(size_t) c] = P (id + "_pan");
        pPhase[(size_t) c] = P (id + "_phase");
        pDuck [(size_t) c] = P (id + "_duck");
        pFuzz [(size_t) c] = channels[(size_t) c].isFX ? P (id + "_fuzz") : nullptr;
    }
    pInGain   = P ("in_gain");
    pOutGain  = P ("out_gain");
    pGlue     = P ("glue");
    pAnalyzer = P ("analyzer");
    pDiGain   = P ("di_gain");
    pDiMute   = P ("di_mute");
    pDiSolo   = P ("di_solo");
    pDiPhase  = P ("di_phase");
    pDiDuck   = P ("di_duck");
}

juce::AudioProcessorValueTreeState::ParameterLayout BoRBassEnhancerProcessor::createLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;
    auto pct  = [](float v){ return String (v, 0); };

    for (const auto& ch : channels)
    {
        String id (ch.id), nm (ch.name);
        layout.add (std::make_unique<AudioParameterFloat> (
            ParameterID { id + "_gain", 1 }, nm + " Gain",
            NormalisableRange<float> (-60.0f, 12.0f, 0.1f), ch.defGainDb,
            AudioParameterFloatAttributes().withLabel ("dB")));
        layout.add (std::make_unique<AudioParameterBool> (ParameterID { id + "_mute", 1 }, nm + " Mute", ch.defMute));
        layout.add (std::make_unique<AudioParameterBool> (ParameterID { id + "_solo", 1 }, nm + " Solo", false));
        layout.add (std::make_unique<AudioParameterFloat> (
            ParameterID { id + "_pan", 1 }, nm + " Pan",
            NormalisableRange<float> (-1.0f, 1.0f, 0.01f), 0.0f));
        layout.add (std::make_unique<AudioParameterBool> (ParameterID { id + "_phase", 1 }, nm + " Phase", false));
        // SC = duck this layer from the LO FX (dirt) sidechain key
        layout.add (std::make_unique<AudioParameterBool> (ParameterID { id + "_duck", 1 }, nm + " Sidechain", false));
        if (ch.isFX)
            layout.add (std::make_unique<AudioParameterBool> (ParameterID { id + "_fuzz", 1 }, nm + " Fuzz", true));
    }
    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "in_gain", 1 }, "Input Gain",
        NormalisableRange<float> (-24.0f, 24.0f, 0.1f), 0.0f, AudioParameterFloatAttributes().withLabel ("dB")));
    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "glue", 1 }, "Glue",
        NormalisableRange<float> (0.0f, 1.0f, 0.01f), 0.0f));
    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "out_gain", 1 }, "Output Gain",
        NormalisableRange<float> (-24.0f, 24.0f, 0.1f), 0.0f, AudioParameterFloatAttributes().withLabel ("dB")));
    // spectrum analyzer feed on/off (turn off to save CPU)
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "analyzer", 1 }, "Analyzer", true));

    // DI blend strip — the original DI tone, blended in. Muted by default.
    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "di_gain", 1 }, "DI Gain",
        NormalisableRange<float> (-60.0f, 12.0f, 0.1f), 0.0f, AudioParameterFloatAttributes().withLabel ("dB")));
    layout.add (std::make_unique<AudioParameterBool>  (ParameterID { "di_mute", 1 }, "DI Mute", true));
    layout.add (std::make_unique<AudioParameterBool>  (ParameterID { "di_solo", 1 }, "DI Solo", false));
    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "di_pan", 1 }, "DI Pan", NormalisableRange<float> (-1.0f, 1.0f, 0.01f), 0.0f));
    layout.add (std::make_unique<AudioParameterBool>  (ParameterID { "di_phase", 1 }, "DI Phase", false));
    layout.add (std::make_unique<AudioParameterBool>  (ParameterID { "di_duck",  1 }, "DI Sidechain", false));

    ignoreUnused (pct);
    return layout;
}

void BoRBassEnhancerProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    sr = sampleRate;
    numOut = getTotalNumOutputChannels();

    juce::dsp::ProcessSpec monoSpec { sampleRate, (juce::uint32) samplesPerBlock, 1 };

    for (int c = 0; c < NUM_CH; ++c)
    {
        int size = 0;
        if (auto* data = irForChannel (c, size))
            convs[(size_t) c].loadImpulseResponse (data, (size_t) size,
                juce::dsp::Convolution::Stereo::no,
                juce::dsp::Convolution::Trim::no, 0,
                juce::dsp::Convolution::Normalise::yes);
        convs[(size_t) c].prepare (monoSpec);
    }

    for (int fx = 0; fx < 3; ++fx)
    {
        int size = 0;
        if (auto* data = fuzzIrFor (fx, size))
            fuzzConvs[(size_t) fx].loadImpulseResponse (data, (size_t) size,
                juce::dsp::Convolution::Stereo::no, juce::dsp::Convolution::Trim::no, 0,
                juce::dsp::Convolution::Normalise::yes);
        fuzzConvs[(size_t) fx].prepare (monoSpec);
    }

    // LO FX fuzz chains — per-channel locked settings (drive,asym,hard,fizz,
    // body,warmth,low, level trim, 8 grit bands). The level trim equalises the
    // fuzz loudness to the clean voicing at the same fader, so toggling FUZZ
    // doesn't jump (K-weighted match measured with `tools/bor-bench cal`).
    fuzz[0].configure (150.f, 0.2f, 0.7f, 0.8f, 2.8f, 0.65f, -0.2f, -11.2f, { {0.9f,0.25f,0.85f,0.8f,0.4f,0.45f,0.8f,0.25f} });
    fuzz[1].configure ( 84.f, 0.2f, 0.7f, 0.8f, 4.0f, 1.0f,  -0.2f, -16.1f, { {0.65f,0.25f,0.85f,0.15f,0.0f,0.0f,0.2f,0.4f} });
    fuzz[2].configure ( 97.f, 0.2f, 0.7f, 1.0f, 2.8f, 0.65f, -1.2f, -11.4f, { {0.9f,0.25f,0.85f,0.8f,0.4f,0.35f,0.0f,0.0f} });
    for (auto& fz : fuzz) fz.prepare (sampleRate, samplesPerBlock);
    fuzzOsLat.store (fuzz[0].oversamplingLatency(), std::memory_order_relaxed);

    juce::dsp::ProcessSpec stereoSpec { sampleRate, (juce::uint32) samplesPerBlock, 2 };
    glueComp.prepare (stereoSpec);
    glueComp.setAttack (12.0f);
    glueComp.setRelease (160.0f);

    monoIn.setSize (1, samplesPerBlock);
    dryIn.setSize  (1, samplesPerBlock);
    work.setSize   (1, samplesPerBlock);
    outBus.setSize (2, samplesPerBlock);
    layerBuf.setSize (NUM_CH, samplesPerBlock);
    keyEnv.allocate ((size_t) samplesPerBlock, true);
    voiceMono.allocate ((size_t) samplesPerBlock, true);

    // sidechain follower: 5 ms attack, 140 ms release
    scEnv = 0.0f;
    scAtk = 1.0f - std::exp (-1.0f / (float) (sampleRate * 0.005));
    scRel = 1.0f - std::exp (-1.0f / (float) (sampleRate * 0.140));

    for (int w = 0; w < 2; ++w)
    {
        analyzerBuf[(size_t) w].setSize (1, analyzerFifo[(size_t) w].getTotalSize());
        analyzerBuf[(size_t) w].clear();
        analyzerFifo[(size_t) w].reset();
    }

    // DI reference A/B crossfade (~10 ms) + load measurement for the SYS panel
    abXf = 0.0f;
    abCoef = 1.0f - std::exp (-1.0f / (float) (sampleRate * 0.010));
    loadMeasurer.reset (sampleRate, samplesPerBlock);
    smSnap = true;   // gain ramps snap to targets on the first block
}

// ---- spectrum analyzer SPSC fifos -------------------------------------------
void BoRBassEnhancerProcessor::writeAnalyzer (int which, const float* mono, int n)
{
    auto& fifo = analyzerFifo[(size_t) which];
    const int toWrite = juce::jmin (fifo.getFreeSpace(), n);
    if (toWrite <= 0) return;
    int s1, sz1, s2, sz2;
    fifo.prepareToWrite (toWrite, s1, sz1, s2, sz2);
    auto* buf = analyzerBuf[(size_t) which].getWritePointer (0);
    if (sz1 > 0) juce::FloatVectorOperations::copy (buf + s1, mono, sz1);
    if (sz2 > 0) juce::FloatVectorOperations::copy (buf + s2, mono + sz1, sz2);
    fifo.finishedWrite (toWrite);
}

int BoRBassEnhancerProcessor::readAnalyzer (int which, float* dest, int maxSamples)
{
    auto& fifo = analyzerFifo[(size_t) which];
    const int toRead = juce::jmin (fifo.getNumReady(), maxSamples);
    if (toRead <= 0) return 0;
    int s1, sz1, s2, sz2;
    fifo.prepareToRead (toRead, s1, sz1, s2, sz2);
    const auto* buf = analyzerBuf[(size_t) which].getReadPointer (0);
    if (sz1 > 0) juce::FloatVectorOperations::copy (dest, buf + s1, sz1);
    if (sz2 > 0) juce::FloatVectorOperations::copy (dest + sz1, buf + s2, sz2);
    fifo.finishedRead (toRead);
    return toRead;
}

bool BoRBassEnhancerProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    const auto in  = layouts.getMainInputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;
    if (in != juce::AudioChannelSet::mono() && in != juce::AudioChannelSet::stereo())
        return false;
    return true;
}

void BoRBassEnhancerProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const int nIn = getTotalNumInputChannels();
    const int n   = buffer.getNumSamples();
    numOut = getTotalNumOutputChannels();
    juce::AudioProcessLoadMeasurer::ScopedTimer cpuTimer (loadMeasurer, n);
    lastBlock.store (n, std::memory_order_relaxed);

    // All mix gains (strip gain/mute/solo/pan, DI, INPUT, OUTPUT) ramp linearly
    // across the block from the previous block's value — otherwise moving a
    // control while playing steps the gain once per block (audible zipper noise).
    const float rampInc = 1.0f / (float) n;

    // --- input: dry DI (pre-gain, for the DI blend) + mono with input-gain (pushes fuzz) ---
    const float inG = juce::Decibels::decibelsToGain (pInGain->load());
    if (smSnap) smInG = inG;
    auto* mono = monoIn.getWritePointer (0);
    auto* dry  = dryIn.getWritePointer (0);
    for (int s = 0; s < n; ++s)
    {
        float v = 0.0f;
        for (int ch = 0; ch < nIn; ++ch) v += buffer.getReadPointer (ch)[s];
        const float avg = (nIn > 0 ? v / (float) nIn : 0.0f);
        dry[s]  = avg;
        mono[s] = avg * (smInG + (inG - smInG) * (float) (s + 1) * rampInc);
    }
    smInG = inG;

    // --- solo state (the DI strip participates) + per-strip active/gain ---
    bool anySolo = false;
    for (int c = 0; c < NUM_CH; ++c) if (pSolo[(size_t) c]->load() > 0.5f) anySolo = true;
    if (pDiSolo->load() > 0.5f) anySolo = true;

    std::array<bool, NUM_CH>  active {};
    std::array<float, NUM_CH> gain {};
    for (int c = 0; c < NUM_CH; ++c)
    {
        const bool muted = pMute[(size_t) c]->load() > 0.5f;
        const bool solo  = pSolo[(size_t) c]->load() > 0.5f;
        active[(size_t) c] = ! muted && (! anySolo || solo);
        gain[(size_t) c]   = active[(size_t) c] ? juce::Decibels::decibelsToGain (pGain[(size_t) c]->load()) : 0.0f;
    }
    const bool  diActive = (pDiMute->load() <= 0.5f) && (! anySolo || pDiSolo->load() > 0.5f);
    const float diGain   = diActive ? juce::Decibels::decibelsToGain (pDiGain->load()) : 0.0f;

    // ---- PASS 1: render each layer (post fuzz/conv/phase) into layerBuf; build room feed ----
    juce::FloatVectorOperations::clear (voiceMono, n);

    auto renderLayer = [&] (int c, const float* src)
    {
        const auto& def = channels[(size_t) c];
        auto* w = work.getWritePointer (0);
        juce::FloatVectorOperations::copy (w, src, n);

        const bool fuzzOn = def.isFX && pFuzz[(size_t) c] != nullptr && pFuzz[(size_t) c]->load() > 0.5f;
        // only this block's n samples — wrapping all of `work` (sized to the host max)
        // feeds stale samples back through the convolution when the host renders
        // shorter blocks (GarageBand/Logic live input)
        auto cb = juce::dsp::AudioBlock<float> (work).getSubBlock (0, (size_t) n);
        juce::dsp::ProcessContextReplacing<float> ctx (cb);
        if (fuzzOn)
        {
            fuzz[(size_t) (c - 3)].processPreCab (w, n);
            fuzzConvs[(size_t) (c - 3)].process (ctx);
            fuzz[(size_t) (c - 3)].processPostCab (w, n);
        }
        else
        {
            convs[(size_t) c].process (ctx);   // dialed v2 voicing baked into this IR
        }
        if (pPhase[(size_t) c]->load() > 0.5f)
            juce::FloatVectorOperations::multiply (w, -1.0f, n);

        juce::FloatVectorOperations::copy (layerBuf.getWritePointer (c), w, n);
    };

    for (int c = 0; c < 6; ++c)
    {
        renderLayer (c, mono);
        const float* lc = layerBuf.getReadPointer (c);
        for (int s = 0; s < n; ++s) voiceMono[(size_t) s] += lc[s];   // rooms hear the voicing sum
    }
    for (int c = 6; c < NUM_CH; ++c) renderLayer (c, voiceMono.getData());

    // ---- sidechain key = LO FX (3,4,5) post-gain sum -> follower -> per-sample duck gain ----
    constexpr float SC_THRESH = -32.0f, SC_AMOUNT = 0.7f, SC_MAXRED = 12.0f;
    for (int s = 0; s < n; ++s)
    {
        float k = 0.0f;
        for (int c = 3; c <= 5; ++c) k += layerBuf.getReadPointer (c)[s] * gain[(size_t) c];
        k = std::abs (k);
        scEnv += (k > scEnv ? scAtk : scRel) * (k - scEnv);
        const float kdb  = juce::Decibels::gainToDecibels (scEnv, -100.0f);
        const float over = juce::jmax (0.0f, kdb - SC_THRESH);
        const float red  = juce::jmin (SC_MAXRED, over * SC_AMOUNT);
        keyEnv[(size_t) s] = juce::Decibels::decibelsToGain (-red);   // 0..1 duck multiplier
    }

    // ---- PASS 2: mix to stereo with per-strip pan + optional sidechain duck; publish meters --
    outBus.clear();
    auto* outL = outBus.getWritePointer (0);
    auto* outR = outBus.getWritePointer (1);

    // ramps from the previous block's effective L/R gains to this block's targets,
    // so gain/pan moves AND mute/solo cuts glide instead of stepping
    auto mixStrip = [&] (const float* lc, float g, bool duckOn, float pan, float phase,
                         float& prevLg, float& prevRg, std::atomic<float>& levelOut)
    {
        const float ang = (pan * 0.5f + 0.5f) * juce::MathConstants<float>::halfPi;
        const float tLg = std::cos (ang) * g * phase, tRg = std::sin (ang) * g * phase;
        if (smSnap) { prevLg = tLg; prevRg = tRg; }
        if (tLg == 0.0f && tRg == 0.0f && prevLg == 0.0f && prevRg == 0.0f)
        { levelOut.store (0.0f, std::memory_order_relaxed); return; }

        float pk = 0.0f;
        for (int s = 0; s < n; ++s)
        {
            const float r  = (float) (s + 1) * rampInc;
            const float lg = prevLg + (tLg - prevLg) * r;
            const float rg = prevRg + (tRg - prevRg) * r;
            const float v  = lc[s] * (duckOn ? keyEnv[(size_t) s] : 1.0f);
            outL[s] += v * lg;
            outR[s] += v * rg;
            pk = juce::jmax (pk, std::abs (v));
        }
        prevLg = tLg; prevRg = tRg;
        levelOut.store (pk * std::abs (g), std::memory_order_relaxed);
    };

    for (int c = 0; c < NUM_CH; ++c)
    {
        // inactive strips still mix with target gain 0 so the cut ramps out
        // SUB (c == 0) is dead centre by design — its pan param is vestigial
        const float pan = (isStereo() && c != 0) ? pPan[(size_t) c]->load() : 0.0f;
        mixStrip (layerBuf.getReadPointer (c), gain[(size_t) c],
                  pDuck[(size_t) c]->load() > 0.5f, pan, 1.0f,
                  smLg[(size_t) c], smRg[(size_t) c], chLevel[(size_t) c]);
    }

    {
        // DI blend stays centred (its strip carries the A/B button instead of pan)
        const float phase = pDiPhase->load() > 0.5f ? -1.0f : 1.0f;
        mixStrip (dry, diGain, pDiDuck->load() > 0.5f, 0.0f, phase, smDiLg, smDiRg, diLevel);
    }

    // --- glue: compress the sum (threshold/ratio scale with the knob) ---
    const float glue = pGlue->load();
    if (glue > 0.0001f)
    {
        glueComp.setThreshold (-3.0f - glue * 21.0f);   // 0..1 -> -3..-24 dB
        glueComp.setRatio (1.0f + glue * 5.0f);          // 1..6 :1
        auto gb = juce::dsp::AudioBlock<float> (outBus).getSubBlock (0, (size_t) n);
        juce::dsp::ProcessContextReplacing<float> gctx (gb);
        glueComp.process (gctx);
    }
    // makeup ramps (and ramps back to unity when glue disengages) — knob moves
    // would otherwise step the gain once per block
    const float makeup = glue > 0.0001f ? juce::Decibels::decibelsToGain (glue * 4.0f) : 1.0f;
    if (smSnap) smMakeup = makeup;
    if (smMakeup != 1.0f || makeup != 1.0f)
        outBus.applyGainRamp (0, n, smMakeup, makeup);
    smMakeup = makeup;

    // --- output gain (ramped) + write to the host buffer (mono or stereo) ---
    const float outG = juce::Decibels::decibelsToGain (pOutGain->load());
    if (smSnap) smOutG = outG;
    if (numOut >= 2)
    {
        for (int s = 0; s < n; ++s)
        {
            const float og = smOutG + (outG - smOutG) * (float) (s + 1) * rampInc;
            buffer.getWritePointer (0)[s] = outL[s] * og;
            buffer.getWritePointer (1)[s] = outR[s] * og;
        }
        for (int ch = 2; ch < numOut; ++ch) buffer.clear (ch, 0, n);
    }
    else if (numOut == 1)
    {
        for (int s = 0; s < n; ++s)
        {
            const float og = smOutG + (outG - smOutG) * (float) (s + 1) * rampInc;
            buffer.getWritePointer (0)[s] = (outL[s] + outR[s]) * 0.5f * og;
        }
    }
    smOutG = outG;
    smSnap = false;

    // --- DI reference A/B: crossfade the host buffer toward the raw DI ---
    const float abTarget = abDi.load (std::memory_order_relaxed) ? 1.0f : 0.0f;
    if (abXf > 1.0e-4f || abTarget > 0.0f)
    {
        for (int s = 0; s < n; ++s)
        {
            abXf += abCoef * (abTarget - abXf);
            for (int ch = 0; ch < juce::jmin (numOut, 2); ++ch)
            {
                auto* o = buffer.getWritePointer (ch);
                o[s] += abXf * (dry[s] - o[s]);
            }
        }
    }
    else abXf = 0.0f;

    // --- spectrum analyzer feeds: what you hear (post, incl. A/B) + the raw DI ---
    if (analyzerEnabled())
    {
        auto* a = work.getWritePointer (0);
        if (numOut >= 2)
            for (int s = 0; s < n; ++s) a[s] = (buffer.getReadPointer (0)[s] + buffer.getReadPointer (1)[s]) * 0.5f;
        else
            juce::FloatVectorOperations::copy (a, buffer.getReadPointer (0), n);
        writeAnalyzer (0, a, n);
        writeAnalyzer (1, dry, n);
    }
}

void BoRBassEnhancerProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
    {
        xml->setAttribute ("stateVersion", 2);
        copyXmlToBinary (*xml, destData);
    }
}

// stateVersion < 2 predates the fuzz loudness match (the fuzz path now sits
// 11-16 dB lower). Bump the gain of strips that were saved with fuzz engaged
// so old sessions keep their mix.
static void migrateState (juce::XmlElement& xml)
{
    if (xml.getIntAttribute ("stateVersion", 1) >= 2) return;
    struct Fix { const char* id; float bumpDb; };
    for (const auto& fx : { Fix { "lofx57", 11.2f }, Fix { "lofx421", 16.1f }, Fix { "lofxtwt", 11.4f } })
    {
        bool fuzzOn = true;   // _fuzz defaults to on, so a missing node means engaged
        juce::XmlElement* gainNode = nullptr;
        for (auto* p : xml.getChildWithTagNameIterator ("PARAM"))
        {
            const auto pid = p->getStringAttribute ("id");
            if (pid == juce::String (fx.id) + "_fuzz") fuzzOn = p->getDoubleAttribute ("value", 1.0) > 0.5;
            if (pid == juce::String (fx.id) + "_gain") gainNode = p;
        }
        if (fuzzOn && gainNode != nullptr)
            gainNode->setAttribute ("value", juce::jlimit (-60.0, 12.0,
                gainNode->getDoubleAttribute ("value") + (double) fx.bumpDb));
    }
    xml.setAttribute ("stateVersion", 2);
}

void BoRBassEnhancerProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
        {
            migrateState (*xml);
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
        }
}

// ---- presets ---------------------------------------------------------------
// Factory presets are param overrides (plain values) applied on top of a reset.
namespace {
using PV = std::pair<juce::String, float>;
const std::vector<std::pair<juce::String, std::vector<PV>>>& factoryPresets()
{
    static const std::vector<std::pair<juce::String, std::vector<PV>>> p = {
        { "Init", {} },   // factory defaults
        // FX gains sit |fuzz trim| above the pre-0.1.4 values (0/-3/-8): same tone,
        // loudness-matched fuzz path. 421 wants +13.1 but the range caps at +12.
        { "Hysterical", { {"in_gain",8.0f}, {"glue",0.55f},
                          {"lofx57_fuzz",1.0f},{"lofx421_fuzz",1.0f},{"lofxtwt_fuzz",1.0f},
                          {"lofx57_gain",11.2f},{"lofx421_gain",12.0f},{"lofxtwt_gain",3.4f},
                          {"lowcln1_gain",-4.0f},{"lowcln2_gain",-9.0f},
                          {"sub_duck",1.0f},{"lowcln1_duck",1.0f} } },
        { "Subby", { {"glue",0.2f},
                     {"lofx57_mute",1.0f},{"lofx421_mute",1.0f},{"lofxtwt_mute",1.0f},
                     {"sub_gain",2.0f},{"lowcln1_gain",-1.0f},{"lowcln2_gain",-5.0f} } },
        { "Clean Stack", { {"glue",0.25f},
                           {"lofx57_fuzz",0.0f},{"lofx421_fuzz",0.0f},{"lofxtwt_fuzz",0.0f},
                           {"lofx57_gain",-6.0f},{"lofx421_gain",-9.0f},{"lofxtwt_gain",-15.0f},
                           {"roomnear_mute",0.0f},{"roomnear_gain",-26.0f} } },
        { "Dirt Duck", { {"in_gain",4.0f},{"glue",0.4f},
                         {"lofx57_fuzz",1.0f},{"lofx421_fuzz",1.0f},{"lofxtwt_fuzz",1.0f},
                         {"sub_duck",1.0f},{"lowcln1_duck",1.0f},{"lowcln2_duck",1.0f} } },
    };
    return p;
}
}

juce::StringArray BoRBassEnhancerProcessor::getFactoryPresetNames() const
{
    juce::StringArray a;
    for (auto& pr : factoryPresets()) a.add (pr.first);
    return a;
}

void BoRBassEnhancerProcessor::loadFactoryPreset (int index)
{
    auto& list = factoryPresets();
    if (index < 0 || index >= (int) list.size()) return;
    for (auto* p : getParameters())
        if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (p))
            rp->setValueNotifyingHost (rp->getDefaultValue());
    for (auto& kv : list[(size_t) index].second)
        if (auto* p = apvts.getParameter (kv.first))
            p->setValueNotifyingHost (p->convertTo0to1 (kv.second));
}

juce::File BoRBassEnhancerProcessor::getUserPresetDir() const
{
    auto d = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                 .getChildFile ("Box of Rules").getChildFile ("Bass Better-er").getChildFile ("Presets");
    d.createDirectory();
    return d;
}

bool BoRBassEnhancerProcessor::saveUserPreset (const juce::String& name)
{
    auto f = getUserPresetDir().getChildFile (juce::File::createLegalFileName (name) + ".xml");
    if (auto xml = apvts.copyState().createXml())
    {
        xml->setAttribute ("stateVersion", 2);
        return xml->writeTo (f);
    }
    return false;
}

bool BoRBassEnhancerProcessor::loadUserPresetFile (const juce::File& f)
{
    if (auto xml = juce::XmlDocument::parse (f))
        if (xml->hasTagName (apvts.state.getType()))
        {
            migrateState (*xml);
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
            return true;
        }
    return false;
}

juce::AudioProcessorEditor* BoRBassEnhancerProcessor::createEditor()
{
    return new BoRBassEnhancerEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new BoRBassEnhancerProcessor();
}
