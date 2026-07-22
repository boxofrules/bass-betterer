// bor-bench — offline calibration + benchmark harness for Bass Better-er.
// © 2026 Box of Rules. Source-available, proprietary — see LICENSE.
// Developer tool, never shipped: build with -DBOR_BUILD_TOOLS=ON.
//
//   bor-bench cal     measure fuzz-vs-clean loudness per LO FX strip (K-weighted,
//                     BS.1770 pre-filter at 48 kHz) -> the FuzzChain level trims
//   bor-bench bench   DSP throughput per config, reported as x-realtime at 48k/512
//
// The test signal is a synthetic bass DI: plucked E1/A1/D2/G2 notes with a
// harmonic stack and decay envelope, peaking around -10 dBFS.

#include "../Source/PluginProcessor.h"
#include <vector>
#include <cstdio>
#include <cstdlib>

// ---- BS.1770 K-weighting (pre-filter + RLB high-pass), 48 kHz coefficients ----
struct KWeight
{
    // ITU-R BS.1770-4 stage 1 (high shelf) and stage 2 (RLB high-pass), fs = 48 kHz
    double s1z1 = 0, s1z2 = 0, s2z1 = 0, s2z2 = 0;
    double sumSq = 0; long count = 0;

    void push (float xIn)
    {
        const double x = xIn;
        // stage 1: shelf
        const double b0 = 1.53512485958697, b1 = -2.69169618940638, b2 = 1.19839281085285;
        const double a1 = -1.69065929318241, a2 = 0.73248077421585;
        const double w1 = x - a1 * s1z1 - a2 * s1z2;
        const double y1 = b0 * w1 + b1 * s1z1 + b2 * s1z2;
        s1z2 = s1z1; s1z1 = w1;
        // stage 2: RLB high-pass
        const double c1 = -1.99004745483398, c2 = 0.99007225036621;
        const double w2 = y1 - c1 * s2z1 - c2 * s2z2;
        const double y2 = w2 - 2.0 * s2z1 + s2z2;
        s2z2 = s2z1; s2z1 = w2;
        sumSq += y2 * y2; ++count;
    }
    double loudnessDb() const
    { return count > 0 ? 10.0 * std::log10 (sumSq / (double) count + 1.0e-20) : -120.0; }
};

// ---- synthetic bass DI ------------------------------------------------------
static std::vector<float> synthBassDI (double sr, double seconds)
{
    const double notes[] = { 41.2, 55.0, 73.4, 98.0 };   // E1 A1 D2 G2
    const double noteLen = 1.0;
    std::vector<float> s ((size_t) (sr * seconds), 0.0f);

    for (size_t i = 0; i < s.size(); ++i)
    {
        const double t = (double) i / sr;
        const double f0 = notes[(size_t) (t / noteLen) % 4];
        const double tn = std::fmod (t, noteLen);
        const double env = std::exp (-tn * 3.0) * (1.0 - std::exp (-tn * 400.0)); // pluck
        double v = 0.0;
        for (int h = 1; h <= 10; ++h)        // sawish stack, HF decays faster
            v += std::sin (2.0 * juce::MathConstants<double>::pi * f0 * h * t)
                 * std::pow (1.0 / h, 1.3) * std::exp (-tn * 0.6 * h);
        s[i] = (float) (v * env);
    }
    float pk = 0.0f;
    for (float v : s) pk = std::max (pk, std::abs (v));
    const float norm = juce::Decibels::decibelsToGain (-10.0f) / std::max (pk, 1.0e-9f);
    for (float& v : s) v *= norm;
    return s;
}

// ---- processor helpers ------------------------------------------------------
static std::unique_ptr<BoRBassEnhancerProcessor> makeProc (double sr, int block)
{
    auto p = std::make_unique<BoRBassEnhancerProcessor>();
    p->setPlayConfigDetails (2, 2, sr, block);
    p->prepareToPlay (sr, block);
    return p;
}

static void setParam (BoRBassEnhancerProcessor& p, const juce::String& id, float plainValue)
{
    auto* par = p.apvts.getParameter (id);
    if (par == nullptr)   // a stale id would otherwise silently mis-calibrate the trims
    {
        std::fprintf (stderr, "bor-bench: unknown parameter '%s'\n", id.toRawUTF8());
        std::exit (2);
    }
    par->setValueNotifyingHost (par->convertTo0to1 (plainValue));
}

// Render the signal through the processor; K-weighted loudness of the mono sum,
// skipping the first `skipSeconds` so AGC/convolver state settles.
static double renderLoudness (BoRBassEnhancerProcessor& p, const std::vector<float>& sig,
                              double sr, int block, double skipSeconds)
{
    juce::AudioBuffer<float> buf (2, block);
    juce::MidiBuffer midi;
    KWeight kw;
    const size_t skip = (size_t) (sr * skipSeconds);

    for (size_t pos = 0; pos < sig.size(); pos += (size_t) block)
    {
        const int n = (int) std::min ((size_t) block, sig.size() - pos);
        buf.setSize (2, n, false, false, true);
        for (int ch = 0; ch < 2; ++ch)
            buf.copyFrom (ch, 0, sig.data() + pos, n);
        p.processBlock (buf, midi);
        for (int i = 0; i < n; ++i)
            if (pos + (size_t) i >= skip)
                kw.push (0.5f * (buf.getSample (0, i) + buf.getSample (1, i)));
    }
    return kw.loudnessDb();
}

// ---- modes ------------------------------------------------------------------
static int runCal()
{
    const double sr = 48000.0; const int block = 512;
    const auto sig = synthBassDI (sr, 8.0);
    struct StripDef { const char* id; bool hi; };
    const StripDef strips[] = { { "lofx57", false }, { "lofx421", false }, { "lofxtwt", false },
                                { "hioct", true }, { "hih", true } };

    // v0.2.0: three renders per drive strip — clean, FUZZ, DIST — soloed at
    // gain 0. The baked levelDb trims equalise FUZZ to clean; the printed
    // DIST adjust is the extra per-character trim (drive::DIST_TRIM_DB) that
    // brings DIST level with clean through the SAME locks. HI strips: "clean"
    // is the shifted signal through the cab IR with drive off.
    std::printf ("drive vs clean loudness per strip (K-weighted, soloed, gain 0 dB)\n");
    for (const auto& sdef : strips)
    {
        const juce::String id (sdef.id);
        double lDb[3] = {};
        for (int mode = 0; mode < 3; ++mode)   // 0 clean, 1 FUZZ, 2 DIST
        {
            auto p = makeProc (sr, block);
            setParam (*p, "analyzer", 0.0f);
            setParam (*p, id + "_solo", 1.0f);
            setParam (*p, id + "_gain", 0.0f);
            if (sdef.hi) setParam (*p, id + "_mute", 0.0f);
            setParam (*p, id + "_fuzz", mode > 0 ? 1.0f : 0.0f);
            setParam (*p, id + "_drivetype", mode == 2 ? 1.0f : 0.0f);
            lDb[mode] = renderLoudness (*p, sig, sr, block, 1.0);
        }
        std::printf ("  %-8s  clean %7.2f   FUZZ %7.2f (d %+6.2f)   DIST %7.2f (d %+6.2f)  -> DIST adjust %+6.2f dB\n",
                     sdef.id, lDb[0], lDb[1], lDb[1] - lDb[0], lDb[2], lDb[2] - lDb[0], lDb[0] - lDb[2]);
    }
    return 0;
}

static int runBench()
{
    const double sr = 48000.0; const int block = 512;
    const double seconds = 60.0;
    const auto sig = synthBassDI (sr, seconds);

    struct Config { const char* name; bool fuzz; bool analyzer; bool rooms; };
    const Config configs[] = {
        { "default (3x fuzz, analyzer on)        ", true,  true,  false },
        { "default, analyzer off                 ", true,  false, false },
        { "all clean (fuzz off), analyzer off    ", false, false, false },
        { "everything on (fuzz + rooms + analyzer)", true,  true,  true  },
    };

    std::printf ("DSP throughput, %.0fs of 48 kHz audio, %d-sample blocks (single thread)\n",
                 seconds, block);
    for (const auto& c : configs)
    {
        auto p = makeProc (sr, block);
        setParam (*p, "analyzer", c.analyzer ? 1.0f : 0.0f);
        for (auto* id : { "lofx57", "lofx421", "lofxtwt" })
            setParam (*p, juce::String (id) + "_fuzz", c.fuzz ? 1.0f : 0.0f);
        if (c.rooms)
            for (auto* id : { "roomnear", "roomfar" })
            { setParam (*p, juce::String (id) + "_mute", 0.0f); setParam (*p, juce::String (id) + "_gain", -20.0f); }

        juce::AudioBuffer<float> buf (2, block);
        juce::MidiBuffer midi;
        float drain[1 << 12];

        const auto t0 = juce::Time::getHighResolutionTicks();
        for (size_t pos = 0; pos + (size_t) block <= sig.size(); pos += (size_t) block)
        {
            for (int ch = 0; ch < 2; ++ch)
                buf.copyFrom (ch, 0, sig.data() + pos, block);
            p->processBlock (buf, midi);
            if (c.analyzer)   // the editor would drain the fifos; do the same so writes aren't free
                for (int w = 0; w < 2; ++w)
                    while (p->readAnalyzer (w, drain, (int) std::size (drain)) > 0) {}
        }
        const double wall = juce::Time::highResolutionTicksToSeconds (
                                juce::Time::getHighResolutionTicks() - t0);
        std::printf ("  %s  %6.2fs wall  %6.1fx realtime  (~%4.1f%% of one core)\n",
                     c.name, wall, seconds / wall, 100.0 * wall / seconds);
    }
    return 0;
}

// ---- byte-identity fingerprints ---------------------------------------------
// FNV-1a checksums of full renders. The regression contract for DSP changes:
// any refactor of the drive chain must keep these identical for the shipped
// FUZZ voicing (run before and after; diff = you changed the sound).
static juce::uint64 renderChecksum (BoRBassEnhancerProcessor& p, const std::vector<float>& sig, int block)
{
    juce::AudioBuffer<float> buf (2, block);
    juce::MidiBuffer midi;
    juce::uint64 h = 1469598103934665603ULL;   // FNV-1a offset basis
    for (size_t pos = 0; pos < sig.size(); pos += (size_t) block)
    {
        const int n = (int) std::min ((size_t) block, sig.size() - pos);
        buf.setSize (2, n, false, false, true);
        for (int ch = 0; ch < 2; ++ch)
            buf.copyFrom (ch, 0, sig.data() + pos, n);
        p.processBlock (buf, midi);
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < n; ++i)
            {
                juce::uint32 bits;
                const float v = buf.getSample (ch, i);
                std::memcpy (&bits, &v, sizeof (bits));
                for (int b = 0; b < 4; ++b)
                { h ^= (bits >> (8 * b)) & 0xFF; h *= 1099511628211ULL; }
            }
    }
    return h;
}

static int runFnv()
{
    const double sr = 48000.0; const int block = 512;
    const auto sig = synthBassDI (sr, 6.0);

    struct Cfg { const char* name; bool fuzz; bool dist; const char* solo; bool unmuteSolo; };
    const Cfg cfgs[] = {
        { "default (fresh instance)   ", false, false, nullptr,   false },
        { "fuzz on x3                 ", true,  false, nullptr,   false },
        { "fuzz on, solo lofx57       ", true,  false, "lofx57",  false },
        { "fuzz on, solo lofx421      ", true,  false, "lofx421", false },
        { "fuzz on, solo lofxtwt      ", true,  false, "lofxtwt", false },
        // v0.2.0 additions (new fingerprints, recorded not inherited)
        { "DIST on x3                 ", true,  true,  nullptr,   false },
        { "DIST on, solo lofx57       ", true,  true,  "lofx57",  false },
        { "HI CRUNCH solo (unmuted)   ", false, false, "hioct",   true  },
        { "HI AIR solo (unmuted)      ", false, false, "hih",     true  },
    };
    std::printf ("render fingerprints (FNV-1a, 6s synth DI, 48k/512) + K-weighted loudness\n");
    for (const auto& c : cfgs)
    {
        auto p = makeProc (sr, block);
        setParam (*p, "analyzer", 0.0f);
        if (c.fuzz)
            for (auto* id : { "lofx57", "lofx421", "lofxtwt" })
                setParam (*p, juce::String (id) + "_fuzz", 1.0f);
        if (c.dist)
            for (auto* id : { "lofx57", "lofx421", "lofxtwt" })
                setParam (*p, juce::String (id) + "_drivetype", 1.0f);
        if (c.solo != nullptr)
        {
            setParam (*p, juce::String (c.solo) + "_solo", 1.0f);
            if (c.unmuteSolo) setParam (*p, juce::String (c.solo) + "_mute", 0.0f);
        }
        const auto h = renderChecksum (*p, sig, block);
        auto p2 = makeProc (sr, block);
        setParam (*p2, "analyzer", 0.0f);
        if (c.fuzz) for (auto* id : { "lofx57", "lofx421", "lofxtwt" }) setParam (*p2, juce::String (id) + "_fuzz", 1.0f);
        if (c.dist) for (auto* id : { "lofx57", "lofx421", "lofxtwt" }) setParam (*p2, juce::String (id) + "_drivetype", 1.0f);
        if (c.solo != nullptr)
        {
            setParam (*p2, juce::String (c.solo) + "_solo", 1.0f);
            if (c.unmuteSolo) setParam (*p2, juce::String (c.solo) + "_mute", 0.0f);
        }
        const double lk = renderLoudness (*p2, sig, sr, block, 1.0);
        std::printf ("  %s %016llx  %7.2f dB\n", c.name, (unsigned long long) h, lk);
    }
    return 0;
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    const juce::String mode = argc > 1 ? argv[1] : "";
    if (mode == "cal")   return runCal();
    if (mode == "bench") return runBench();
    if (mode == "fnv")   return runFnv();
    std::printf ("usage: bor-bench cal|bench|fnv\n");
    return 1;
}
