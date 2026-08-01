// Bass Better-er — © 2026 Box of Rules. Source-available, proprietary — see LICENSE.
#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include <array>
#include "FuzzChain.h"
#include "OctaveShifter.h"

// Bass Better-er — drop on a bass DI channel; splits it into the BoR multi-mic
// voicings (each a measured H1 cab IR), mixes them with per-channel level/mute/solo/
// pan, optional fuzz on the LO FX channels, glue compression on the sum, and I/O trim.
class BoRBassEnhancerProcessor : public juce::AudioProcessor,
                                 private juce::AudioProcessorValueTreeState::Listener,
                                 private juce::AsyncUpdater
{
public:
    // v0.2.1: back to the 8 v0.1.x strips. The v0.2.0 octave-up layers
    // moved to the premium plugin only: the granular shifter's
    // inherent 16-64 ms lag + an un-ear-locked drive placeholder made them
    // unshippable here (they were also the only latency reporter). v0.2.0
    // sessions load fine; their hioct_*/hih_* nodes are simply ignored.
    // v1.0: the shifter returns as the opt-in GUITAR mode (octave DOWN this
    // time — causal, and the only latency reporter: zero-latency with the
    // mode off).
    static constexpr int NUM_CH = 8;
    struct ChanDef { const char* id; const char* name; bool isFX; bool isRoom; float defGainDb; bool defMute; };
    static const std::array<ChanDef, NUM_CH> channels;

    // channel -> drive chain slot (drive:: table order: LO FX 57/421/TWT;
    // -1 = not drive-capable)
    static constexpr int driveSlotFor (int c) noexcept
    {
        return (c >= 3 && c <= 5) ? c - 3 : -1;
    }

    BoRBassEnhancerProcessor();
    ~BoRBassEnhancerProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Bass Better-er"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    // must cover the room IRs' 24000-tap reverb tail (0.54 s at 44.1 kHz) or
    // hosts that trust this value truncate the room decay on bounce/freeze
    double getTailLengthSeconds() const override { return 0.6; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    bool isStereo() const noexcept { return numOut >= 2; }

    // Per-channel post-gain peak level for the UI meters (lock-free, written each block).
    float getChannelLevel (int c) const noexcept
    { return (c >= 0 && c < NUM_CH) ? chLevel[(size_t) c].load (std::memory_order_relaxed) : 0.0f; }

    // Dry DI-blend strip level (the original DI tone, blended in; muted by default).
    float getDiLevel() const noexcept { return diLevel.load (std::memory_order_relaxed); }

    // Spectrum analyzer feeds (lock-free SPSC). The editor pulls samples to FFT.
    // which = 0: processed output (what you hear), 1: the raw DI input.
    bool analyzerEnabled() const noexcept { return pAnalyzer != nullptr && pAnalyzer->load() > 0.5f; }
    int  readAnalyzer (int which, float* dest, int maxSamples);   // message thread

    // A/B audition: true = pass the raw DI through (click-free crossfade) so the
    // processed stack can be compared against the untouched input. Deliberately not
    // an APVTS parameter — it is a listening tool and must never persist in a session.
    void setDiReference (bool b) noexcept { abDi.store (b, std::memory_order_relaxed); }
    bool getDiReference() const noexcept  { return abDi.load (std::memory_order_relaxed); }

    // Live performance stats for the SYS info panel (message thread reads).
    double getCpuLoad() const noexcept      { return loadMeasurer.getLoadAsProportion(); }
    int    getXRunCount() const noexcept    { return loadMeasurer.getXRunCount(); }
    int    getLastBlockSize() const noexcept{ return lastBlock.load (std::memory_order_relaxed); }
    // cached at prepare time: reading the live Oversampling object from the UI
    // thread would race prepareToPlay re-creating it
    float  getFuzzOsLatency() const noexcept{ return fuzzOsLat.load (std::memory_order_relaxed); }

    // Presets (message thread). Per-project state already persists via get/setStateInformation;
    // these add portable named configs: a handful of factory presets + user save/recall to disk.
    juce::StringArray getFactoryPresetNames() const;
    void loadFactoryPreset (int index);
    juce::StringArray getArtistPresetNames() const;   // "Box Of Rules (Artist)" menu section
    void loadArtistPreset (int index);
    juce::File getUserPresetDir() const;
    bool saveUserPreset (const juce::String& name);
    bool loadUserPresetFile (const juce::File&);

    juce::AudioProcessorValueTreeState apvts;

private:
    juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

    // GUITAR mode latency reporting: the shifter is the only latency source,
    // reported only while engaged. setLatencySamples must not be called on the
    // audio thread, so the param listener bounces through an AsyncUpdater.
    void parameterChanged (const juce::String&, float) override { triggerAsyncUpdate(); }
    void handleAsyncUpdate() override { refreshLatency(); }
    void refreshLatency();

    double sr = 48000.0;
    int numOut = 2;

    std::array<juce::dsp::Convolution, NUM_CH> convs;      // clean/room voicing IR per channel
    std::array<juce::dsp::Convolution, 3> fuzzConvs;       // H1 cab IR for the FUZZ drive path
    std::array<juce::dsp::Convolution, 3> distConvs;       // blend cab for the DIST drive path
    std::array<FuzzChain, 3> fuzz;  // drive chains (driveSlotFor: LO FX x3)
    juce::dsp::Compressor<float> glueComp;

    // cached parameter pointers (lock-free reads on the audio thread)
    std::array<std::atomic<float>*, NUM_CH> pGain{}, pMute{}, pSolo{}, pPan{}, pFuzz{}, pPhase{}, pDuck{};
    std::array<std::atomic<float>*, NUM_CH> pDriveType{};   // v0.2.0: FUZZ/DIST per drive strip
    std::array<std::atomic<float>*, NUM_CH> pPad{};         // v1.0: per-strip headroom pad
    std::atomic<float>* pDiPad = nullptr;
    std::atomic<float>* pInGain  = nullptr;
    std::atomic<float>* pOutGain = nullptr;
    std::atomic<float>* pGlue    = nullptr;
    std::atomic<float>* pDriveRel     = nullptr;  // global drive-envelope dials
    std::atomic<float>* pDriveSustain = nullptr;
    std::atomic<float>* pAnalyzer = nullptr;  // spectrum display feed on/off (CPU saver)
    std::atomic<float>* pGuitarMode = nullptr; // octave-down front end (play a guitar, get a bass)
    std::atomic<float>* pDiGain  = nullptr;    // DI blend strip (raw input, pre input-gain)
    std::atomic<float>* pDiMute  = nullptr;
    std::atomic<float>* pDiSolo  = nullptr;
    std::atomic<float>* pDiPhase = nullptr;
    std::atomic<float>* pDiDuck  = nullptr;
    // di_pan / sub_pan still exist as parameters (state compatibility) but the DSP
    // keeps both strips dead centre, so neither is cached or read.

    // UI meter feed — per-channel post-gain peak, published once per block.
    std::array<std::atomic<float>, NUM_CH> chLevel {};
    std::atomic<float> diLevel { 0.0f };

    // two-pass render buffers
    juce::AudioBuffer<float> monoIn, dryIn, work, outBus; // dryIn = original DI; outBus 2-ch
    juce::AudioBuffer<float> layerBuf;                     // NUM_CH mono layers (post fuzz/conv)
    juce::HeapBlock<float>   roomFeed;                     // per-room sum of the selected voicing layers
    std::array<std::array<std::atomic<float>*, 6>, 2> pRoomSrc {};   // [room][voicing] feed toggles

    // master EQ (post glue): 4 bands x stereo, skipped when every dial is neutral.
    // Each band's centre/corner is a 4-way choice param (`eq_*_freq`); this
    // table is the single numeric source — createLayout() builds the choice
    // strings from it, and the editor reads names via getCurrentChoiceName().
    static constexpr std::array<std::array<float, 4>, 4> EQ_FREQS {{
        {   45.0f,   60.0f,   90.0f,  120.0f },   // LOW shelf   (default 90)
        {  180.0f,  250.0f,  350.0f,  500.0f },   // LO MID peak (default 250)
        {  700.0f,  900.0f, 1200.0f, 1800.0f },   // HI MID peak (default 900)
        { 2400.0f, 3200.0f, 4800.0f, 6800.0f },   // HIGH shelf  (default 3200)
    }};
    static constexpr std::array<int, 4> EQ_FREQ_DEFAULT { 2, 1, 1, 1 };
    std::array<std::atomic<float>*, 4> pEq {}, pEqFreq {};
    std::array<std::array<juce::dsp::IIR::Filter<float>, 2>, 4> eqF;
    std::array<float, 8> eqCur {};   // 4 gains + 4 freq choices; sentinel = redesign
    bool eqWasActive = false;

    std::atomic<float>* pDriveAmt = nullptr;   // global DRIVE dial (% of the locked amounts)
    std::atomic<float>* pWidth    = nullptr;   // WIDTH dial (spread amount, 0 = off)
    std::atomic<float>* pFxSat    = nullptr;   // SAT dial (combined FX-bus saturation)
    float smFxSat = 0.0f;
    juce::AudioBuffer<float> fxBus;            // FX strips' sub-bus while SAT is engaged
    float fxSatEnv = 0.0f, fxSatAtkC = 0.0f, fxSatRelC = 0.0f;   // shaper AGC follower
    float smWidth = 0.0f;
    juce::HeapBlock<float> spreadBuf;          // spreader: 6 ms mid delay + side low-cut
    int   spreadLen = 1, spreadW = 0;
    float spreadLp = 0.0f, spreadLpC = 0.0f;

    // spectrum analyzer fifos (mono -> editor FFT): [0] processed output, [1] raw DI
    std::array<juce::AbstractFifo, 2> analyzerFifo { juce::AbstractFifo { 1 << 14 },
                                                     juce::AbstractFifo { 1 << 14 } };
    std::array<juce::AudioBuffer<float>, 2> analyzerBuf;
    void writeAnalyzer (int which, const float* mono, int n);

    // DI reference A/B (see setDiReference) — smoothed so toggling never clicks
    std::atomic<bool> abDi { false };
    float abXf = 0.0f, abCoef = 0.0f;

    // GUITAR mode: octave-down granular shifter ahead of the whole chain.
    // rawIn keeps the un-shifted input for the A/B reference; shiftBuf is the
    // shifter output; gmXf crossfades engage/disengage (same 10 ms law as abXf).
    // The shifter is fully bypassed (not run-and-discarded) when off — the
    // byte-identity contract for every existing render depends on that.
    OctaveShifter shifter;
    juce::AudioBuffer<float> rawIn, shiftBuf;
    float gmXf = 0.0f, gmCoef = 0.0f;
    bool  gmWasActive = false;

    // block-rate gain ramps (zipper-noise fix): previous block's effective values,
    // snapped (not ramped) on the first block after prepare
    std::array<float, NUM_CH> smLg {}, smRg {};
    float smDiLg = 0.0f, smDiRg = 0.0f, smInG = 1.0f, smOutG = 1.0f, smMakeup = 1.0f;
    bool  smSnap = true;

    // An FX strip's three convs (clean voicing / FUZZ H1 cab / DIST
    // blend) share one signal path: the idle ones hold stale FIFO state, so
    // the conv being switched TO is reset on any drive-path change (drive
    // toggle OR the TYPE key while engaged).
    enum class DrivePath : int { clean = 0, fuzzCab, distCab };
    std::array<DrivePath, 3> prevPath { DrivePath::clean, DrivePath::clean, DrivePath::clean };
    // rooms skip their (heaviest) convolution while fully silent; reset on re-entry
    std::array<bool, 2> roomIdle {};

    juce::AudioProcessLoadMeasurer loadMeasurer;   // CPU % for the SYS info panel
    std::atomic<int> lastBlock { 0 };
    std::atomic<float> fuzzOsLat { 0.0f };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BoRBassEnhancerProcessor)
};
