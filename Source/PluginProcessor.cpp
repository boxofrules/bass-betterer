// Bass Better-er — © 2026 Box of Rules. Source-available, proprietary — see LICENSE.
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "BinaryData.h"

// Channel table — id (param prefix), display name, isFX (clean/fuzz switch),
// isRoom, default gain (dB), default mute.
const std::array<BoRBassEnhancerProcessor::ChanDef, BoRBassEnhancerProcessor::NUM_CH>
BoRBassEnhancerProcessor::channels = {{
    // v1.0 display names dropped the legacy "LOW" prefix (the premium engine's
    // HI layers never shipped here) and name the clean bands by speaker size.
    // The id (param prefix) is the compatibility surface — NEVER rename ids.
    { "sub",     "SUB",         false, false,   0.0f, false },
    { "lowcln1", "CLEAN 15\"",  false, false,  -2.0f, false },
    { "lowcln2", "CLEAN 12\"",  false, false,  -5.0f, false },
    // FX defaults sit |fuzz trim| above the old -4/-8/-14 so the shipped fuzz-on
    // tone is unchanged now the fuzz path is loudness-matched to clean (see below)
    { "lofx57",  "FX 57",       true,  false,   7.2f, false },
    { "lofx421", "FX 421",      true,  false,   8.1f, false },
    { "lofxtwt", "FX TWEETER",  true,  false,  -2.6f, false },
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

// FX channel (0..2) -> DIST cab IR: the DIST BLEND (v0.2.1, mirroring
// the premium "DIST 2" recipe exactly — one representative rig voice per
// mic, the six curated family captures averaged offline into one impulse;
// 57 / AT->421 / OX->TWEETER). The TYPE key swaps the drive-path cab between
// this and the H1 fuzz cab — the clipper params are unchanged by the swap.
static const char* distIrFor (int fx, int& size)
{
    switch (fx)
    {
        case 0: size = BinaryData::ir_lofx57_dist_wavSize;  return BinaryData::ir_lofx57_dist_wav;
        case 1: size = BinaryData::ir_lofx421_dist_wavSize; return BinaryData::ir_lofx421_dist_wav;
        case 2: size = BinaryData::ir_lofxtwt_dist_wavSize; return BinaryData::ir_lofxtwt_dist_wav;
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
        pDriveType[(size_t) c] = channels[(size_t) c].isFX ? P (id + "_drivetype") : nullptr;
    }

    // v0.2.0: the DIST character's fitted values ride the encrypted asset
    // pack (embedded like the IRs). A malformed blob falls back to FUZZ.
    drive::loadFits (BinaryData::drive_fits_bin, BinaryData::drive_fits_binSize);

    pInGain   = P ("in_gain");
    pOutGain  = P ("out_gain");
    pGlue     = P ("glue");
    pDriveRel     = P ("drive_rel");
    pDriveSustain = P ("drive_sustain");
    for (int r = 0; r < 2; ++r)
        for (int v = 0; v < 6; ++v)
            pRoomSrc[(size_t) r][(size_t) v] =
                P (juce::String (channels[(size_t) (6 + r)].id) + "_src_" + channels[(size_t) v].id);
    { int b = 0; for (auto* id : { "eq_low", "eq_lomid", "eq_himid", "eq_high" })
      {
          pEq[(size_t) b]     = P (id);
          pEqFreq[(size_t) b] = P (juce::String (id) + "_freq");
          ++b;
      } }
    pDriveAmt = P ("drive_amt");
    pWidth    = P ("width");
    pAnalyzer = P ("analyzer");
    pGuitarMode = P ("guitar_mode");
    apvts.addParameterListener ("guitar_mode", this);   // latency reporting (refreshLatency)
    pDiGain   = P ("di_gain");
    pDiMute   = P ("di_mute");
    pDiSolo   = P ("di_solo");
    pDiPhase  = P ("di_phase");
    pDiDuck   = P ("di_duck");

    // Spectrum display view mode ("freqView": all/pre/post) is a non-automatable
    // ValueTree property, not an APVTS parameter (see PluginEditor::freqView()),
    // so it has no parameter "default value" to declare — make the fresh-instance
    // default (ALL) explicit here instead of relying only on getProperty's fallback.
    // setStateInformation() replaces apvts.state wholesale for a restored session,
    // so this has no effect once a saved session/preset is loaded.
    apvts.state.setProperty ("freqView", "all", nullptr);
    apvts.state.setProperty ("uiMode", "expert", nullptr);   // SIMPLE/EXPERT view (see editor)
}

BoRBassEnhancerProcessor::~BoRBassEnhancerProcessor()
{
    apvts.removeParameterListener ("guitar_mode", this);
    cancelPendingUpdate();
}

void BoRBassEnhancerProcessor::refreshLatency()
{
    const int lat = (pGuitarMode != nullptr && pGuitarMode->load() > 0.5f)
                        ? shifter.latencySamples() : 0;
    if (lat != getLatencySamples())
        setLatencySamples (lat);
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
        // v0.2.0: sidechain ducking left the free plugin (premium feature).
        // The `_duck` params stay DECLARED for session/state compatibility —
        // the surface is append-only once shipped — but nothing reads them.
        layout.add (std::make_unique<AudioParameterBool> (ParameterID { id + "_duck", 1 }, nm + " Sidechain", false));
        // v0.2.0: FUZZ defaults OFF on the LO FX strips on a fresh instance/Init
        // preset (clean stack first, dirt is an opt-in choice) — saved sessions/
        // other factory presets that explicitly set _fuzz are unaffected;
        // migrateState()'s "missing node means engaged" fallback stays true since
        // it only interprets pre-stateVersion-2 XML.
        if (ch.isRoom)
        {
            // v1.0: per-room source select — which voicing layers feed this
            // room's mono sum. All-on (the default) is the classic full-stack
            // feed, bit-identical to pre-v1 renders.
            for (int v = 0; v < 6; ++v)
                layout.add (std::make_unique<AudioParameterBool> (
                    ParameterID { id + "_src_" + channels[(size_t) v].id, 1 },
                    nm + " Feed " + channels[(size_t) v].name, true));
        }
        if (ch.isFX)
        {
            layout.add (std::make_unique<AudioParameterBool> (ParameterID { id + "_fuzz", 1 }, nm + " Drive", false));
            // v0.2.0: the drive character — FUZZ (the original, byte-identical
            // to every earlier release) or DIST (tighter, brighter; the fitted
            // values ship in the encrypted asset pack). The choice list is
            // append-only from here.
            layout.add (std::make_unique<AudioParameterChoice> (
                ParameterID { id + "_drivetype", 1 }, nm + " Drive Type",
                StringArray { "FUZZ", "DIST" }, 0));
        }
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
    // v1.0: global drive-envelope dials, shared by every drive strip (the AGC
    // is a gain stage around the cab convolution, so no per-setting IRs).
    // RELEASE is the follower release: the stock 250 ms fell fast from each
    // pluck's attack peak to the body level — heard as a sidechain-style duck.
    // 2500 ms won the 2026-08-01 stem audition and is the new shipped tone;
    // migrateState() pins pre-v5 sessions back to the legacy 250 ms.
    // SUSTAIN restores env^(1 - s/200): upward compression of the strip's
    // dynamics (0 = stock envelope, bit-exact).
    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "drive_rel", 1 }, "Fuzz Release",
        NormalisableRange<float> (100.0f, 4000.0f, 1.0f, 0.4f), 2500.0f,
        AudioParameterFloatAttributes().withLabel ("ms")));
    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "drive_sustain", 1 }, "Fuzz Sustain",
        NormalisableRange<float> (0.0f, 100.0f, 1.0f), 0.0f,
        AudioParameterFloatAttributes().withLabel ("%")));
    // v1.0: global DRIVE dial — scales every strip's locked drive amount.
    // 100 % (the default) is an exact x1 (applied by /100 division), so the
    // locked characters reproduce bit-for-bit. The AGC keeps the level story
    // sane either side: this dial is how hard the clipper is hit, not volume.
    {
        NormalisableRange<float> dr (25.0f, 400.0f, 1.0f);
        dr.setSkewForCentre (100.0f);
        layout.add (std::make_unique<AudioParameterFloat> (
            ParameterID { "drive_amt", 1 }, "Fuzz Drive", dr, 100.0f,
            AudioParameterFloatAttributes().withLabel ("%")));
    }
    // v1.0: WIDTH — mid/side image scale between GLUE and OUTPUT, stereo
    // instances only (the editor hides it in mono, the DSP skips it). 100 %
    // is skipped entirely: the M/S decompose+recompose is not bit-exact even
    // at unity, so the neutral default must bypass, not multiply by 1.
    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "width", 1 }, "Width",
        NormalisableRange<float> (0.0f, 200.0f, 1.0f), 100.0f,
        AudioParameterFloatAttributes().withLabel ("%")));
    // v1.0: master EQ column (post glue, pre output gain). All dials at 0 =
    // the stage is skipped entirely (no CPU, byte-identical to pre-EQ renders).
    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "eq_low", 1 }, "EQ Low",
        NormalisableRange<float> (-12.0f, 12.0f, 0.1f), 0.0f, AudioParameterFloatAttributes().withLabel ("dB")));
    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "eq_lomid", 1 }, "EQ Low Mid",
        NormalisableRange<float> (-12.0f, 12.0f, 0.1f), 0.0f, AudioParameterFloatAttributes().withLabel ("dB")));
    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "eq_himid", 1 }, "EQ High Mid",
        NormalisableRange<float> (-12.0f, 12.0f, 0.1f), 0.0f, AudioParameterFloatAttributes().withLabel ("dB")));
    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "eq_high", 1 }, "EQ High",
        NormalisableRange<float> (-12.0f, 12.0f, 0.1f), 0.0f, AudioParameterFloatAttributes().withLabel ("dB")));
    // each band's frequency, as an append-only choice (defaults = the v1.0
    // fixed centres, so sessions saved before the choice existed are unmoved)
    {
        static const std::array<const char*, 4> bandIds  { "eq_low", "eq_lomid", "eq_himid", "eq_high" };
        static const std::array<const char*, 4> bandNames { "EQ Low", "EQ Low Mid", "EQ High Mid", "EQ High" };
        for (size_t b = 0; b < 4; ++b)
        {
            StringArray opts;
            for (float f : EQ_FREQS[b])
                opts.add (f < 1000.0f ? String ((int) f) + " Hz"
                                      : String (f / 1000.0f, 1) + " kHz");
            layout.add (std::make_unique<AudioParameterChoice> (
                ParameterID { String (bandIds[b]) + "_freq", 1 },
                String (bandNames[b]) + " Freq", opts, EQ_FREQ_DEFAULT[b]));
        }
    }
    // spectrum analyzer feed on/off (turn off to save CPU)
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "analyzer", 1 }, "Analyzer", true));
    // v1.0: GUITAR mode — the whole chain hears the input an octave down
    // (play a guitar, get a bass). Default off reproduces every earlier
    // render byte-identically. Excluded from preset load/save: presets are
    // tones, this is "what instrument is plugged in". Available in every
    // format; the Standalone shows a red lag notice while it is active.
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "guitar_mode", 1 }, "Guitar Mode", false));

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

        size = 0;
        if (auto* data = distIrFor (fx, size))
            distConvs[(size_t) fx].loadImpulseResponse (data, (size_t) size,
                juce::dsp::Convolution::Stereo::no, juce::dsp::Convolution::Trim::no, 0,
                juce::dsp::Convolution::Normalise::yes);
        distConvs[(size_t) fx].prepare (monoSpec);
    }

    // Drive chains (v0.2.0: the drive:: tables replace the old inline
    // configure constants — same numbers, same sound). The per-strip locks
    // carry the mic-chain shaping + the level trim that equalises drive
    // loudness to the clean voicing at the same fader (K-weighted match
    // measured with `tools/bor-bench cal`); the character is FUZZ (the locked
    // v1 constants, byte-identical to every earlier release — bor-bench fnv)
    // or DIST from the encrypted pack, selected per block in renderLayer.
    for (int slot = 0; slot < 3; ++slot)
    {
        const int c = slot + 3;
        const bool dist = pDriveType[(size_t) c] != nullptr && pDriveType[(size_t) c]->load() > 0.5f;
        auto lk = drive::LOCKS[(size_t) slot];   // + the character's measured loudness adjust
        lk.levelDb += dist ? drive::DIST_TRIM_DB[(size_t) slot] : drive::FUZZ_TRIM_DB[(size_t) slot];
        fuzz[(size_t) slot].setLocks (lk);
        fuzz[(size_t) slot].setCharacter (dist ? drive::DIST[(size_t) slot] : drive::FUZZ[(size_t) slot]);
    }
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
    roomFeed.allocate ((size_t) samplesPerBlock, true);

    // master EQ: stereo pair per band; sentinel forces a redesign at this rate
    for (auto& band : eqF)
        for (auto& f : band) { f.prepare (monoSpec); f.reset(); }
    eqCur.fill (1.0e9f);   // gains AND freq choices — both re-read on first use
    eqWasActive = false;

    // GUITAR mode: octave-down shifter + raw/shifted scratch. The engage
    // crossfade snaps to the saved state (no fade-in on session load).
    shifter.prepare (sampleRate, 0.5, false);
    rawIn.setSize    (1, samplesPerBlock);
    shiftBuf.setSize (1, samplesPerBlock);
    gmCoef = 1.0f - std::exp (-1.0f / (float) (sampleRate * 0.010));
    gmXf = pGuitarMode->load() > 0.5f ? 1.0f : 0.0f;
    gmWasActive = gmXf > 0.0f;

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

    // all convs above were just re-prepared, so no stale state to flush
    for (int fx = 0; fx < 3; ++fx)
        prevPath[(size_t) fx] = pFuzz[(size_t) (fx + 3)]->load() <= 0.5f ? DrivePath::clean
                              : pDriveType[(size_t) (fx + 3)]->load() > 0.5f ? DrivePath::distCab
                                                                             : DrivePath::fuzzCab;
    roomIdle.fill (false);

    // prepare is the canonical (message-thread-safe) latency report point;
    // runtime toggles go through the guitar_mode listener -> AsyncUpdater.
    refreshLatency();
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
    // GUITAR mode inserts the octave-down shifter here, ahead of everything:
    // the shifted signal becomes the plugin's "DI" (dry -> DI blend strip,
    // analyzer, and mono -> the stack), while rawIn keeps the true input for
    // the A/B reference. The else-branch is the original input stage,
    // untouched — with the mode off the shifter never runs (fully bypassed,
    // not run-and-discarded) and every earlier render stays byte-identical.
    const float inG = juce::Decibels::decibelsToGain (pInGain->load());
    if (smSnap) smInG = inG;
    auto* mono = monoIn.getWritePointer (0);
    auto* dry  = dryIn.getWritePointer (0);
    // GUITAR mode runs in every format, Standalone included (v1.0: it was
    // briefly plugin-only). There is no host to compensate the ~32 ms
    // tracking latency in the Standalone, so the editor shows a red
    // "unavoidable lag" notice there while the mode is active.
    const bool   gmOn     = pGuitarMode->load() > 0.5f;
    const float  gmTarget = gmOn ? 1.0f : 0.0f;
    const float* abRef    = dry;                 // what the A/B key auditions
    if (gmOn || gmXf > 1.0e-4f)                  // engaged, or still fading out
    {
        auto* raw = rawIn.getWritePointer (0);
        auto* shf = shiftBuf.getWritePointer (0);
        for (int s = 0; s < n; ++s)
        {
            float v = 0.0f;
            for (int ch = 0; ch < nIn; ++ch) v += buffer.getReadPointer (ch)[s];
            raw[s] = (nIn > 0 ? v / (float) nIn : 0.0f);
        }
        if (! gmWasActive) { shifter.reset(); gmWasActive = true; }
        shifter.processBlock (raw, shf, n);
        for (int s = 0; s < n; ++s)
        {
            gmXf += gmCoef * (gmTarget - gmXf);  // 10 ms one-pole, same law as abXf
            const float v = raw[s] + gmXf * (shf[s] - raw[s]);
            dry[s]  = v;
            mono[s] = v * (smInG + (inG - smInG) * (float) (s + 1) * rampInc);
        }
        abRef = rawIn.getReadPointer (0);        // A/B = the true (un-shifted) input
    }
    else
    {
        gmXf = 0.0f;
        gmWasActive = false;
        for (int s = 0; s < n; ++s)
        {
            float v = 0.0f;
            for (int ch = 0; ch < nIn; ++ch) v += buffer.getReadPointer (ch)[s];
            const float avg = (nIn > 0 ? v / (float) nIn : 0.0f);
            dry[s]  = avg;
            mono[s] = avg * (smInG + (inG - smInG) * (float) (s + 1) * rampInc);
        }
    }
    smInG = inG;

    // --- solo state (the DI strip participates) + per-strip active/gain ---
    bool anySolo = false;
    for (int c = 0; c < NUM_CH; ++c) if (pSolo[(size_t) c]->load() > 0.5f) anySolo = true;
    if (pDiSolo->load() > 0.5f) anySolo = true;

    // -60 dB is the bottom of the fader range: treat it as true silence (the
    // default decibelsToGain floor is -100, which leaves 0.001 of signal), so
    // floored strips hit the zero-gain early-outs below
    std::array<bool, NUM_CH>  active {};
    std::array<float, NUM_CH> gain {};
    for (int c = 0; c < NUM_CH; ++c)
    {
        const bool muted = pMute[(size_t) c]->load() > 0.5f;
        const bool solo  = pSolo[(size_t) c]->load() > 0.5f;
        active[(size_t) c] = ! muted && (! anySolo || solo);
        gain[(size_t) c]   = active[(size_t) c] ? juce::Decibels::decibelsToGain (pGain[(size_t) c]->load(), -60.0f) : 0.0f;
    }
    const bool  diActive = (pDiMute->load() <= 0.5f) && (! anySolo || pDiSolo->load() > 0.5f);
    const float diGain   = diActive ? juce::Decibels::decibelsToGain (pDiGain->load(), -60.0f) : 0.0f;

    // ---- PASS 1: render each layer (post fuzz/conv/phase) into layerBuf ----

    auto renderLayer = [&] (int c, const float* src)
    {
        const auto& def = channels[(size_t) c];
        auto* w = work.getWritePointer (0);
        juce::FloatVectorOperations::copy (w, src, n);

        const int  slot   = driveSlotFor (c);
        const bool fuzzOn = def.isFX && pFuzz[(size_t) c] != nullptr && pFuzz[(size_t) c]->load() > 0.5f;
        const bool dist   = fuzzOn && pDriveType[(size_t) c] != nullptr && pDriveType[(size_t) c]->load() > 0.5f;
        if (fuzzOn)
        {
            // v0.2.0: select this strip's drive character (FUZZ or DIST) —
            // cheap when unchanged, glides (no reset) when switched live.
            // Locks carry the character's measured loudness adjust so a
            // character switch holds level (bor-bench cal). v0.2.1: DIST also
            // swaps the drive-path cab to the DIST blend (below) —
            // the cab IS the character difference, same recipe as premium.
            auto lk = drive::LOCKS[(size_t) slot];
            lk.levelDb += dist ? drive::DIST_TRIM_DB[(size_t) slot] : drive::FUZZ_TRIM_DB[(size_t) slot];
            fuzz[(size_t) slot].setLocks (lk);
            // The drive dials are step-1 params, but a skewed range's 0..1
            // roundtrip (pow) can hand back e.g. 99.9999924 for "100" — round
            // to the legal grid BEFORE deriving factors, or the byte-identity
            // contract (100 % / legacy 250 ms = bit-exact) silently breaks.
            const float dAmt = std::round (pDriveAmt->load());
            const float dRel = std::round (pDriveRel->load());
            const float dSus = std::round (pDriveSustain->load());
            // global DRIVE: scale the locked amount (/100 so 100 % is exact x1)
            auto chr = dist ? drive::DIST[(size_t) slot] : drive::FUZZ[(size_t) slot];
            chr[drive::Amount] *= dAmt / 100.0f;
            fuzz[(size_t) slot].setCharacter (chr);
            // global envelope dials (no-op when unmoved). ms -> s by DIVISION:
            // 250/1000 and 2500/1000 are exact in float — *0.001f would land
            // one ulp off.
            fuzz[(size_t) slot].setEnvShape (0.005f, dRel / 1000.0f, 1.0f - dSus / 200.0f);
        }
        if (def.isFX)
        {
            // the conv we're switching to hasn't been fed since the last
            // path change (drive toggle or TYPE key) — flush its FIFOs or it
            // replays a stale tail
            const auto path = ! fuzzOn ? DrivePath::clean
                            : dist     ? DrivePath::distCab : DrivePath::fuzzCab;
            if (path != prevPath[(size_t) slot])
            {
                if      (path == DrivePath::clean)   convs[(size_t) c].reset();
                else if (path == DrivePath::fuzzCab) fuzzConvs[(size_t) slot].reset();
                else                                 distConvs[(size_t) slot].reset();
                prevPath[(size_t) slot] = path;
            }
        }
        // only this block's n samples — wrapping all of `work` (sized to the host max)
        // feeds stale samples back through the convolution when the host renders
        // shorter blocks (GarageBand/Logic live input)
        auto cb = juce::dsp::AudioBlock<float> (work).getSubBlock (0, (size_t) n);
        juce::dsp::ProcessContextReplacing<float> ctx (cb);
        if (fuzzOn)
        {
            fuzz[(size_t) slot].processPreCab (w, n);
            (dist ? distConvs[(size_t) slot] : fuzzConvs[(size_t) slot]).process (ctx);
            fuzz[(size_t) slot].processPostCab (w, n);
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
        renderLayer (c, mono);
    for (int c = 6; c < NUM_CH; ++c)
    {
        // a fully silent room (the default state) skips its convolution — the two
        // room IRs carry 24000-tap tails, the heaviest DSP in the plugin. mixStrip
        // early-outs on the same all-zero condition, so the stale layerBuf row is
        // never read; the conv is flushed when the room comes back in.
        auto& idle = roomIdle[(size_t) (c - 6)];
        if (gain[(size_t) c] == 0.0f && smLg[(size_t) c] == 0.0f && smRg[(size_t) c] == 0.0f)
        {
            idle = true;
            continue;
        }
        if (idle) { convs[(size_t) c].reset(); idle = false; }
        // v1.0: per-room source select — sum only the toggled voicing layers.
        // Same 0..5 add order as the old shared voicing sum, so the all-on
        // default is bit-identical to pre-v1 renders.
        juce::FloatVectorOperations::clear (roomFeed, n);
        for (int v = 0; v < 6; ++v)
            if (pRoomSrc[(size_t) (c - 6)][(size_t) v]->load() > 0.5f)
                juce::FloatVectorOperations::add (roomFeed, layerBuf.getReadPointer (v), n);
        renderLayer (c, roomFeed);
    }

    // (v0.2.0: the LO FX sidechain duck left the free plugin — premium
    // feature. The retired `_duck` params are declared-but-inert.)

    // ---- PASS 2: mix to stereo with per-strip pan + optional sidechain duck; publish meters --
    outBus.clear (0, 0, n);   // only this block's samples (the buffer is sized to the host max)
    outBus.clear (1, 0, n);
    auto* outL = outBus.getWritePointer (0);
    auto* outR = outBus.getWritePointer (1);

    // ramps from the previous block's effective L/R gains to this block's targets,
    // so gain/pan moves AND mute/solo cuts glide instead of stepping
    auto mixStrip = [&] (const float* lc, float g, float pan, float phase,
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
            const float v  = lc[s];
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
        mixStrip (layerBuf.getReadPointer (c), gain[(size_t) c], pan, 1.0f,
                  smLg[(size_t) c], smRg[(size_t) c], chLevel[(size_t) c]);
    }

    {
        // DI blend stays centred (its strip carries the A/B button instead of pan)
        const float phase = pDiPhase->load() > 0.5f ? -1.0f : 1.0f;
        mixStrip (dry, diGain, 0.0f, phase, smDiLg, smDiRg, diLevel);
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

    // --- WIDTH (stereo only): mid/side image scale, skipped at 100 % ---
    if (numOut >= 2)
    {
        const float w = std::round (pWidth->load()) / 100.0f;   // round: skew-roundtrip lesson
        if (smSnap) smWidth = w;
        if (! juce::exactlyEqual (w, 1.0f) || ! juce::exactlyEqual (smWidth, 1.0f))
        {
            float* L = outBus.getWritePointer (0);
            float* R = outBus.getWritePointer (1);
            for (int s2 = 0; s2 < n; ++s2)
            {
                const float ww = smWidth + (w - smWidth) * (float) (s2 + 1) * rampInc;
                const float m  = 0.5f * (L[s2] + R[s2]);
                const float sd = 0.5f * (L[s2] - R[s2]) * ww;
                L[s2] = m + sd;
                R[s2] = m - sd;
            }
        }
        smWidth = w;
    }

    // --- master EQ (post glue, pre output): four dials, skipped when neutral ---
    {
        bool active = false, moved = false;
        for (int b = 0; b < 4; ++b)
        {
            const float db = pEq[(size_t) b]->load();
            if (! juce::exactlyEqual (db, eqCur[(size_t) b])) moved = true;
            if (! juce::exactlyEqual (pEqFreq[(size_t) b]->load(), eqCur[(size_t) (4 + b)])) moved = true;
            if (std::abs (db) > 0.001f) active = true;
        }
        if (active)
        {
            if (moved)
            {
                using C = juce::dsp::IIR::Coefficients<float>;
                auto g   = [] (float db) { return juce::Decibels::decibelsToGain (db); };
                auto set = [this] (int b, C::Ptr c) { eqF[(size_t) b][0].coefficients = c;
                                                      eqF[(size_t) b][1].coefficients = c; };
                auto fq  = [this] (int b) {
                    const int i = juce::jlimit (0, 3, (int) eqCur[(size_t) (4 + b)]);
                    return juce::jmin (EQ_FREQS[(size_t) b][(size_t) i], (float) sr * 0.45f);
                };
                for (int b = 0; b < 4; ++b)
                {
                    eqCur[(size_t) b]       = pEq[(size_t) b]->load();
                    eqCur[(size_t) (4 + b)] = pEqFreq[(size_t) b]->load();
                }
                set (0, C::makeLowShelf   (sr, fq (0), 0.71f, g (eqCur[0])));
                set (1, C::makePeakFilter (sr, fq (1), 1.0f,  g (eqCur[1])));
                set (2, C::makePeakFilter (sr, fq (2), 0.9f,  g (eqCur[2])));
                set (3, C::makeHighShelf  (sr, fq (3), 0.71f, g (eqCur[3])));
            }
            eqWasActive = true;
            for (int ch = 0; ch < 2; ++ch)
            {
                float* d = outBus.getWritePointer (ch);
                for (auto& band : eqF)
                {
                    auto& f = band[(size_t) ch];
                    for (int s = 0; s < n; ++s) d[s] = f.processSample (d[s]);
                }
            }
        }
        else if (eqWasActive)   // dials returned to neutral: drop the stale state
        {
            for (auto& band : eqF) for (auto& f : band) f.reset();
            eqWasActive = false;
        }
    }

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
    // In GUITAR mode abRef is the true (un-shifted) input — the A/B key
    // answers "what did I plug in", not "what does the shifter feed the stack".
    // With the mode off, abRef IS dry (same pointer, unchanged behaviour).
    const float abTarget = abDi.load (std::memory_order_relaxed) ? 1.0f : 0.0f;
    if (abXf > 1.0e-4f || abTarget > 0.0f)
    {
        for (int s = 0; s < n; ++s)
        {
            abXf += abCoef * (abTarget - abXf);
            for (int ch = 0; ch < juce::jmin (numOut, 2); ++ch)
            {
                auto* o = buffer.getWritePointer (ch);
                o[s] += abXf * (abRef[s] - o[s]);
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
        xml->setAttribute ("stateVersion", 5);
        copyXmlToBinary (*xml, destData);
    }
}

// stateVersion < 2 predates the fuzz loudness match (the fuzz path now sits
// 11-16 dB lower). Bump the gain of strips that were saved with fuzz engaged
// so old sessions keep their mix.
static void migrateState (juce::XmlElement& xml)
{
    if (xml.getIntAttribute ("stateVersion", 1) < 2)
    {
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
    // stateVersion 3 (v0.2.0) added the since-removed octave strips and the
    // per-strip drive type. No value migration is needed: every new parameter's
    // default reproduces the old behaviour exactly (octave strips muted, LO drive type FUZZ), so a v2
    // session loads bit-identically with the new params at their defaults.
    // stateVersion 4 (v1.0) adds guitar_mode. Default false reproduces a v3
    // session byte-identically — no value migration.
    // stateVersion 5 (v1.0) adds the global drive-envelope dials, whose
    // defaults are the NEW tone (release 2500 ms). A pre-v5 session gets an
    // explicit legacy 250 ms node so its render is unchanged.
    if (xml.getIntAttribute ("stateVersion", 1) < 5)
    {
        auto* p = xml.createNewChildElement ("PARAM");
        p->setAttribute ("id", "drive_rel");
        p->setAttribute ("value", 250.0);
        xml.setAttribute ("stateVersion", 5);
    }
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
// GUITAR mode is excluded from the preset surface entirely (presets are tones,
// the mode is "what instrument is plugged in"): the factory reset skips it,
// user presets neither save it nor apply it. Sessions still persist it.
static void stripGuitarMode (juce::XmlElement& xml)
{
    for (auto* p : xml.getChildWithTagNameIterator ("PARAM"))
        if (p->getStringAttribute ("id") == "guitar_mode")
        {
            xml.removeChildElement (p, true);
            return;
        }
}
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
                          {"lowcln1_gain",-4.0f},{"lowcln2_gain",-9.0f} } },
        { "Subby", { {"glue",0.2f},
                     {"lofx57_mute",1.0f},{"lofx421_mute",1.0f},{"lofxtwt_mute",1.0f},
                     {"sub_gain",2.0f},{"lowcln1_gain",-1.0f},{"lowcln2_gain",-5.0f} } },
        { "Clean Stack", { {"glue",0.25f},
                           {"lofx57_fuzz",0.0f},{"lofx421_fuzz",0.0f},{"lofxtwt_fuzz",0.0f},
                           {"lofx57_gain",-6.0f},{"lofx421_gain",-9.0f},{"lofxtwt_gain",-15.0f},
                           {"roomnear_mute",0.0f},{"roomnear_gain",-26.0f} } },
        // v0.2.0: was "Dirt Duck" — the sidechain duck left the free plugin,
        // and a preset must not be named for a move it no longer makes. The
        // lows are tucked by fader instead.
        { "Dirt Wall", { {"in_gain",4.0f},{"glue",0.4f},
                         {"lofx57_fuzz",1.0f},{"lofx421_fuzz",1.0f},{"lofxtwt_fuzz",1.0f},
                         {"sub_gain",-3.0f},{"lowcln1_gain",-5.0f},{"lowcln2_gain",-8.0f} } },
        // v0.2.0: the DIST character across the FX strips — tighter and
        // brighter than Dirt Wall's fuzz, the low bed pulled back so the mids
        // own the drive. (v0.2.1: Crunch Air left with the octave layers —
        // they are premium now.)
        { "Dist Stack", { {"in_gain",3.0f},{"glue",0.35f},
                          {"lofx57_fuzz",1.0f},{"lofx421_fuzz",1.0f},{"lofxtwt_fuzz",1.0f},
                          {"lofx57_drivetype",1.0f},{"lofx421_drivetype",1.0f},{"lofxtwt_drivetype",1.0f},
                          {"lowcln1_gain",-4.0f},{"lowcln2_gain",-7.0f} } },
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
            if (rp->paramID != "guitar_mode")   // presets are tones; GUITAR mode is what's plugged in
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
        xml->setAttribute ("stateVersion", 5);
        stripGuitarMode (*xml);
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
            stripGuitarMode (*xml);   // a shared/hand-edited preset must not flip the octave
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
