#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include <array>
#include "FuzzChain.h"

// Bass Better-er — drop on a bass DI channel; splits it into the BoR multi-mic
// voicings (each a measured H1 cab IR), mixes them with per-channel level/mute/solo/
// pan, optional fuzz on the LO FX channels, glue compression on the sum, and I/O trim.
class BoRBassEnhancerProcessor : public juce::AudioProcessor
{
public:
    static constexpr int NUM_CH = 8;
    struct ChanDef { const char* id; const char* name; bool isFX; bool isRoom; float defGainDb; bool defMute; };
    static const std::array<ChanDef, NUM_CH> channels;

    BoRBassEnhancerProcessor();
    ~BoRBassEnhancerProcessor() override = default;

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
    double getTailLengthSeconds() const override { return 0.4; }
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

    std::array<juce::dsp::Convolution, NUM_CH> convs;      // clean/room voicing IR per channel
    std::array<juce::dsp::Convolution, 3> fuzzConvs;       // H1 cab IR for FX fuzz mode
    std::array<FuzzChain, 3> fuzz;  // the 3 LO FX channels (idx maps c-3)
    juce::dsp::Compressor<float> glueComp;

    // cached parameter pointers (lock-free reads on the audio thread)
    std::array<std::atomic<float>*, NUM_CH> pGain{}, pMute{}, pSolo{}, pPan{}, pFuzz{}, pPhase{}, pDuck{};
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

    juce::AudioProcessLoadMeasurer loadMeasurer;   // CPU % for the SYS info panel
    std::atomic<int> lastBlock { 0 };
    std::atomic<float> fuzzOsLat { 0.0f };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BoRBassEnhancerProcessor)
};
