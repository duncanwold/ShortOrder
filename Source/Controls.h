#pragma once

// v0.6: no longer the editor's controls (the faceplate's modules are). It survives as the plain strip under the faceplate for the
// controls a show flag adds (ADVANCED): the editor passes the faceplate's own parameters
// as `exclude`, so only those appear, and the strip is there only when one does.
// v0.5.10: the controls, one column per parameter group (Note Detection, Note Shape, Voice, Chord; ADVANCED and Output while
// their flags show them), in place of the generic editor's one-control-per-row list. It walks the parameter tree, so a control a
// show flag adds to the layout appears in its group's column. Each control is attached to its parameter (APVTS
// attachments), so the parameters themselves, the host's own controls view, automation and the host bypass are unchanged.

#include <juce_audio_processors/juce_audio_processors.h>
#include "Params.h"

class ControlColumns : public juce::Component
{
public:
    explicit ControlColumns (juce::AudioProcessorValueTreeState&, const ShowFlags& show = {}, const juce::StringArray& exclude = {});
    ~ControlColumns() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    // The height the columns need (the tallest column), for the editor's layout.
    int preferredHeight() const;

    // For the snapshot tool: the columns (group IDs and titles, left to right), the controls (parameter IDs) in each.
    int columnCount() const { return (int) columns.size(); }
    int controlCount() const;
    juce::String columnGroup (int column) const;
    juce::String columnTitle (int column) const;
    juce::StringArray controlsIn (int column) const;
    juce::String groupOf (const juce::String& parameterID) const; // the column's group ID, or empty if not shown
    juce::Rectangle<int> controlBounds (const juce::String& parameterID) const; // its label and widget, in this component

    static constexpr int kTitleHeight = 22, kLabelHeight = 17, kWidgetHeight = 26, kControlGap = 10, kPadding = 8;

private:
    struct Control;
    struct Column;
    std::vector<std::unique_ptr<Column>> columns;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ControlColumns)
};
