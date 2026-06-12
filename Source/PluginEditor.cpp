#include "PluginEditor.h"
#include <cmath>

#ifndef JucePlugin_VersionString
 #define JucePlugin_VersionString "dev"   // console/tool builds (bor-bench)
#endif

// ============================================================================
// Box of Rules brand tokens (from the design system colors_and_type.css).
// ============================================================================
namespace bor
{
    const juce::Colour ink        { 0xff0B0B0C };
    const juce::Colour ink2       { 0xff16171A };
    const juce::Colour ink3       { 0xff1F2126 };
    const juce::Colour bone       { 0xffEDE7DA };
    const juce::Colour mute       { 0xff6E6E73 };
    const juce::Colour mute2      { 0xff9B958A };
    const juce::Colour rule       { 0xff2A2C30 };
    const juce::Colour accent     { 0xff69AFBF };   // signal cyan
    const juce::Colour accentOn   { 0xff0B0B0C };
    const juce::Colour sigOff     { 0xff3A3C40 };
    const juce::Colour vu         { 0xff4FAE5A };
    const juce::Colour amber      { 0xffF2A20C };
    const juce::Colour oxbloodHi  { 0xffC42424 };

    inline juce::Font mono (float h, bool bold = false)
    {
        return juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(),
                                              h, bold ? juce::Font::bold : juce::Font::plain));
    }
}

// Helper structs in a named namespace (avoids internal-linkage member warnings).
namespace bbe
{
using SA = juce::AudioProcessorValueTreeState::SliderAttachment;
using BA = juce::AudioProcessorValueTreeState::ButtonAttachment;

// ---- flat hairline toggle (M / S / Ø / SC / FUZZ / FREQ) --------------------
struct SquareButton : public juce::Button
{
    SquareButton (const juce::String& t, juce::Colour onBg_, juce::Colour onFg_)
        : juce::Button (t), onBg (onBg_), onFg (onFg_) { setClickingTogglesState (true); }

    void paintButton (juce::Graphics& g, bool hover, bool) override
    {
        const bool on = getToggleState();
        juce::Colour bg = juce::Colours::transparentBlack, fg = bor::mute2, bd = bor::rule;
        if (on)         { bg = onBg;      fg = onFg;       bd = onBg; }
        else if (hover) { bg = bor::ink3; fg = bor::bone;  bd = bor::mute; }

        auto b = getLocalBounds().toFloat();
        if (! bg.isTransparent()) { g.setColour (bg); g.fillRect (b); }
        g.setColour (bd); g.drawRect (b, 1.0f);
        g.setColour (fg);
        g.setFont (bor::mono (juce::jmin (12.0f, b.getHeight() * 0.55f), true));
        g.drawText (getButtonText(), getLocalBounds(), juce::Justification::centred, false);
    }
    juce::Colour onBg, onFg;
};

// ---- schematic vertical fader (track + accent fill + square cap) ------------
struct FaderLnf : public juce::LookAndFeel_V4
{
    void drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height,
                           float sliderPos, float, float,
                           juce::Slider::SliderStyle, juce::Slider& s) override
    {
        const float cx = x + width * 0.5f, top = (float) y, bot = (float) y + (float) height;
        g.setColour (s.findColour (juce::Slider::backgroundColourId));
        g.fillRect (cx - 1.0f, top, 2.0f, (float) height);
        g.setColour (s.findColour (juce::Slider::trackColourId));
        g.fillRect (cx - 1.0f, sliderPos, 2.0f, bot - sliderPos);
        const float cap = 18.0f;
        juce::Rectangle<float> capR (cx - cap * 0.5f, sliderPos - cap * 0.5f, cap, cap);
        g.setColour (s.findColour (juce::Slider::thumbColourId));
        g.fillRect (capR);
        if (s.isMouseOverOrDragging()) { g.setColour (bor::bone); g.drawRect (capR.expanded (2.0f), 1.0f); }
    }
};

// ---- arc rotary (outline circle, cyan arc, bone pointer) -------------------
struct KnobLnf : public juce::LookAndFeel_V4
{
    void drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                           float pos, float startAngle, float endAngle, juce::Slider&) override
    {
        auto b = juce::Rectangle<int> (x, y, width, height).toFloat().reduced (4.0f);
        const float cx = b.getCentreX(), cy = b.getCentreY();
        const float r  = juce::jmin (b.getWidth(), b.getHeight()) * 0.5f;
        g.setColour (bor::rule);
        g.drawEllipse (cx - r, cy - r, r * 2.0f, r * 2.0f, 2.0f);
        const float toAngle = startAngle + pos * (endAngle - startAngle);
        juce::Path arc;
        arc.addCentredArc (cx, cy, r, r, 0.0f, startAngle, toAngle, true);
        g.setColour (bor::accent);
        g.strokePath (arc, juce::PathStrokeType (2.0f));
        const float px = cx + std::sin (toAngle) * (r - 4.0f);
        const float py = cy - std::cos (toAngle) * (r - 4.0f);
        g.setColour (bor::bone);
        g.drawLine (cx, cy, px, py, 2.0f);
        g.fillEllipse (cx - 2.0f, cy - 2.0f, 4.0f, 4.0f);
    }
};

// ---- editor LnF: brand-toned diagonal grip so the resize corner is findable ----
struct EditorLnf : public juce::LookAndFeel_V4
{
    void drawCornerResizer (juce::Graphics& g, int w, int h, bool isMouseOver, bool isMouseDragging) override
    {
        g.setColour (isMouseOver || isMouseDragging ? bor::mute2 : bor::mute);
        const float ww = (float) w, hh = (float) h;
        for (float i = 0.3f; i < 1.0f; i += 0.3f)
            g.drawLine (ww * i, hh, ww, hh * i, 1.0f);
    }
};

// ---- hex BR monogram (from BOR-icon-black.svg), filled signal cyan ----------
struct HexLogo : public juce::Component
{
    HexLogo()
    {
        const juce::String svg =
            "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 300 300'>"
            "<path fill-rule='evenodd' clip-rule='evenodd' fill='#69AFBF' d='"
            "M150.13,54.79l13.76,7.94,13.76,7.95,13.76,7.94,13.76,7.95,13.76,7.94,13.76,7.94v40.83"
            "s-11.84,6.84-11.84,6.84l11.84,6.84v40.83s-27.52,15.89-27.52,15.89v-63.56h-41.28v87.39"
            "l-13.76,7.94-82.56-47.67v-40.83l11.84-6.84-11.84-6.84v-40.83l82.56-47.67Z"
            "M163.89,95.04v35.16l41.28,5.44v-25.58l-41.28-15.02Z"
            "M136.37,95.04l-41.28,15.02v25.57l41.28-5.44v-35.16Z"
            "M136.37,150.13h-41.28v40.07l41.28,15.03v-55.09Z'/></svg>";
        if (auto xml = juce::XmlDocument::parse (svg))
            drawable = juce::Drawable::createFromSVG (*xml);
        setInterceptsMouseClicks (false, false);
    }
    void paint (juce::Graphics& g) override
    {
        if (drawable != nullptr)
            drawable->drawWithin (g, getLocalBounds().toFloat(), juce::RectanglePlacement::centred, 1.0f);
    }
    std::unique_ptr<juce::Drawable> drawable;
};

// ---- master knob: caption + rotary + tabular value -------------------------
struct Knob : public juce::Component
{
    Knob (BoRBassEnhancerProcessor& p, const juce::String& pid, const juce::String& cap,
          int decimals_, KnobLnf& lnf) : decimals (decimals_)
    {
        slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        slider.setRotaryParameters (juce::degreesToRadians (225.0f), juce::degreesToRadians (495.0f), true);
        slider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        slider.setLookAndFeel (&lnf);
        addAndMakeVisible (slider);
        att = std::make_unique<SA> (p.apvts, pid, slider);
        caption.setText (cap, juce::dontSendNotification);
        caption.setJustificationType (juce::Justification::centred);
        caption.setFont (bor::mono (11.0f));
        caption.setColour (juce::Label::textColourId, bor::mute2);
        addAndMakeVisible (caption);
        value.setJustificationType (juce::Justification::centred);
        value.setFont (bor::mono (15.0f));
        value.setColour (juce::Label::textColourId, bor::bone);
        addAndMakeVisible (value);
        slider.onValueChange = [this] { refresh(); };
        refresh();
    }
    ~Knob() override { slider.setLookAndFeel (nullptr); }
    void refresh() { value.setText (juce::String (slider.getValue(), decimals), juce::dontSendNotification); }
    void resized() override
    {
        auto r = getLocalBounds();
        caption.setBounds (r.removeFromTop (14));
        value.setBounds (r.removeFromBottom (20));
        slider.setBounds (r.withSizeKeepingCentre (58, 58));
    }
    int decimals;
    juce::Slider slider;
    juce::Label caption, value;
    std::unique_ptr<SA> att;
};

// ---- one channel strip -----------------------------------------------------
struct Strip : public juce::Component
{
    Strip (BoRBassEnhancerProcessor& p, const juce::String& prefix, const juce::String& nm,
           bool isFX_, bool hasPan_, bool hasAB_, FaderLnf& flnf, KnobLnf& klnf)
        : isFX (isFX_), hasPan (hasPan_), hasAB (hasAB_), chName (nm)
    {
        gain.setSliderStyle (juce::Slider::LinearVertical);
        gain.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        gain.setLookAndFeel (&flnf);
        gain.setColour (juce::Slider::backgroundColourId, bor::rule);
        addAndMakeVisible (gain);
        gainAtt = std::make_unique<SA> (p.apvts, prefix + "_gain", gain);
        gain.onValueChange = [this] { repaint(); };

        mute  = makeTog ("M",  bor::bone,   bor::ink,     "Mute");
        solo  = makeTog ("S",  bor::accent, bor::accentOn,"Solo");
        phase = makeTog (juce::String::fromUTF8 ("\xC3\x98"), bor::bone, bor::ink, "Phase invert");
        sc    = makeTog ("SC", bor::amber,  bor::ink,     "Duck this layer from the LO FX sidechain");
        muteAtt  = std::make_unique<BA> (p.apvts, prefix + "_mute",  *mute);
        soloAtt  = std::make_unique<BA> (p.apvts, prefix + "_solo",  *solo);
        phaseAtt = std::make_unique<BA> (p.apvts, prefix + "_phase", *phase);
        scAtt    = std::make_unique<BA> (p.apvts, prefix + "_duck",  *sc);
        mute->onStateChange = [this] { refreshMuted(); };

        if (isFX)
        {
            fuzz = makeTog ("FUZZ", bor::accent, bor::accentOn, "Engage fuzz");
            fuzzAtt = std::make_unique<BA> (p.apvts, prefix + "_fuzz", *fuzz);
        }
        if (hasPan)
        {
            pan.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
            pan.setRotaryParameters (juce::degreesToRadians (225.0f), juce::degreesToRadians (495.0f), true);
            pan.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
            pan.setLookAndFeel (&klnf);
            pan.setTooltip ("Pan");
            addAndMakeVisible (pan);
            panAtt = std::make_unique<SA> (p.apvts, prefix + "_pan", pan);
        }
        if (hasAB)   // DI strip: A/B audition lives where pan would (owner wires onClick)
            ab = makeTog ("A/B", bor::amber, bor::ink, "Audition the raw DI against the processed sound");
        refreshMuted();
    }

    ~Strip() override { gain.setLookAndFeel (nullptr); if (hasPan) pan.setLookAndFeel (nullptr); }

    std::unique_ptr<SquareButton> makeTog (const juce::String& t, juce::Colour bg, juce::Colour fg,
                                           const juce::String& tip)
    {
        auto b = std::make_unique<SquareButton> (t, bg, fg);
        b->setTooltip (tip);
        addAndMakeVisible (*b);
        return b;
    }

    void refreshMuted()
    {
        muted = mute->getToggleState();
        gain.setColour (juce::Slider::trackColourId, muted ? bor::sigOff : bor::accent);
        gain.setColour (juce::Slider::thumbColourId, muted ? bor::mute   : bor::accent);
        gain.repaint(); repaint();
    }

    void setLevel (float l) { if (std::abs (l - level) > 0.004f) { level = l; repaint(); } }

    void paint (juce::Graphics& g) override
    {
        auto b = getLocalBounds();
        g.setColour (bor::ink2); g.fillRect (b);
        g.setColour (bor::rule); g.drawRect (b, 1);

        g.setColour (bor::bone);
        g.setFont (bor::mono (11.0f, true));
        g.drawFittedText (chName, nameRect, juce::Justification::centredTop, 2);

        const int segs = 22;
        auto m = meterRect.toFloat();
        const float gap = 2.0f;
        const float segH = (m.getHeight() - gap * (segs - 1)) / segs;
        const int lit = muted ? 0 : juce::roundToInt (level * segs);
        for (int i = 0; i < segs; ++i)
        {
            const float yy = m.getBottom() - (i + 1) * segH - i * gap;
            juce::Colour c = bor::sigOff;
            if (i < lit) c = (i >= segs - 2) ? bor::oxbloodHi : (i >= segs - 5 ? bor::amber : bor::vu);
            g.setColour (c);
            g.fillRect (m.getX(), yy, m.getWidth(), segH);
        }

        g.setColour (muted ? bor::mute : bor::bone);
        g.setFont (bor::mono (17.0f));
        g.drawText (juce::String (gain.getValue(), 1), readoutRect, juce::Justification::centred);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (1).withTrimmedLeft (8).withTrimmedRight (8);
        r.removeFromTop (11);
        nameRect = r.removeFromTop (28);
        r.removeFromTop (6);

        auto ms = r.removeFromTop (22);
        const int gw = (ms.getWidth() - 9) / 4;
        mute->setBounds  (ms.removeFromLeft (gw)); ms.removeFromLeft (3);
        solo->setBounds  (ms.removeFromLeft (gw)); ms.removeFromLeft (3);
        phase->setBounds (ms.removeFromLeft (gw)); ms.removeFromLeft (3);
        sc->setBounds    (ms.removeFromLeft (gw));

        r.removeFromTop (8);
        auto fuzzRow = r.removeFromTop (22);
        if (fuzz != nullptr) fuzz->setBounds (fuzzRow);

        r.removeFromTop (12);
        if (hasPan)      pan.setBounds (r.removeFromBottom (32).withSizeKeepingCentre (34, 30));
        else if (hasAB)  ab->setBounds (r.removeFromBottom (32).withSizeKeepingCentre (52, 22));
        readoutRect = r.removeFromBottom (22);
        r.removeFromBottom (8);

        auto block = r.withSizeKeepingCentre (34 + 8 + 8, r.getHeight());
        gain.setBounds (block.removeFromLeft (34));
        block.removeFromLeft (8);
        meterRect = block.removeFromLeft (8);
    }

    bool isFX, hasPan, hasAB;
    juce::String chName;
    juce::Slider gain, pan;
    std::unique_ptr<SquareButton> mute, solo, phase, sc, fuzz, ab;
    std::unique_ptr<SA> gainAtt, panAtt;
    std::unique_ptr<BA> muteAtt, soloAtt, phaseAtt, scAtt, fuzzAtt;
    float level = 0.0f;
    bool  muted = false;
    juce::Rectangle<int> nameRect, readoutRect, meterRect;
};

// ---- spectrum analyzer (FFT of the output + the raw DI; processor fifos) ----
struct Analyzer : public juce::Component
{
    // 8192-sample FFT -> ~5.9 Hz bins at 48 kHz: the low end renders as a curve
    // instead of the stair-steps a 2048 FFT produced below ~500 Hz.
    static constexpr int order = 13, fftSize = 1 << order, ringMask = fftSize - 1, NPTS = 200;

    Analyzer() : fft (order), window ((size_t) fftSize, juce::dsp::WindowingFunction<float>::hann) {}

    struct Trace
    {
        juce::HeapBlock<float> ring { (size_t) fftSize, true };
        int ringW = 0;
        std::array<float, NPTS> disp {};
    };

    static float ptFreq (float i) { return 20.0f * std::pow (1000.0f, i / (float) (NPTS - 1)); } // 20..20k

    void update (BoRBassEnhancerProcessor& p)
    {
        if (! p.analyzerEnabled())
        {
            if (on) { on = false; for (auto& t : traces) t.disp.fill (0.0f); repaint(); }
            return;
        }
        on = true;
        const float sr  = p.getSampleRate() > 0 ? (float) p.getSampleRate() : 48000.0f;
        const float nyq = sr * 0.5f;

        for (int w = 0; w < 2; ++w)   // 0 = processed output, 1 = raw DI
        {
            auto& tr = traces[(size_t) w];
            int total = 0, got;
            while ((got = p.readAnalyzer (w, tmp, fftSize)) > 0)
            {
                for (int i = 0; i < got; ++i) tr.ring[(size_t) (tr.ringW++ & ringMask)] = tmp[i];
                total += got;
                if (total > (1 << 15)) break;
            }

            for (int i = 0; i < fftSize; ++i)
                fftData[(size_t) i] = tr.ring[(size_t) ((tr.ringW - fftSize + i) & ringMask)];
            window.multiplyWithWindowingTable (fftData, (size_t) fftSize);
            fft.performFrequencyOnlyForwardTransform (fftData);

            // bins this display point spans (log axis): average their power when the
            // span is wide (HF: no skipped peaks), interpolate between neighbouring
            // bins when it is narrow (LF: no stair-steps)
            const float binHz = sr / (float) fftSize;
            auto magAt = [this] (float bin)
            {
                const int i0 = juce::jlimit (1, fftSize / 2 - 2, (int) bin);
                const float frac = juce::jlimit (0.0f, 1.0f, bin - (float) i0);
                return fftData[(size_t) i0] * (1.0f - frac) + fftData[(size_t) (i0 + 1)] * frac;
            };
            for (int i = 0; i < NPTS; ++i)
            {
                auto& d = tr.disp[(size_t) i];
                // the x-axis runs to 20 kHz regardless of rate: above Nyquist there is
                // no signal, so fall to the floor instead of smearing the last bin
                // across the top of the display (auval tests 11025/22050 Hz)
                if (ptFreq ((float) i) >= nyq) { d *= 0.7f; continue; }
                const float ba = ptFreq ((float) i - 0.5f) / binHz;
                const float bb = ptFreq ((float) i + 0.5f) / binHz;
                float mag;
                if (bb - ba >= 1.0f)
                {
                    const int s = juce::jlimit (1, fftSize / 2 - 1, (int) std::ceil (ba));
                    const int e = juce::jlimit (s, fftSize / 2 - 1, (int) bb);
                    float pw = 0.0f;
                    for (int b = s; b <= e; ++b) pw += fftData[(size_t) b] * fftData[(size_t) b];
                    mag = std::sqrt (pw / (float) (e - s + 1));
                }
                else
                    mag = magAt (ptFreq ((float) i) / binHz);

                const float db   = juce::Decibels::gainToDecibels (mag / ((float) fftSize * 0.5f), -100.0f);
                const float norm = juce::jlimit (0.0f, 1.0f, (db + 90.0f) / 90.0f);
                d = norm > d ? norm : d * 0.7f + norm * 0.3f;
            }
        }
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat();
        g.setColour (bor::ink2); g.fillRect (b);
        g.setColour (bor::rule); g.drawRect (b, 1.0f);
        auto inner = b.reduced (1.0f);

        auto xForFreq = [&] (float f) { return inner.getX() + inner.getWidth() * std::log (f / 20.0f) / std::log (1000.0f); };
        auto yForDb   = [&] (float db) { return inner.getBottom() - juce::jlimit (0.0f, 1.0f, (db + 90.0f) / 90.0f) * inner.getHeight(); };

        // --- grid: log-frequency verticals + a couple of dB references ---
        g.setFont (bor::mono (8.0f));
        struct Tick { float hz; const char* label; };
        static const Tick ticks[] = { {50,nullptr},{100,"100"},{200,nullptr},{500,nullptr},
                                      {1000,"1k"},{2000,nullptr},{5000,nullptr},{10000,"10k"},{20000,nullptr} };
        for (auto& t : ticks)
        {
            const float x = xForFreq (t.hz);
            g.setColour (bor::rule.withAlpha (0.55f));
            g.drawVerticalLine ((int) x, inner.getY(), inner.getBottom() - 1.0f);
            if (t.label != nullptr)
            {
                g.setColour (bor::mute2);
                g.drawText (t.label, juce::Rectangle<float> (x + 2.0f, inner.getBottom() - 11.0f, 28.0f, 10.0f),
                            juce::Justification::topLeft);
            }
        }
        for (float db : { -12.0f, -24.0f, -48.0f })
        {
            const float y = yForDb (db);
            g.setColour (bor::rule.withAlpha (0.5f));
            g.drawHorizontalLine ((int) y, inner.getX(), inner.getRight());
            g.setColour (bor::mute2);
            g.drawText (juce::String ((int) db), juce::Rectangle<float> (inner.getRight() - 24.0f, y - 10.0f, 22.0f, 10.0f),
                        juce::Justification::topRight);
        }

        if (on)
        {
            auto tracePath = [&] (const Trace& tr, juce::Path& line)
            {
                for (int i = 0; i < NPTS; ++i)
                {
                    const float x = inner.getX() + inner.getWidth() * (float) i / (float) (NPTS - 1);
                    const float y = inner.getBottom() - tr.disp[(size_t) i] * inner.getHeight();
                    if (i == 0) line.startNewSubPath (x, y); else line.lineTo (x, y);
                }
            };

            // raw DI underneath in grey, processed output on top in signal cyan
            if (view != 2)   // ALL or PRE
            {
                juce::Path diLine;
                tracePath (traces[1], diLine);
                g.setColour (bor::mute2.withAlpha (0.85f));
                g.strokePath (diLine, juce::PathStrokeType (view == 1 ? 1.5f : 1.0f));
            }
            if (view != 1)   // ALL or POST
            {
                juce::Path outLine;
                tracePath (traces[0], outLine);
                juce::Path fill (outLine);
                fill.lineTo (inner.getRight(), inner.getBottom());
                fill.lineTo (inner.getX(), inner.getBottom());
                fill.closeSubPath();
                g.setColour (bor::accent.withAlpha (0.16f)); g.fillPath (fill);
                g.setColour (bor::accent);                   g.strokePath (outLine, juce::PathStrokeType (1.5f));
            }
        }

        // header: title + colour legend
        g.setFont (bor::mono (9.0f));
        auto hdr = b.toNearestInt().reduced (5, 4);
        g.setColour (bor::mute2);
        g.drawText (on ? juce::String::fromUTF8 ("SPECTRUM  \xC2\xB7  Hz") : juce::String ("SPECTRUM // OFF"),
                    hdr, juce::Justification::topLeft);
        if (on)
        {
            auto leg = hdr.removeFromTop (10);
            leg.removeFromLeft (110);
            if (view != 2)
            { g.setColour (bor::mute2);  g.drawText (juce::String::fromUTF8 ("\xE2\x80\x94 DI"),  leg.removeFromLeft (38), juce::Justification::topLeft); }
            if (view != 1)
            { g.setColour (bor::accent); g.drawText (juce::String::fromUTF8 ("\xE2\x80\x94 OUT"), leg.removeFromLeft (44), juce::Justification::topLeft); }
        }
        g.setColour (bor::mute2);
        g.drawText ("dB", juce::Rectangle<int> ((int) inner.getRight() - 26, (int) inner.getY() + 3, 22, 10),
                    juce::Justification::topRight);
    }

    juce::dsp::FFT fft;
    juce::dsp::WindowingFunction<float> window;
    juce::HeapBlock<float> fftData { (size_t) (2 * fftSize), true };
    juce::HeapBlock<float> tmp     { (size_t) fftSize, true };
    std::array<Trace, 2> traces;   // [0] processed output, [1] raw DI
    bool on = true;
    int  view = 0;                 // 0 = ALL, 1 = PRE (DI only), 2 = POST (out only)
};

// ---- SYS overlay: live engine/host stats (CPU, latency, buffer, format) -----
// Answers the "what is the CPU load / latency / oversampling" questions in-product.
struct InfoPanel : public juce::Component
{
    InfoPanel()
    {
        setInterceptsMouseClicks (true, true);   // children (COPY) must get clicks
        copyBtn = std::make_unique<SquareButton> ("COPY", bor::accent, bor::accentOn);
        copyBtn->setClickingTogglesState (false);
        copyBtn->setTooltip ("Copy these stats to the clipboard (for bug reports)");
        copyBtn->onClick = [this]
        {
            juce::StringArray out { "Bass Better-er SYS" };
            for (const auto& l : lines)
                out.add (l.upToFirstOccurrenceOf ("|", false, false).paddedRight (' ', 13)
                         + l.fromFirstOccurrenceOf ("|", false, false));
            juce::SystemClipboard::copyTextToClipboard (out.joinIntoString ("\n"));
            copyBtn->setButtonText ("COPIED");
            juce::Timer::callAfterDelay (900, [sp = juce::Component::SafePointer<SquareButton> (copyBtn.get())]
                                              { if (sp != nullptr) sp->setButtonText ("COPY"); });
        };
        addAndMakeVisible (*copyBtn);
    }

    void resized() override { copyBtn->setBounds (getWidth() - 16 - 64, 11, 64, 20); }

    // each line is "KEY|value"
    void setLines (juce::StringArray l) { if (l != lines) { lines = std::move (l); repaint(); } }
    void mouseDown (const juce::MouseEvent&) override { if (onDismiss) onDismiss(); }

    void paint (juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat();
        g.setColour (bor::ink.withAlpha (0.97f)); g.fillRect (b);
        g.setColour (bor::accent);                g.drawRect (b, 1.0f);
        auto r = getLocalBounds().reduced (16, 12);
        g.setColour (bor::bone);
        g.setFont (bor::mono (11.0f, true));
        g.drawText ("SYS // ENGINE", r.removeFromTop (18), juce::Justification::topLeft);
        r.removeFromTop (4);
        g.setFont (bor::mono (11.0f));
        for (const auto& l : lines)
        {
            auto row = r.removeFromTop (16);
            g.setColour (bor::mute2);
            g.drawText (l.upToFirstOccurrenceOf ("|", false, false), row.removeFromLeft (110),
                        juce::Justification::centredLeft);
            g.setColour (bor::bone);
            g.drawText (l.fromFirstOccurrenceOf ("|", false, false), row, juce::Justification::centredLeft);
        }
    }

    std::function<void()> onDismiss;
    juce::StringArray lines;
    std::unique_ptr<SquareButton> copyBtn;
};

static void drawRegMark (juce::Graphics& g, int x, int y)
{
    juce::Rectangle<float> r ((float) x, (float) y, 12.0f, 12.0f);
    g.setColour (bor::mute.withAlpha (0.5f));
    g.drawEllipse (r, 1.0f);
    g.drawLine (r.getCentreX(), r.getY(), r.getCentreX(), r.getBottom(), 1.0f);
    g.drawLine (r.getX(), r.getCentreY(), r.getRight(), r.getCentreY(), 1.0f);
}
} // namespace bbe

// ============================================================================
// Content — the fixed 1180×784 panel with every control. The editor scales it.
// ============================================================================
struct BoRBassEnhancerEditor::Content : public juce::Component, private juce::Timer
{
    Content (BoRBassEnhancerProcessor& p) : proc (p)
    {
        logo = std::make_unique<bbe::HexLogo>();
        addAndMakeVisible (*logo);

        auto setupLbl = [this] (juce::Label& l, const juce::String& t, juce::Colour col,
                                float h, bool bold, juce::Justification j)
        {
            l.setText (t, juce::dontSendNotification);
            l.setFont (bor::mono (h, bold));
            l.setColour (juce::Label::textColourId, col);
            l.setJustificationType (j);
            addAndMakeVisible (l);
        };
        setupLbl (wordmark, "BASS BETTER-ER", bor::bone, 22.0f, true, juce::Justification::centredLeft);

        const bool stereo = proc.isStereo();
        setupLbl (modeLbl, stereo ? "STEREO" : "MONO", bor::mute2, 11.0f, false,
                  juce::Justification::centredRight);

        // preset menu
        presetBox.setTextWhenNothingSelected ("PRESET");
        presetBox.setColour (juce::ComboBox::backgroundColourId, bor::ink2);
        presetBox.setColour (juce::ComboBox::textColourId, bor::bone);
        presetBox.setColour (juce::ComboBox::outlineColourId, bor::rule);
        presetBox.setColour (juce::ComboBox::arrowColourId, bor::mute2);
        addAndMakeVisible (presetBox);
        rebuildPresetMenu();
        presetBox.onChange = [this] { presetChosen(); };

        // DI blend strip first (no pan — A/B audition sits there instead), then the
        // 8 voicing layers. SUB has no pan either: the lows stay dead centre.
        strips.add (new bbe::Strip (proc, "di", "DI", false, false, true, faderLnf, knobLnf));
        for (int c = 0; c < BoRBassEnhancerProcessor::NUM_CH; ++c)
        {
            const auto& ch = BoRBassEnhancerProcessor::channels[(size_t) c];
            strips.add (new bbe::Strip (proc, ch.id, ch.name, ch.isFX,
                                        stereo && c != 0, false, faderLnf, knobLnf));
        }
        for (auto* s : strips) addAndMakeVisible (s);

        // A/B audition — a listening tool, deliberately not a saved parameter
        strips[0]->ab->setToggleState (proc.getDiReference(), juce::dontSendNotification);
        strips[0]->ab->onClick = [this] { proc.setDiReference (strips[0]->ab->getToggleState()); };

        analyzer = std::make_unique<bbe::Analyzer>();
        addAndMakeVisible (*analyzer);

        // FREQ cycles the spectrum display: OFF -> ALL -> PRE (DI) -> POST (plugin).
        // OFF drops the analyzer feed (the CPU saver); the view mode persists with
        // the session as a non-automatable state property.
        freq = std::make_unique<bbe::SquareButton> ("FREQ", bor::accent, bor::accentOn);
        freq->setTooltip ("Spectrum display: click to cycle OFF / ALL / PRE (DI) / POST");
        freq->setClickingTogglesState (false);
        freq->onClick = [this] { cycleFreqMode(); };
        addAndMakeVisible (*freq);
        refreshFreqButton();

        // SYS — engine stats overlay
        sys = std::make_unique<bbe::SquareButton> ("SYS", bor::accent, bor::accentOn);
        sys->setTooltip ("Engine stats: CPU, latency, buffer, format");
        sys->onClick = [this] { info->setVisible (sys->getToggleState()); info->toFront (false); };
        addAndMakeVisible (*sys);
        info = std::make_unique<bbe::InfoPanel>();
        info->onDismiss = [this] { sys->setToggleState (false, juce::dontSendNotification);
                                   info->setVisible (false); };
        addChildComponent (*info);

        // update notice — populated by the (at most daily) release check
        updateLink.setColour (juce::HyperlinkButton::textColourId, bor::amber);
        updateLink.setFont (bor::mono (11.0f, true), false, juce::Justification::centredRight);
        addChildComponent (updateLink);
        startUpdateCheck();

        inKnob   = std::make_unique<bbe::Knob> (proc, "in_gain",  "INPUT",  1, knobLnf);
        glueKnob = std::make_unique<bbe::Knob> (proc, "glue",     "GLUE",   2, knobLnf);
        outKnob  = std::make_unique<bbe::Knob> (proc, "out_gain", "OUTPUT", 1, knobLnf);
        addAndMakeVisible (*inKnob); addAndMakeVisible (*glueKnob); addAndMakeVisible (*outKnob);


        setSize (1180, 784);
        startTimerHz (30);
    }

    ~Content() override
    {
        stopTimer();
        // join the update check before teardown (worst case ~4 s on its first run,
        // and only within the brief once-a-day check window)
        if (updateThread != nullptr) updateThread->stopThread (5000);
        // The A/B audition ends with the editor: leaving it engaged would keep the
        // plugin silently passing raw DI with no visible indication in the host.
        proc.setDiReference (false);
    }

    void timerCallback() override
    {
        auto smooth = [] (float& cur, float lin)
        {
            const float db   = juce::Decibels::gainToDecibels (lin, -60.0f);
            const float norm = juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 60.0f);
            cur = norm > cur ? norm : cur * 0.80f + norm * 0.20f;
            return cur;
        };
        strips[0]->setLevel (smooth (meterLevel[0], proc.getDiLevel()));
        for (int c = 0; c < BoRBassEnhancerProcessor::NUM_CH; ++c)
            strips[c + 1]->setLevel (smooth (meterLevel[(size_t) (c + 1)], proc.getChannelLevel (c)));
        analyzer->update (proc);

        refreshFreqButton();   // host automation can flip the analyzer param under us

        if (info->isVisible())
        {
            const float load = (float) proc.getCpuLoad();
            cpuDisp = load > cpuDisp ? load : cpuDisp * 0.85f + load * 0.15f;
            info->setLines ({
                "VERSION|"     + juce::String (JucePlugin_VersionString),
                "FORMAT|"      + juce::String (juce::AudioProcessor::getWrapperTypeDescription (proc.wrapperType)),
                "HOST|"        + juce::String (juce::PluginHostType().getHostDescription()),
                "OS|"          + juce::SystemStats::getOperatingSystemName(),
                "OUTPUT|"      + juce::String (proc.isStereo() ? "STEREO" : "MONO"),
                "SAMPLE RATE|" + juce::String (proc.getSampleRate(), 0) + " Hz",
                "BLOCK|"       + juce::String (proc.getLastBlockSize()) + " smp (host)",
                "LATENCY|"     + juce::String (proc.getLatencySamples()) + " smp (zero-latency convolution)",
                "FUZZ OS|4x, +" + juce::String (proc.getFuzzOsLatency(), 2) + " smp in the fuzz path",
                "CPU|"         + juce::String (cpuDisp * 100.0f, 1) + " % of one core",
                "XRUNS|"       + juce::String (proc.getXRunCount()) + " (callbacks over budget)",
            });
        }
    }

    // ---- FREQ display cycle: OFF -> ALL -> PRE (DI only) -> POST (plugin only) ----
    juce::String freqView() const
    { return proc.apvts.state.getProperty ("freqView", "all").toString(); }

    void cycleFreqMode()
    {
        auto* p = proc.apvts.getParameter ("analyzer");
        const auto v = freqView();
        auto setAnalyzer = [p] (float val)   // gesture-bracketed: hosts in touch/latch
        {                                    // automation write need to see the gesture
            p->beginChangeGesture();
            p->setValueNotifyingHost (val);
            p->endChangeGesture();
        };
        if (! proc.analyzerEnabled())
        { setAnalyzer (1.0f); proc.apvts.state.setProperty ("freqView", "all", nullptr); }
        else if (v == "all") proc.apvts.state.setProperty ("freqView", "pre",  nullptr);
        else if (v == "pre") proc.apvts.state.setProperty ("freqView", "post", nullptr);
        else                 setAnalyzer (0.0f);
        refreshFreqButton();
    }

    void refreshFreqButton()
    {
        const bool on = proc.analyzerEnabled();
        const auto v  = freqView();
        freq->setToggleState (on, juce::dontSendNotification);
        freq->setButtonText (! on ? "FREQ:OFF" : v == "pre" ? "FREQ:PRE" : v == "post" ? "FREQ:POST" : "FREQ:ALL");
        analyzer->view = v == "pre" ? 1 : v == "post" ? 2 : 0;
    }

    // ---- update notice: GitHub latest-release tag, checked at most once a day ----
    static std::array<int, 3> parseVer (juce::String s)
    {
        s = s.trim().trimCharactersAtStart ("vV");
        const auto t = juce::StringArray::fromTokens (s, ".", "");
        return { t.size() > 0 ? t[0].getIntValue() : 0,
                 t.size() > 1 ? t[1].getIntValue() : 0,
                 t.size() > 2 ? t[2].getIntValue() : 0 };
    }

    void maybeShowUpdate (const juce::String& tag)
    {
        if (tag.isEmpty() || ! (parseVer (JucePlugin_VersionString) < parseVer (tag))) return;
        updateLink.setButtonText (juce::String::fromUTF8 ("UPDATE \xE2\x86\x92 ") + tag);
        updateLink.setVisible (true);
    }

    // Owned, joinable thread (NOT Thread::launch): a detached thread could outlive
    // the last plugin instance and execute unmapped code when the host unloads the
    // bundle. The Content destructor joins it; the cache-file I/O stays off the
    // message thread.
    struct UpdateCheckThread : public juce::Thread
    {
        UpdateCheckThread (Content& c, juce::File f, juce::String cached)
            : juce::Thread ("BBE update check"), owner (c), file (std::move (f)),
              cachedTag (std::move (cached)) {}

        void run() override
        {
            juce::String tag;
            juce::URL url ("https://api.github.com/repos/boxofrules/bass-betterer/releases/latest");
            if (auto stream = url.createInputStream (
                    juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                        .withConnectionTimeoutMs (4000)))
                tag = juce::JSON::parse (stream->readEntireStreamAsString())
                          .getProperty ("tag_name", {}).toString();

            juce::XmlElement xml ("UPDATE");
            xml.setAttribute ("lastCheckMs", juce::String (juce::Time::currentTimeMillis()));
            xml.setAttribute ("latestTag", tag.isNotEmpty() ? tag : cachedTag);
            xml.writeTo (file, {});

            if (threadShouldExit() || tag.isEmpty()) return;
            juce::MessageManager::callAsync (
                [safe = juce::Component::SafePointer<Content> (&owner), tag]
                { if (safe != nullptr) safe->maybeShowUpdate (tag); });
        }

        Content& owner;
        juce::File file;
        juce::String cachedTag;
    };

    void startUpdateCheck()
    {
        const auto f = proc.getUserPresetDir().getParentDirectory().getChildFile ("update_check.xml");
        juce::String cachedTag;
        juce::int64 last = 0;
        if (auto xml = juce::XmlDocument::parse (f))
        {
            cachedTag = xml->getStringAttribute ("latestTag");
            last = xml->getStringAttribute ("lastCheckMs").getLargeIntValue();
        }
        maybeShowUpdate (cachedTag);
        if (juce::Time::currentTimeMillis() - last < 24LL * 3600 * 1000) return;

        updateThread = std::make_unique<UpdateCheckThread> (*this, f, cachedTag);
        updateThread->startThread();
    }

    void rebuildPresetMenu()
    {
        presetBox.clear (juce::dontSendNotification);
        auto fac = proc.getFactoryPresetNames();
        factoryCount = fac.size();
        presetBox.addSectionHeading ("Factory");
        for (int i = 0; i < fac.size(); ++i) presetBox.addItem (fac[i], i + 1);

        userFiles = proc.getUserPresetDir().findChildFiles (juce::File::findFiles, false, "*.xml");
        if (! userFiles.isEmpty())
        {
            presetBox.addSeparator();
            presetBox.addSectionHeading ("User");
            for (int i = 0; i < userFiles.size(); ++i)
                presetBox.addItem (userFiles[i].getFileNameWithoutExtension(), 100 + i);
        }
        presetBox.addSeparator();
        presetBox.addItem (juce::String::fromUTF8 ("Save current\xE2\x80\xA6"), 1000);
    }

    void presetChosen()
    {
        const int sel = presetBox.getSelectedId();
        if (sel >= 1 && sel <= factoryCount)
        {
            proc.loadFactoryPreset (sel - 1);
        }
        else if (sel >= 100 && sel < 100 + userFiles.size())
        {
            proc.loadUserPresetFile (userFiles[sel - 100]);
        }
        else if (sel == 1000)
        {
            presetBox.setSelectedId (0, juce::dontSendNotification);
            saveWin = std::make_unique<juce::AlertWindow> ("Save preset", "Preset name:",
                                                           juce::MessageBoxIconType::NoIcon);
            saveWin->addTextEditor ("name", "My Preset");
            saveWin->addButton ("Save",   1, juce::KeyPress (juce::KeyPress::returnKey));
            saveWin->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
            saveWin->enterModalState (true, juce::ModalCallbackFunction::create ([this] (int r)
            {
                if (r == 1)
                {
                    const auto nm = saveWin->getTextEditorContents ("name").trim();
                    if (nm.isNotEmpty()) { proc.saveUserPreset (nm); rebuildPresetMenu(); }
                }
                saveWin.reset();
            }), false);
        }
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (bor::ink);
        g.setColour (bor::accent.withAlpha (0.035f));
        for (int x = 0; x < getWidth();  x += 32) g.drawVerticalLine  (x, 0.0f, (float) getHeight());
        for (int y = 0; y < getHeight(); y += 32) g.drawHorizontalLine (y, 0.0f, (float) getWidth());

        g.setColour (bor::rule);
        g.drawRect (getLocalBounds(), 1);
        g.drawHorizontalLine (hdrRuleY,  28.0f, (float) getWidth() - 28.0f);
        g.drawHorizontalLine (botRuleY,  28.0f, (float) getWidth() - 28.0f);
        g.drawHorizontalLine (footRuleY, 0.0f,  (float) getWidth());

        bbe::drawRegMark (g, 8, 8);
        bbe::drawRegMark (g, getWidth() - 20, 8);
        bbe::drawRegMark (g, 8, getHeight() - 20);
        bbe::drawRegMark (g, getWidth() - 20, getHeight() - 20);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (1);
        const int padX = 28;

        auto header = r.removeFromTop (77);
        const int cy = header.getCentreY();
        logo->setBounds (padX, cy - 17, 34, 34);
        wordmark.setBounds (padX + 34 + 14, cy - 13, 300, 26);
        modeLbl.setBounds  (header.getRight() - padX - 70, cy - 8, 70, 16);
        presetBox.setBounds (header.getRight() - padX - 70 - 12 - 220, cy - 13, 220, 26);
        sys->setBounds (presetBox.getX() - 12 - 44, cy - 13, 44, 26);
        updateLink.setBounds (sys->getX() - 12 - 170, cy - 13, 170, 26);
        info->setBounds (getLocalBounds().withSizeKeepingCentre (430, 262));

        hdrRuleY = r.getY();
        r.removeFromTop (1);

        auto section = r.removeFromTop (516).withTrimmedLeft (padX).withTrimmedRight (padX);
        section.removeFromTop (20); section.removeFromBottom (22);
        const int n = strips.size(), gap = 10;
        const int sw = (section.getWidth() - gap * (n - 1)) / n;
        for (int i = 0; i < n; ++i)
        {
            auto col = section.removeFromLeft (i == n - 1 ? section.getWidth() : sw);
            strips[i]->setBounds (col);
            if (i < n - 1) section.removeFromLeft (gap);
        }

        botRuleY = r.getY();
        r.removeFromTop (1);

        auto bottom = r.removeFromTop (149).withTrimmedLeft (padX).withTrimmedRight (padX);
        const int kw = 84, kh = 110, kgap = 36;
        const int ky = bottom.getCentreY() - kh / 2;
        int kx = bottom.getRight() - kw;
        outKnob->setBounds  (kx, ky, kw, kh); kx -= kw + kgap;
        glueKnob->setBounds (kx, ky, kw, kh); kx -= kw + kgap;
        inKnob->setBounds   (kx, ky, kw, kh);

        // spectrum analyzer fills the left, with the FREQ mode cycler above it
        auto left = bottom.withTrimmedRight (bottom.getRight() - (kx - 28));
        freq->setBounds (left.removeFromTop (24).removeFromLeft (92));
        left.removeFromTop (6);
        analyzer->setBounds (left.removeFromTop (96));

        footRuleY = r.getY();
    }

    BoRBassEnhancerProcessor& proc;
    bbe::FaderLnf faderLnf;
    bbe::KnobLnf  knobLnf;
    juce::TooltipWindow tooltip { this };

    std::unique_ptr<bbe::HexLogo> logo;
    juce::Label wordmark, modeLbl;
    juce::ComboBox presetBox;
    int factoryCount = 0;
    juce::Array<juce::File> userFiles;
    std::unique_ptr<juce::AlertWindow> saveWin;
    juce::OwnedArray<bbe::Strip> strips;
    std::unique_ptr<bbe::Analyzer> analyzer;
    std::unique_ptr<bbe::SquareButton> freq, sys;
    std::unique_ptr<bbe::InfoPanel> info;
    std::unique_ptr<bbe::Knob> inKnob, glueKnob, outKnob;
    juce::HyperlinkButton updateLink { {}, juce::URL ("https://github.com/boxofrules/bass-betterer/releases/latest") };
    std::unique_ptr<UpdateCheckThread> updateThread;
    float cpuDisp = 0.0f;

    std::array<float, BoRBassEnhancerProcessor::NUM_CH + 1> meterLevel {};
    int hdrRuleY = 0, botRuleY = 0, footRuleY = 0;
};

// ============================================================================
// Editor — resizable, aspect-locked wrapper that scales the panel (true zoom).
// ============================================================================
BoRBassEnhancerEditor::BoRBassEnhancerEditor (BoRBassEnhancerProcessor& p)
    : AudioProcessorEditor (&p), proc (p)
{
    setOpaque (true);
    editorLnf = std::make_unique<bbe::EditorLnf>();
    setLookAndFeel (editorLnf.get());

    content = std::make_unique<Content> (proc);
    addAndMakeVisible (*content);

    // Canonical JUCE resizable-plugin setup: enable resizing + a corner resizer (so it
    // works across hosts incl. Logic), configure the editor's built-in constrainer
    // (limits + aspect lock), and hide the grip via EditorLnf so it looks like a plain
    // window corner. The panel scales to fit.
    setResizable (true, true);
    setResizeLimits (649, 431, 1534, 1019);              // ~0.55x .. ~1.3x of the design
    if (auto* c = getConstrainer()) c->setFixedAspectRatio (1180.0 / 784.0);
    setSize (1003, 666);                                 // ~0.85x — comfortable default
}

BoRBassEnhancerEditor::~BoRBassEnhancerEditor() { setLookAndFeel (nullptr); }

void BoRBassEnhancerEditor::paint (juce::Graphics& g) { g.fillAll (bor::ink); }

void BoRBassEnhancerEditor::resized()
{
    const float dw = 1180.0f, dh = 784.0f;
    const float s  = juce::jmin ((float) getWidth() / dw, (float) getHeight() / dh);
    const float x  = ((float) getWidth()  - dw * s) * 0.5f;
    const float y  = ((float) getHeight() - dh * s) * 0.5f;
    content->setBounds (0, 0, (int) dw, (int) dh);
    content->setTransform (juce::AffineTransform::scale (s).translated (x, y));
}
