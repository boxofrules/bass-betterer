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
    // v0.2.0: 10 strips — the original 8 plus the two HI layers (HI CRUNCH /
    // HI AIR): DI -> one shared octave-up shifter -> per-strip drive -> HI
    // cab IR. Both ship MUTED, so a fresh instance (and every old session)
    // renders byte-identically to v0.1.x and stays zero-latency (bor-bench
    // fnv holds); latency is reported only while a HI strip is unmuted.
    static constexpr int NUM_CH   = 10;
    static constexpr int FIRST_HI = 8;
    static constexpr int NUM_HI   = 2;
    struct ChanDef { const char* id; const char* name; bool isFX; bool isRoom; bool isHI; float defGainDb; bool defMute; };
    static const std::array<ChanDef, NUM_CH> channels;

    // channel -> drive chain slot (drive:: table order: LO FX 57/421/TWT,
    // HI CRUNCH, HI AIR; -1 = not drive-capable)
    static constexpr int driveSlotFor (int c) noexcept
    {
        if (c >= 3 && c <= 5) return c - 3;
        if (c >= FIRST_HI && c < FIRST_HI + NUM_HI) return 3 + (c - FIRST_HI);
        return -1;
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
    juce::File getUserPresetDir() const;
    bool saveUserPreset (const juce::String& name);
    bool loadUserPresetFile (const juce::File&);

    juce::AudioProcessorValueTreeState apvts;

private:
    juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

    double sr = 48000.0;
    int numOut = 2;

    std::array<juce::dsp::Convolution, NUM_CH> convs;      // clean/room/HI voicing IR per channel
    std::array<juce::dsp::Convolution, 3> fuzzConvs;       // H1 cab IR for the LO FX drive mode
    std::array<FuzzChain, 5> fuzz;  // drive chains (driveSlotFor: LO FX x3 + HI x2)
    juce::dsp::Compressor<float> glueComp;

    // v0.2.0 HI layers: ONE shared octave-up shifter feeds both HI strips
    // (runs only while an audible HI strip consumes it; reset on re-entry so
    // it never replays a stale ring). hiShift = its per-block output.
    OctaveShifter shifter;
    juce::HeapBlock<float> hiShift;
    bool shifterIdle = true;
    std::array<bool, NUM_HI> hiIdle {};

    // latency policy: report the shifter latency to the host only while a HI
    // strip is unmuted (mute flips arrive via the APVTS listener; the actual
    // setLatencySamples happens on the message thread via the AsyncUpdater)
    void parameterChanged (const juce::String&, float) override { triggerAsyncUpdate(); }
    void handleAsyncUpdate() override { refreshLatency(); }
    void refreshLatency();

    // cached parameter pointers (lock-free reads on the audio thread)
    std::array<std::atomic<float>*, NUM_CH> pGain{}, pMute{}, pSolo{}, pPan{}, pFuzz{}, pPhase{}, pDuck{};
    std::array<std::atomic<float>*, NUM_CH> pDriveType{};   // v0.2.0: FUZZ/DIST per drive strip
    std::atomic<float>* pInGain  = nullptr;
    std::atomic<float>* pOutGain = nullptr;
    std::atomic<float>* pGlue    = nullptr;
    std::atomic<float>* pAnalyzer = nullptr;  // spectrum display feed on/off (CPU saver)
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

    // two-pass render buffers (needed so the LO FX-keyed ducking can pre-compute the key)
    juce::AudioBuffer<float> monoIn, dryIn, work, outBus; // dryIn = original DI; outBus 2-ch
    juce::AudioBuffer<float> layerBuf;                     // NUM_CH mono layers (post fuzz/conv)
    juce::HeapBlock<float>   keyEnv;                       // per-sample LO FX sidechain envelope
    juce::HeapBlock<float>   voiceMono;                    // voicing sum that feeds the rooms
    float scEnv = 0.0f, scAtk = 0.0f, scRel = 0.0f;       // sidechain follower state/coeffs

    // spectrum analyzer fifos (mono -> editor FFT): [0] processed output, [1] raw DI
    std::array<juce::AbstractFifo, 2> analyzerFifo { juce::AbstractFifo { 1 << 14 },
                                                     juce::AbstractFifo { 1 << 14 } };
    std::array<juce::AudioBuffer<float>, 2> analyzerBuf;
    void writeAnalyzer (int which, const float* mono, int n);

    // DI reference A/B (see setDiReference) — smoothed so toggling never clicks
    std::atomic<bool> abDi { false };
    float abXf = 0.0f, abCoef = 0.0f;

    // block-rate gain ramps (zipper-noise fix): previous block's effective values,
    // snapped (not ramped) on the first block after prepare
    std::array<float, NUM_CH> smLg {}, smRg {};
    float smDiLg = 0.0f, smDiRg = 0.0f, smInG = 1.0f, smOutG = 1.0f, smMakeup = 1.0f;
    bool  smSnap = true;

    // FX clean/fuzz convs share one strip: the idle one holds stale FIFO state,
    // so it is reset when the FUZZ toggle switches back to it (LO FX only —
    // the HI strips run ONE IR for both clean and driven paths)
    std::array<bool, 3> prevFuzzOn {};
    // rooms skip their (heaviest) convolution while fully silent; reset on re-entry
    std::array<bool, 2> roomIdle {};

    juce::AudioProcessLoadMeasurer loadMeasurer;   // CPU % for the SYS info panel
    std::atomic<int> lastBlock { 0 };
    std::atomic<float> fuzzOsLat { 0.0f };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BoRBassEnhancerProcessor)
};
