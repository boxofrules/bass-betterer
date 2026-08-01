// bor-preset-match — fit a Bass Better-er preset to a reference tone.
// © 2026 Box of Rules. Source-available, proprietary — see LICENSE.
// Developer tool, never shipped: build with -DBOR_BUILD_TOOLS=ON.
//
//   bor-preset-match <di.wav> <target.wav> <out-preset.xml> <preset-name>
//
// Renders the DI through the real processor and coordinate-descends the
// preset surface (strip gains, DI blend, fuzz/type keys, DRIVE/RELEASE,
// INPUT/GLUE, master-EQ gains + band freqs) to minimise the 1/6-octave
// log-spectrum distance to the target. Output gain is solved analytically
// (the metric is gain-invariant; the mean dB offset becomes out_gain).
// Writes a loadable user-preset XML (apvts state + stateVersion).

#include "../Source/PluginProcessor.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <map>
#include <vector>

// ---- wav load ---------------------------------------------------------------
static juce::AudioBuffer<float> loadWav (const char* path, double& srOut)
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (juce::File (juce::String::fromUTF8 (path))));
    if (r == nullptr) { std::fprintf (stderr, "cannot read %s\n", path); std::exit (2); }
    srOut = r->sampleRate;
    juce::AudioBuffer<float> b (2, (int) r->lengthInSamples);
    r->read (&b, 0, (int) r->lengthInSamples, 0, true, r->numChannels > 1);
    if (r->numChannels == 1) b.copyFrom (1, 0, b, 0, 0, b.getNumSamples());
    return b;
}

// ---- 1/6-octave mono log-spectrum -------------------------------------------
struct Spectrum
{
    static constexpr int ORDER = 12, SIZE = 1 << ORDER, HOP = SIZE / 2;
    std::vector<double> bandDb;   // 30 Hz .. 16 kHz, 1/6 oct

    static std::vector<std::pair<int,int>> bandBins (double sr)
    {
        std::vector<std::pair<int,int>> bands;
        for (double lo = 30.0; lo < 16000.0; lo *= std::pow (2.0, 1.0 / 6.0))
        {
            const double hi = lo * std::pow (2.0, 1.0 / 6.0);
            int b0 = (int) std::floor (lo * SIZE / sr), b1 = (int) std::floor (hi * SIZE / sr);
            b0 = juce::jlimit (1, SIZE / 2 - 1, b0);
            b1 = juce::jlimit (b0, SIZE / 2 - 1, b1);
            bands.push_back ({ b0, b1 });
        }
        return bands;
    }

    static Spectrum of (const juce::AudioBuffer<float>& buf, double sr)
    {
        static juce::dsp::FFT fft { ORDER };
        const int n = buf.getNumSamples();
        std::vector<float> win ((size_t) SIZE);
        for (int i = 0; i < SIZE; ++i)
            win[(size_t) i] = 0.5f * (1.0f - std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) (SIZE - 1)));
        std::vector<double> power ((size_t) SIZE / 2, 0.0);
        std::vector<float> td ((size_t) SIZE * 2);
        int frames = 0;
        for (int pos = 0; pos + SIZE <= n; pos += HOP, ++frames)
        {
            for (int i = 0; i < SIZE; ++i)
                td[(size_t) i] = 0.5f * (buf.getSample (0, pos + i) + buf.getSample (1, pos + i)) * win[(size_t) i];
            std::fill (td.begin() + SIZE, td.end(), 0.0f);
            fft.performFrequencyOnlyForwardTransform (td.data());
            for (int k = 0; k < SIZE / 2; ++k)
                power[(size_t) k] += (double) td[(size_t) k] * td[(size_t) k];
        }
        Spectrum s;
        for (auto [b0, b1] : bandBins (sr))
        {
            double sum = 0.0;
            for (int k = b0; k <= b1; ++k) sum += power[(size_t) k];
            s.bandDb.push_back (10.0 * std::log10 (sum / juce::jmax (1, frames) + 1.0e-18));
        }
        return s;
    }
};

// gain-invariant distance: mean-square dB error after removing the mean offset;
// the removed offset (target - render) is returned as the ideal out_gain in dB
static double distance (const Spectrum& render, const Spectrum& target, double& gainDbOut)
{
    const size_t nb = juce::jmin (render.bandDb.size(), target.bandDb.size());
    double off = 0.0;
    for (size_t i = 0; i < nb; ++i) off += target.bandDb[i] - render.bandDb[i];
    off /= (double) nb;
    double err = 0.0;
    for (size_t i = 0; i < nb; ++i)
    {
        const double d = target.bandDb[i] - (render.bandDb[i] + off);
        err += d * d;
    }
    gainDbOut = off;
    return err / (double) nb;
}

// ---- render the DI through the processor with a param set -------------------
using ParamMap = std::map<juce::String, float>;

static juce::AudioBuffer<float> renderWith (const ParamMap& params,
                                            const juce::AudioBuffer<float>& di, double sr)
{
    constexpr int block = 512;
    BoRBassEnhancerProcessor proc;
    proc.setPlayConfigDetails (2, 2, sr, block);
    proc.prepareToPlay (sr, block);
    for (const auto& kv : params)
    {
        auto* p = proc.apvts.getParameter (kv.first);
        if (p == nullptr) { std::fprintf (stderr, "unknown param %s\n", kv.first.toRawUTF8()); std::exit (2); }
        p->setValueNotifyingHost (p->convertTo0to1 (kv.second));
    }
    const int len = di.getNumSamples();
    juce::AudioBuffer<float> out (2, len), buf (2, block);
    juce::MidiBuffer midi;
    for (int pos = 0; pos < len; pos += block)
    {
        const int n = juce::jmin (block, len - pos);
        buf.setSize (2, n, false, false, true);
        for (int ch = 0; ch < 2; ++ch) buf.copyFrom (ch, 0, di, ch, pos, n);
        proc.processBlock (buf, midi);
        for (int ch = 0; ch < 2; ++ch) out.copyFrom (ch, pos, buf, ch, 0, n);
    }
    return out;
}

int main (int argc, char** argv)
{
    if (argc != 5)
    {
        std::fprintf (stderr, "usage: bor-preset-match <di.wav> <target.wav> <out-preset.xml> <name>\n");
        return 2;
    }
    juce::ScopedJuceInitialiser_GUI juceInit;

    double srD = 0, srT = 0;
    auto di     = loadWav (argv[1], srD);
    auto target = loadWav (argv[2], srT);
    if (srD != srT) { std::fprintf (stderr, "sample-rate mismatch\n"); return 2; }
    const auto targetSpec = Spectrum::of (target, srT);

    // start point: the plugin's defaults, analyzer off for determinism/speed
    ParamMap best { { "analyzer", 0.0f } };
    double bestGainDb = 0.0;
    auto evalErr = [&] (const ParamMap& pm, double& gDb)
    { return distance (Spectrum::of (renderWith (pm, di, srD), srD), targetSpec, gDb); };
    double bestErr = evalErr (best, bestGainDb);
    std::printf ("start err %.3f\n", bestErr); std::fflush (stdout);

    struct CCoord { const char* id; float lo, hi, step; };            // continuous
    static const std::vector<CCoord> cont {
        { "in_gain",       -12.0f, 12.0f, 3.0f },
        { "sub_gain",      -24.0f, 12.0f, 3.0f },
        { "lowcln1_gain",  -24.0f, 12.0f, 3.0f },
        { "lowcln2_gain",  -24.0f, 12.0f, 3.0f },
        { "lofx57_gain",   -24.0f, 12.0f, 3.0f },
        { "lofx421_gain",  -24.0f, 12.0f, 3.0f },
        { "lofxtwt_gain",  -24.0f, 12.0f, 3.0f },
        { "roomnear_gain", -60.0f,  3.0f, 6.0f },
        { "roomfar_gain",  -60.0f,  3.0f, 6.0f },
        { "di_gain",       -24.0f,  6.0f, 3.0f },
        { "glue",            0.0f,  1.0f, 0.25f },
        { "drive_amt",      25.0f, 400.0f, 50.0f },
        { "eq_low",         -9.0f,  9.0f, 2.0f },
        { "eq_lomid",       -9.0f,  9.0f, 2.0f },
        { "eq_himid",       -9.0f,  9.0f, 2.0f },
        { "eq_high",        -9.0f,  9.0f, 2.0f },
    };
    struct DCoord { const char* id; std::vector<float> opts; };        // discrete
    static const std::vector<DCoord> disc {
        { "lofx57_fuzz",       { 0, 1 } },
        { "lofx421_fuzz",      { 0, 1 } },
        { "lofxtwt_fuzz",      { 0, 1 } },
        { "lofx57_drivetype",  { 0, 1 } },
        { "lofx421_drivetype", { 0, 1 } },
        { "lofxtwt_drivetype", { 0, 1 } },
        { "di_mute",           { 0, 1 } },
        { "drive_rel",         { 250, 1000, 2500 } },
        { "eq_low_freq",       { 0, 1, 2, 3 } },
        { "eq_lomid_freq",     { 0, 1, 2, 3 } },
        { "eq_himid_freq",     { 0, 1, 2, 3 } },
        { "eq_high_freq",      { 0, 1, 2, 3 } },
    };
    // defaults for coords not yet in the map (so +step moves from the true start)
    auto cur = [&] (const char* id, float fallback)
    { auto it = best.find (id); return it != best.end() ? it->second : fallback; };
    ParamMap defs;
    {
        BoRBassEnhancerProcessor tmp;
        for (const auto& c : cont) defs[c.id] = tmp.apvts.getRawParameterValue (c.id)->load();
        for (const auto& d : disc) defs[d.id] = tmp.apvts.getRawParameterValue (d.id)->load();
    }

    for (int pass = 0; pass < 4; ++pass)
    {
        const float scale = 1.0f / (float) (1 << pass);   // 1, 1/2, 1/4, 1/8
        for (const auto& d : disc)                        // discrete first: big moves
            for (float opt : d.opts)
            {
                if (juce::exactlyEqual (opt, cur (d.id, defs[d.id]))) continue;
                ParamMap trial = best; trial[d.id] = opt;
                double g; const double e = evalErr (trial, g);
                if (e < bestErr) { best = trial; bestErr = e; bestGainDb = g; }
            }
        for (const auto& c : cont)
        {
            const float step = c.step * scale;
            for (float dir : { -1.0f, 1.0f })
            {
                for (;;)   // walk while it improves
                {
                    const float v = juce::jlimit (c.lo, c.hi, cur (c.id, defs[c.id]) + dir * step);
                    if (juce::exactlyEqual (v, cur (c.id, defs[c.id]))) break;
                    ParamMap trial = best; trial[c.id] = v;
                    double g; const double e = evalErr (trial, g);
                    if (e >= bestErr) break;
                    best = trial; bestErr = e; bestGainDb = g;
                }
            }
        }
        std::printf ("pass %d err %.3f\n", pass, bestErr); std::fflush (stdout);
    }

    // bake the analytic loudness match into out_gain, then write the preset
    best["out_gain"] = juce::jlimit (-24.0f, 24.0f, (float) bestGainDb);
    best.erase ("analyzer");   // presets shouldn't carry the display switch
    {
        BoRBassEnhancerProcessor proc;
        for (const auto& kv : best)
            if (auto* p = proc.apvts.getParameter (kv.first))
                p->setValueNotifyingHost (p->convertTo0to1 (kv.second));
        if (auto xml = proc.apvts.copyState().createXml())
        {
            xml->setAttribute ("stateVersion", 5);
            xml->writeTo (juce::File (juce::String::fromUTF8 (argv[3])));
        }
    }
    std::printf ("final err %.3f (out_gain %+.1f dB) -> %s\n", bestErr, bestGainDb, argv[3]);
    std::printf ("settings for '%s':\n", argv[4]);
    for (const auto& kv : best) std::printf ("  %-22s %g\n", kv.first.toRawUTF8(), kv.second);
    return 0;
}
