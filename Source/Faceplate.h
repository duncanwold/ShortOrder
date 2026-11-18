#pragma once

// v0.6: the ShortOrder faceplate's modules (docs/history/SPEC-v0.6.md sections 2-5): the header with its status LED, the hero row (set your
// latency: the big detented knob and the ms readout; the LED neck), and the bottom row (note shape, voice, chord with its now box).
// Each is attached to its parameters (juce::ParameterAttachment and friends), so automation, the host's own controls view and
// sessions are unchanged; the editor (PluginEditor) lays them out and feeds them the audio thread's readout at 30 fps. The pure
// rules (the status, the now box, each LED) are static functions, so the snapshot tool checks them without drawing.
// Message thread only.

#include <juce_audio_processors/juce_audio_processors.h>
#include "Rack.h"
#include "Tab.h"
#include "DSP/Engine.h"

// ---------------------------------------------------------------------------
// Header (section 2): shortorder / the 8ms guitar synthesizer, and the status LED in place of readout line 1.
// ---------------------------------------------------------------------------
struct HeaderStatus
{
    enum class Led
    {
        Off,
        Green,
        Orange
    };
    Led led = Led::Off;
    juce::String text = "listening";
    bool operator== (const HeaderStatus& o) const { return led == o.led && text == o.text; }
};

class HeaderBar : public juce::Component
{
public:
    HeaderBar() { setOpaque (true); } // it fills its bounds with the panel colour, so nothing behind it repaints with it

    // Line 1's states, read as line 1 reads them (GTTEditor::formatPitchLine): ABOVE BOARD (orange, `above board D6`); BELOW RANGE or
    // TOO LOW (orange, `too low – try octave`); a note sounding (green, `tracking E4`, the played note); otherwise off, `listening`.
    // sounding: pitch detected, or the note held through Note Release while the voice is open; played: the played note (as quantized).
    static HeaderStatus statusFor (bool sounding, int played, const gtt::NoteDisplay&);

    void setStatus (const HeaderStatus&);
    const HeaderStatus& status() const { return current; }
    void paint (juce::Graphics&) override;

    static constexpr float kTitleSize = 26.0f, kStatusSize = 13.0f;
    static constexpr int kStatusWidth = 190; // fixed, right-aligned: the title never moves
    // v0.6.1: the LED at a fixed spot at the strip's right edge (its centre kLedInset in, so its glow stays inside the header), the
    // text right-aligned, ending kLedGap before the dot. statusArea: the strip a status change repaints.
    static constexpr float kLedDiameter = 8.0f, kLedInset = 11.0f, kLedGap = 8.0f;
    juce::Rectangle<int> statusArea() const { return getLocalBounds().removeFromRight (kStatusWidth); }
    juce::Rectangle<float> statusLedBounds() const; // the dot (whatever the text)
    juce::Rectangle<float> statusTextArea() const;   // where the text is right-aligned

private:
    HeaderStatus current;
};

// ---------------------------------------------------------------------------
// Hero, left (section 3): set your latency.
// ---------------------------------------------------------------------------
// The big knob: 15 detents (gtt::latencyDetentHz), 270 degrees, turning right lowers the latency. Ticks up to the pointer in the text
// colour, past it in the control-border colour; the real latency of detents 0, 4, 8, 12 and 14 round the outside. A drag snaps to the
// detents; between detents (a host's automation) the pointer sits at the true position.
//  - Drag: a press on the tick ring or labels (outside the skirt) jumps to the detent under it and follows the mouse round the centre
//    (the mockup's setFromEvent). A press on the skirt or face starts as the motion says: round the knob, the pointer follows the
//    angle; toward or away from the centre (or a press near the hub), a vertical drag, up = right = faster, 12 px a detent.
//  - The wheel: one detent a notch (a trackpad: one per 0.12 of travel), up = right.
//  - Double-click: the default (B3, 8.3 ms).
// Every change is one gesture (begin / end), so hosts record automation properly.
class LatencyKnob : public juce::Component
{
public:
    explicit LatencyKnob (juce::RangedAudioParameter& window, juce::UndoManager* = nullptr);

    void setSampleRate (double fs);
    double hz() const { return value; }
    double position() const; // detents, 0 .. 14 (continuous between them)
    juce::StringArray labelTexts() const; // the latency labels, left to right (detents 0, 4, 8, 12, 14)
    // Accessibility: "latency 8.3 ms, lowest note B3".
    static juce::String describe (double fs, double hz);

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

    static constexpr int kWidth = 200, kHeight = 178; // the mockup's 200 x 200 box; the empty strip under the labels left off
    static constexpr float kCentre = 100.0f;
    static constexpr int kPixelsPerDetent = 12;

private:
    enum class Drag
    {
        None,
        Undecided,
        Angle,
        Vertical
    };
    void show (float v);
    void setDetent (int k, bool asGesture);
    int detentAtAngle (juce::Point<float> p) const;
    juce::RangedAudioParameter& param;
    std::unique_ptr<juce::ParameterAttachment> attachment;
    double value = 0.0, fs = 48000.0;
    Drag drag = Drag::None;
    bool inGesture = false;
    int lastSet = -1;
    double startPosition = 0.0;
    float wheelAccumulator = 0.0f;
};

// The ms box: 170 px wide whatever it shows; the number at 44 px, each figure in a fixed cell (Didact Gothic's figures aren't
// tabular), then " ms" at 20 px in the secondary colour.
class MsReadout : public juce::Component
{
public:
    void setText (const juce::String& ms);
    const juce::String& text() const { return number; }
    void paint (juce::Graphics&) override;
    static constexpr int kWidth = 170, kHeight = 54;

private:
    juce::String number;
};

class LatencyModule : public juce::Component
{
public:
    explicit LatencyModule (juce::RangedAudioParameter& window);
    void update (double fs); // the labels and the readout follow the rate and the parameter
    void paint (juce::Graphics&) override;
    void resized() override;
    LatencyKnob& knob() { return dial; }
    const LatencyKnob& knob() const { return dial; }
    const MsReadout& readout() const { return ms; }
    static constexpr int kWidth = 290;

private:
    LatencyKnob dial;
    MsReadout ms;
    double rate = 0.0, shownHz = -1.0;
};

// ---------------------------------------------------------------------------
// Hero, right (section 4): the playable zone on your guitar neck.
// ---------------------------------------------------------------------------
enum class NeckLed
{
    Playable,   // green, glowing: at or above the lowest reliable note
    TooLow,     // off grey, inset
    You,        // orange, glowing, 14 px: a position of the note being played
    YouBelow,   // an orange ring round an off-grey LED, 14 px: the played note below range (the low-range detector)
    ChordTone   // blue, glowing, 14 px: a Freeze chord tone
};

class NeckView : public juce::Component
{
public:
    // One position's LED. playing: the played note (or -1); belowRange: the note only the low-range detector hears, below range (or
    // -1); chordTones: Freeze's other tones (empty outside Freeze). Orange wins over blue.
    static NeckLed ledFor (int note, int lowestReliable, int playing, int belowRange, const std::vector<int>& chordTones);

    // freezeTones: the chord's other tones, Freeze only (Harmonize shows none on the neck).
    void update (int lowestReliable, int playing, int belowRange, int aboveBoard, const std::vector<int>& freezeTones, bool freezeLegend);
    NeckLed ledAt (int string, int fret) const; // string 0 = low E .. 5 = high e
    bool aboveBoardShown() const { return aboveBoard >= 0; }
    int playingNote() const { return playing; } // the orange note (-1: none)
    juce::Rectangle<float> aboveBoardMarker() const; // where the arrow is drawn (empty: none)
    juce::StringArray legend() const;
    static juce::String hintText();

    void paint (juce::Graphics&) override;

    static constexpr float kCellW = 25.0f, kCellH = 26.0f, kRowGap = 2.0f, kLabelW = 18.0f, kArrowW = 16.0f, kFretRowH = 16.0f;
    // v0.6.1: the component reaches kGlowMargin past the neck's drawing on each side (the editor widens its bounds by it), so the
    // legend's first LED keeps its whole glow; kLedReach: the furthest any LED's glow reaches from its centre (14 px + 9), plus 1.
    static constexpr int kGlowMargin = 12;
    static constexpr float kLedReach = 17.0f;
    NeckView() { setOpaque (true); }
    juce::Rectangle<int> ledArea (int string, int fret) const; // what a change of that LED repaints
    juce::Rectangle<int> legendArea() const;
    juce::Point<float> legendLedCentre (int k) const;

private:
    juce::Rectangle<float> contentArea() const; // the bounds less the glow margin: the mockup's neck
    juce::Rectangle<float> boxArea() const;
    juce::Point<float> cellCentre (int string, int fret) const;
    int lowest = 0, playing = -1, belowRange = -1, aboveBoard = -1;
    std::vector<int> tones;
    bool chordLegend = false;
};

// ---------------------------------------------------------------------------
// The bottom row (section 5).
// ---------------------------------------------------------------------------
class NoteShapeModule : public juce::Component
{
public:
    explicit NoteShapeModule (juce::AudioProcessorValueTreeState&);
    void paint (juce::Graphics&) override;
    void resized() override;
    const SmallKnob& sustainKnob() const { return sustain; }
    const ChoiceRow& retuneRow() const { return retune; }
    const ChoiceRow& releaseRow() const { return release; }

private:
    SmallKnob sustain;
    ChoiceRow retune, release;
};

class VoiceModule : public juce::Component
{
public:
    // Wave's buttons are its parameter's eight choices.
    explicit VoiceModule (juce::AudioProcessorValueTreeState&);
    void paint (juce::Graphics&) override;
    void resized() override;
    const ChoiceRow& waveGrid() const { return waves; }
    const ChoiceRow& octaveGrid() const { return octaves; }
    // The mockup's waveDefs (a 36 x 20 viewBox), by the engine's Wave.
    static juce::Path waveIcon (gtt::Wave);
    static juce::String waveName (gtt::Wave); // lowercase: the accessibility title and tooltip

private:
    ChoiceRow waves, octaves;
    int waveRows = 2;
};

class ChordModule : public juce::Component
{
public:
    explicit ChordModule (juce::AudioProcessorValueTreeState&);

    // The now box's line (section 5's table): Off `—`; Freeze with tones held `C (C E G) · 2 held` (the chord sounding, named by
    // gtt::chordName, and the remembered notes besides the one being played); Freeze with nothing held `3 notes · play to fill`;
    // Harmonize, the chord sounding `G (G D B)`, or with nothing playing the key `C maj / A min`. tones: Readout::chordTones; held:
    // Readout::memoryHeld.
    static juce::String nowText (const gtt::EngineParams&, uint64_t tones, int held);

    // From the editor's timer: the mode (Size live in Freeze, Key in Harmonize; dimmed otherwise), the HELD LED, the now box.
    void update (const gtt::EngineParams&, uint64_t tones, int held);
    void paint (juce::Graphics&) override;
    void resized() override;

    const ChoiceRow& modeRow() const { return modes; }
    const ChoiceRow& sizeRow() const { return sizes; }
    const juce::ComboBox& keyBox() const { return key; }
    ChoiceRow& modeRow() { return modes; }
    ChoiceRow& sizeRow() { return sizes; }
    juce::ComboBox& keyBox() { return key; }
    bool heldLit() const { return modes.button (1).ledLit(); }
    const juce::String& now() const { return nowLine; }
    bool nowDimmed() const { return nowOff; }

private:
    ChoiceRow modes, sizes;
    juce::ComboBox key;
    juce::ComboBoxParameterAttachment keyAttachment;
    juce::String nowLine = juce::String::fromUTF8 ("\xe2\x80\x94");
    bool nowOff = true, sizeLive = false, keyLive = false;
    juce::Rectangle<int> sizeCaption, keyCaption, nowBox;
};

// Module chrome shared by the bottom row: the outline, padding 10 x 12, and the 14 px title with ~1 px tracking.
namespace faceplate
{
constexpr int kPadX = 12, kPadY = 10, kTitleH = 17;
constexpr float kTitleSize = 14.0f, kTracking = 1.0f;
void drawModuleTitle (juce::Graphics&, juce::Rectangle<int> module, const juce::String& title);
juce::String noteName (int midi); // sharps, middle C = C4
} // namespace faceplate
