// bor-fuzz-variants — offline fuzz-envelope audition renders for Bass Better-er.
// © 2026 Box of Rules. Source-available, proprietary — see LICENSE.
// Developer tool, never shipped: build with -DBOR_BUILD_TOOLS=ON.
//
//   bor-fuzz-variants <in.wav> <out-dir>
//
// Renders the input DI through the full default session with FUZZ engaged on
// all three LO FX strips, once per AGC-envelope variant (attack / release /
// restore exponent — see FuzzChain::setEnvShape). Every render is RMS-matched
// to the stock variant so the ear test is level-fair, and a README.txt in the
// out-dir maps file -> variables.

#include "../Source/PluginProcessor.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <vector>

struct Variant { const char* name; float atkS, relS, exp; };

// stock is 5 ms / 250 ms / 1.0 — variant 01 is the reference
static const Variant VARIANTS[] = {
    { "01 STOCK atk5 rel250",          0.005f, 0.250f, 1.00f },
    { "02 rel350",                     0.005f, 0.350f, 1.00f },
    { "03 rel500",                     0.005f, 0.500f, 1.00f },
    { "04 rel700",                     0.005f, 0.700f, 1.00f },
    { "05 rel1000",                    0.005f, 1.000f, 1.00f },
    { "06 rel1500",                    0.005f, 1.500f, 1.00f },
    { "07 rel2500",                    0.005f, 2.500f, 1.00f },
    { "08 comp085",                    0.005f, 0.250f, 0.85f },
    { "09 comp070",                    0.005f, 0.250f, 0.70f },
    { "10 rel500 comp085",             0.005f, 0.500f, 0.85f },
    { "11 rel700 comp070",             0.005f, 0.700f, 0.70f },
    { "12 rel1000 comp085",            0.005f, 1.000f, 0.85f },
    // slower attack (atk15) auditioned awful — dropped from the grid
};

static void setParam (BoRBassEnhancerProcessor& p, const juce::String& id, float plainValue)
{
    auto* par = p.apvts.getParameter (id);
    if (par == nullptr)
    {
        std::fprintf (stderr, "bor-fuzz-variants: unknown parameter '%s'\n", id.toRawUTF8());
        std::exit (2);
    }
    par->setValueNotifyingHost (par->convertTo0to1 (plainValue));
}

int main (int argc, char** argv)
{
    if (argc != 3)
    {
        std::fprintf (stderr, "usage: bor-fuzz-variants <in.wav> <out-dir>\n");
        return 2;
    }

    juce::ScopedJuceInitialiser_GUI juceInit;   // message manager for param notifications

    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    const juce::File inFile (juce::String::fromUTF8 (argv[1]));
    std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (inFile));
    if (reader == nullptr)
    {
        std::fprintf (stderr, "bor-fuzz-variants: cannot read '%s'\n", argv[1]);
        return 2;
    }
    const double sr = reader->sampleRate;
    const int    numIn = (int) reader->numChannels;
    const juce::int64 len64 = reader->lengthInSamples;
    const int    len = (int) len64;

    juce::AudioBuffer<float> in (2, len);
    reader->read (&in, 0, len, 0, true, numIn > 1);
    if (numIn == 1) in.copyFrom (1, 0, in, 0, 0, len);   // mono -> both channels

    const juce::File outDir (juce::String::fromUTF8 (argv[2]));
    outDir.createDirectory();

    constexpr int block = 512;
    double stockRms = 0.0;
    juce::StringArray notes;

    for (const auto& v : VARIANTS)
    {
        auto p = std::make_unique<BoRBassEnhancerProcessor>();
        p->setPlayConfigDetails (2, 2, sr, block);
        p->prepareToPlay (sr, block);
        // the shipped dials cover release + restore exponent; atk is fixed 5 ms
        setParam (*p, "drive_rel", v.relS * 1000.0f);
        setParam (*p, "drive_sustain", (1.0f - v.exp) * 200.0f);
        setParam (*p, "analyzer", 0.0f);
        for (auto* id : { "lofx57", "lofx421", "lofxtwt" })
            setParam (*p, juce::String (id) + "_fuzz", 1.0f);

        juce::AudioBuffer<float> out (2, len), buf (2, block);
        juce::MidiBuffer midi;
        for (int pos = 0; pos < len; pos += block)
        {
            const int n = juce::jmin (block, len - pos);
            buf.setSize (2, n, false, false, true);
            for (int ch = 0; ch < 2; ++ch)
                buf.copyFrom (ch, 0, in, ch, pos, n);
            p->processBlock (buf, midi);
            for (int ch = 0; ch < 2; ++ch)
                out.copyFrom (ch, pos, buf, ch, 0, n);
        }

        double sumSq = 0.0;
        float pk = 0.0f;
        for (int ch = 0; ch < 2; ++ch)
        {
            const float* s = out.getReadPointer (ch);
            for (int i = 0; i < len; ++i) { sumSq += (double) s[i] * s[i]; }
            pk = juce::jmax (pk, out.getMagnitude (ch, 0, len));
        }
        const double rms = std::sqrt (sumSq / (2.0 * len) + 1.0e-20);
        if (stockRms == 0.0) stockRms = rms;   // first variant is the reference

        float g = (float) (stockRms / rms);
        if (pk * g > 0.98f)   // keep 24-bit headroom; note the shortfall instead of clipping
        {
            notes.add (juce::String (v.name) + ": level-match capped by peak ("
                       + juce::String (juce::Decibels::gainToDecibels (0.98f / (pk * g)), 1) + " dB low)");
            g = 0.98f / pk;
        }
        out.applyGain (g);

        const juce::File outFile = outDir.getChildFile (juce::String (v.name) + ".wav");
        outFile.deleteFile();
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::AudioFormatWriter> writer (
            wav.createWriterFor (new juce::FileOutputStream (outFile), sr, 2, 24, {}, 0));
        if (writer == nullptr || ! writer->writeFromAudioSampleBuffer (out, 0, len))
        {
            std::fprintf (stderr, "bor-fuzz-variants: cannot write '%s'\n",
                          outFile.getFullPathName().toRawUTF8());
            return 2;
        }
        writer.reset();
        std::printf ("  %-28s  match %+.1f dB\n", v.name, juce::Decibels::gainToDecibels (g));
        std::fflush (stdout);
    }

    juce::String readme;
    readme << "Bass Better-er fuzz-envelope variants — " << inFile.getFileName() << "\n"
           << "Full default session, FUZZ engaged on all three LO FX strips.\n"
           << "All files RMS-matched to 01 STOCK so the comparison is level-fair.\n\n"
           << "Variables (FuzzChain AGC envelope — the fuzz's output envelope IS this follower):\n"
           << "  relNNN  — release ms (stock 250). The fast fall from each pluck's attack peak\n"
           << "            down to the body level is the 'sidechain duck'; slower = flatter dip,\n"
           << "            longer hold on decaying notes.\n"
           << "  compNNN — envelope restore exponent /100 (stock 1.00). Below 1 the strip's\n"
           << "            dynamics compress upward: quieter tails come back louder = sustain.\n"
           << "  atkNN   — attack ms (stock 5). Slower = softer grab of the pluck transient.\n\n";
    for (const auto& n : notes) readme << "NOTE " << n << "\n";
    outDir.getChildFile ("README.txt").replaceWithText (readme);
    return 0;
}
