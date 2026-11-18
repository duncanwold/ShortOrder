#include "Controls.h"

namespace
{
const juce::Colour kBlack (0xff000000);
const juce::Colour kMidGrey (0xffb0b0b0);
const juce::Colour kDarkGrey (0xff505050);

// What the editor never shows: the host's bypass (v0.5.9), the ADVANCED group while it's hidden.
bool hiddenParameter (const juce::String& id) { return id == ParamIDs::bypass; }
bool hiddenGroup (const juce::String& id, const ShowFlags& show) { return id == "advanced" && ! show.advanced; }
} // namespace

struct ControlColumns::Control
{
    juce::String id;
    juce::Label label;
    std::unique_ptr<juce::Slider> slider;
    std::unique_ptr<juce::ComboBox> combo;
    std::unique_ptr<juce::ToggleButton> toggle;
    // After the widgets, so they're destroyed first.
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> sliderAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> comboAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> toggleAttachment;

    juce::Component* widget() const
    {
        return slider != nullptr ? static_cast<juce::Component*> (slider.get()) : combo != nullptr ? static_cast<juce::Component*> (combo.get()) : toggle.get();
    }
};

struct ControlColumns::Column
{
    juce::String group;
    juce::Label title;
    std::vector<std::unique_ptr<Control>> controls;
};

ControlColumns::ControlColumns (juce::AudioProcessorValueTreeState& apvts, const ShowFlags& show, const juce::StringArray& exclude)
{
    for (auto* node : apvts.processor.getParameterTree())
    {
        const auto* group = node->getGroup();
        if (group == nullptr || hiddenGroup (group->getID(), show))
            continue;
        auto column = std::make_unique<Column>();
        column->group = group->getID();
        for (auto* param : group->getParameters (true))
        {
            auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (param);
            if (ranged == nullptr || hiddenParameter (ranged->getParameterID()) || exclude.contains (ranged->getParameterID()))
                continue;
            auto c = std::make_unique<Control>();
            c->id = ranged->getParameterID();
            if (auto* choice = dynamic_cast<juce::AudioParameterChoice*> (ranged))
            {
                c->combo = std::make_unique<juce::ComboBox>();
                c->combo->addItemList (choice->choices, 1);
                c->comboAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (apvts, c->id, *c->combo);
            }
            else if (dynamic_cast<juce::AudioParameterBool*> (ranged) != nullptr)
            {
                c->toggle = std::make_unique<juce::ToggleButton> ("On");
                c->toggleAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (apvts, c->id, *c->toggle);
            }
            else
            {
                c->slider = std::make_unique<juce::Slider> (juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight);
                c->slider->setTextBoxStyle (juce::Slider::TextBoxRight, false, 72, kWidgetHeight - 4);
                const auto unit = ranged->getLabel();
                if (unit.isNotEmpty())
                    c->slider->setTextValueSuffix (" " + unit);
                c->sliderAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (apvts, c->id, *c->slider);
            }
            c->label.setText (ranged->getName (64), juce::dontSendNotification);
            c->label.setFont (juce::FontOptions ("Menlo", 12.0f, juce::Font::plain));
            c->label.setColour (juce::Label::textColourId, kBlack);
            c->label.setJustificationType (juce::Justification::bottomLeft);
            c->label.setBorderSize ({ 0, 1, 1, 1 });
            addAndMakeVisible (c->label);
            addAndMakeVisible (c->widget());
            column->controls.push_back (std::move (c));
        }
        if (column->controls.empty())
            continue; // Output with only the (host's) Bypass in it; (v0.6) a group whose controls are all on the faceplate
        column->title.setText (group->getName(), juce::dontSendNotification);
        column->title.setFont (juce::FontOptions ("Menlo", 14.0f, juce::Font::bold));
        column->title.setColour (juce::Label::textColourId, kBlack);
        column->title.setJustificationType (juce::Justification::centredLeft);
        column->title.setBorderSize ({ 0, 1, 0, 1 });
        addAndMakeVisible (column->title);
        columns.push_back (std::move (column));
    }
}

ControlColumns::~ControlColumns() = default;

int ControlColumns::preferredHeight() const
{
    size_t most = 0;
    for (auto& c : columns)
        most = std::max (most, c->controls.size());
    return kPadding + kTitleHeight + (int) most * (kLabelHeight + kWidgetHeight + kControlGap) + kPadding - kControlGap;
}

void ControlColumns::resized()
{
    if (columns.empty())
        return;
    auto area = getLocalBounds().reduced (0, kPadding);
    const float w = (float) area.getWidth() / (float) columns.size();
    for (size_t k = 0; k < columns.size(); ++k)
    {
        auto& col = *columns[k];
        auto r = juce::Rectangle<int> (area.getX() + juce::roundToInt (w * (float) k), area.getY(), juce::roundToInt (w), area.getHeight()).reduced (10, 0);
        col.title.setBounds (r.removeFromTop (kTitleHeight));
        for (auto& c : col.controls)
        {
            c->label.setBounds (r.removeFromTop (kLabelHeight));
            c->widget()->setBounds (r.removeFromTop (kWidgetHeight));
            r.removeFromTop (kControlGap);
        }
    }
}

void ControlColumns::paint (juce::Graphics& g)
{
    // A thin rule between the columns, and under the titles.
    g.setColour (kMidGrey);
    const float w = (float) getWidth() / (float) std::max<size_t> (1, columns.size());
    for (size_t k = 1; k < columns.size(); ++k)
        g.drawVerticalLine (juce::roundToInt (w * (float) k), (float) kPadding, (float) (getHeight() - kPadding));
    g.setColour (kDarkGrey.withAlpha (0.35f));
    for (auto& c : columns)
        g.drawHorizontalLine (c->title.getBottom() - 1, (float) c->title.getX(), (float) c->title.getRight());
}

int ControlColumns::controlCount() const
{
    int n = 0;
    for (auto& c : columns)
        n += (int) c->controls.size();
    return n;
}

juce::String ControlColumns::columnGroup (int column) const { return juce::isPositiveAndBelow (column, columns.size()) ? columns[(size_t) column]->group : juce::String(); }

juce::String ControlColumns::columnTitle (int column) const
{
    return juce::isPositiveAndBelow (column, columns.size()) ? columns[(size_t) column]->title.getText() : juce::String();
}

juce::StringArray ControlColumns::controlsIn (int column) const
{
    juce::StringArray ids;
    if (juce::isPositiveAndBelow (column, columns.size()))
        for (auto& c : columns[(size_t) column]->controls)
            ids.add (c->id);
    return ids;
}

juce::String ControlColumns::groupOf (const juce::String& parameterID) const
{
    for (auto& col : columns)
        for (auto& c : col->controls)
            if (c->id == parameterID)
                return col->group;
    return {};
}

juce::Rectangle<int> ControlColumns::controlBounds (const juce::String& parameterID) const
{
    for (auto& col : columns)
        for (auto& c : col->controls)
            if (c->id == parameterID)
                return c->label.getBounds().getUnion (c->widget()->getBounds());
    return {};
}
