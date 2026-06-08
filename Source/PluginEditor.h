#pragma once
#include "PluginProcessor.h"

// Bass Better-er editor — pure Box of Rules panel (signal-cyan accent), built to the
// Claude Design handoff. The panel is a fixed 1180×784 design; the editor wraps it in
// a resizable, aspect-locked container that scales the whole UI (true zoom).
class BoRBassEnhancerEditor : public juce::AudioProcessorEditor
{
public:
    explicit BoRBassEnhancerEditor (BoRBassEnhancerProcessor&);
    ~BoRBassEnhancerEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    struct Content;   // the fixed-size panel + all controls (defined in the .cpp)

    BoRBassEnhancerProcessor& proc;
    std::unique_ptr<Content> content;
    std::unique_ptr<juce::LookAndFeel_V4> editorLnf;   // hides the corner-resizer grip

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BoRBassEnhancerEditor)
};
