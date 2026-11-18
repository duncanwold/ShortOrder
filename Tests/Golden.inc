// RC1 (v0.6.1): the golden renders. Every dry take in Recordings/Release Candidate Recordings/ through the engine at a set of
// settings (one change at a time from the defaults), hashed. The manifest (Tests/golden/manifest.json, committed) holds the
// hashes; the renders themselves go to Tests/golden/renders/ (gitignored, to listen to, and to find the first differing sample).
//
//   GTTTests --golden          re-render everything and compare with the manifest (non-zero exit on any mismatch)
//   GTTTests --golden-write    render everything and write the manifest and the renders (on the reference commit only)
//
// The safety net for the 1.0 cleanup: every later commit must reproduce every hash bit for bit. Included by OfflineTests.cpp.

namespace golden
{
// --- SHA-256 (FIPS 180-4), so the manifest's hashes don't depend on a JUCE module ------------------------------------------
struct Sha256
{
    uint32_t h[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    uint8_t block[64] {};
    size_t used = 0;
    uint64_t bits = 0;

    static uint32_t rotr (uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
    void compress()
    {
        static const uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
            0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
            0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
            0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
            0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
            0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
        };
        uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = (uint32_t) block[4 * i] << 24 | (uint32_t) block[4 * i + 1] << 16 | (uint32_t) block[4 * i + 2] << 8 | block[4 * i + 3];
        for (int i = 16; i < 64; ++i)
        {
            const uint32_t s0 = rotr (w[i - 15], 7) ^ rotr (w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = rotr (w[i - 2], 17) ^ rotr (w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i)
        {
            const uint32_t t1 = hh + (rotr (e, 6) ^ rotr (e, 11) ^ rotr (e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            const uint32_t t2 = (rotr (a, 2) ^ rotr (a, 13) ^ rotr (a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    void add (const void* data, size_t n)
    {
        auto* p = static_cast<const uint8_t*> (data);
        bits += 8 * (uint64_t) n;
        while (n > 0)
        {
            const size_t take = std::min (n, 64 - used);
            std::memcpy (block + used, p, take);
            used += take;
            p += take;
            n -= take;
            if (used == 64)
            {
                compress();
                used = 0;
            }
        }
    }
    juce::String hex()
    {
        const uint64_t total = bits;
        const uint8_t pad = 0x80, zero = 0;
        add (&pad, 1);
        while (used != 56)
            add (&zero, 1);
        uint8_t len[8];
        for (int i = 0; i < 8; ++i)
            len[i] = (uint8_t) (total >> (56 - 8 * i));
        add (len, 8);
        juce::String s;
        for (auto v : h)
            s << juce::String::toHexString ((int) v).paddedLeft ('0', 8);
        return s;
    }
};

// The float buffer's bytes, as the machine holds them (little-endian IEEE 754 on every Mac ShortOrder builds for).
juce::String sha256 (const std::vector<float>& x)
{
    Sha256 s;
    s.add (x.data(), x.size() * sizeof (float));
    return s.hex();
}

// --- The takes ---------------------------------------------------------------------------------------------------------------
struct TakeInfo
{
    const char* id;
    const char* file;       // the dry take, in Recordings/Release Candidate Recordings/
    const char* wet;        // Duncan's bounce from Logic with ShortOrder on (local only)
    const char* noted;      // the settings named in the file name
    int harmonizeKey;       // the take's key (0 .. 11 = C .. B, the major key's tonic): Harmonize's, or C maj / A min if none noted
};
const TakeInfo kTakes[] = {
    { "flatpick", "solo note flatpicking dry.aif", "solo note flatpicking wet.aif", "defaults", 0 },
    { "fingerpick", "solo note fingerpicking dry.aif", "solo note fingerpicking wet.aif", "defaults", 0 },
    { "bass", "bass line (played 3 octaves down with saw) dry.aif", "bass line (played 3 octaves down with saw) wet.aif", "Octave -3, Saw", 0 },
    { "below", "playing at and below detection level (default latency) dry.aif", "playing at and below detection level (default latency) wet.aif", "defaults", 0 },
    { "freeze3", "regular chord freeze test (chord freeze 3 notes) dry.aif", "regular chord freeze test (chord freeze 3 notes).aif", "Chord Freeze, Size 3", 0 },
    { "harmonics", "harmonics (chord freeze 4 notes) dry.aif", "harmonics (chord freeze 4 notes) wet.aif", "Chord Freeze, Size 4", 0 },
    { "harmonize", "chord harmonizer test (Set to B major : G# minor) dry.aif", "chord harmonizer test (Set to B major : G# minor) wet.aif", "Chord Harmonize, B maj / G# min", 11 },
};
constexpr int kNumTakes = (int) (sizeof (kTakes) / sizeof (kTakes[0]));

juce::File repoDir()
{
    auto d = juce::File::getCurrentWorkingDirectory();
    if (! d.getChildFile ("Recordings").isDirectory())
        d = outDir.getParentDirectory().getParentDirectory();
    return d;
}
juce::File takesDir() { return repoDir().getChildFile ("Recordings").getChildFile ("Release Candidate Recordings"); }

// One channel of a take (the takes are stereo with identical channels), as float, and its sample rate.
std::vector<float> readChannel (const juce::File& f, double* rate = nullptr)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (f));
    if (reader == nullptr)
        return {};
    juce::AudioBuffer<float> b (1, (int) reader->lengthInSamples);
    reader->read (&b, 0, (int) reader->lengthInSamples, 0, true, false);
    if (rate != nullptr)
        *rate = reader->sampleRate;
    return std::vector<float> (b.getReadPointer (0), b.getReadPointer (0) + b.getNumSamples());
}

const std::vector<float>& dry (int take)
{
    static std::vector<float> loaded[kNumTakes];
    static std::once_flag once[kNumTakes];
    std::call_once (once[take], [take] { loaded[take] = readChannel (takesDir().getChildFile (kTakes[take].file)); });
    return loaded[take];
}

// 48 kHz -> 44.1 kHz (160 / 147): windowed sinc, 64 taps a side, Kaiser (beta 9), cut off at 20 kHz. Deterministic (doubles, one
// order of summation), so the resampled input, and so its renders, reproduce bit for bit.
std::vector<float> resample441 (const std::vector<float>& x)
{
    constexpr double from = 48000.0, to = 44100.0, cutoff = 20000.0, beta = 9.0;
    constexpr int half = 64;
    auto besselI0 = [] (double v) {
        double sum = 1.0, term = 1.0;
        for (int k = 1; k < 40; ++k)
        {
            term *= (v / (2.0 * k)) * (v / (2.0 * k));
            sum += term;
        }
        return sum;
    };
    const double i0b = besselI0 (beta), fc = cutoff / from; // cycles per input sample
    const size_t n = (size_t) std::floor ((double) x.size() * to / from);
    std::vector<float> y (n);
    for (size_t m = 0; m < n; ++m)
    {
        const double t = (double) m * from / to;
        const auto centre = (int64_t) std::floor (t);
        double acc = 0.0;
        for (int64_t k = centre - half + 1; k <= centre + half; ++k)
        {
            if (k < 0 || k >= (int64_t) x.size())
                continue;
            const double d = t - (double) k;
            const double r = d / half;
            if (std::abs (r) >= 1.0)
                continue;
            const double arg = 2.0 * fc * d;
            const double sinc = std::abs (arg) < 1e-12 ? 1.0 : std::sin (M_PI * arg) / (M_PI * arg);
            acc += x[(size_t) k] * 2.0 * fc * sinc * besselI0 (beta * std::sqrt (1.0 - r * r)) / i0b;
        }
        y[m] = (float) acc;
    }
    return y;
}

const std::vector<float>& dry441 (int take)
{
    static std::vector<float> loaded[kNumTakes];
    static std::once_flag once[kNumTakes];
    std::call_once (once[take], [take] { loaded[take] = resample441 (dry (take)); });
    return loaded[take];
}

// --- The settings set ----------------------------------------------------------------------------------------------------------
struct Setting
{
    juce::String name;
    EngineParams p;
};

const char* waveName (Wave w)
{
    switch (w)
    {
        case Wave::Saw: return "Saw";
        case Wave::Square: return "Square";
        case Wave::Pulse25: return "NES Lead";
        case Wave::Triangle: return "Triangle";
        case Wave::Sine: return "Sine";
        case Wave::HalfSine: return "Half-Sine";
        case Wave::Organ: return "Organ";
        case Wave::Folded: return "Folded";
    }
    return "?";
}

// Between two detents: half way from detent 6 to detent 7 (on the knob's own scale).
double betweenDetentHz()
{
    const double lo = (double) latencyDetentHz (6), hi = (double) latencyDetentHz (7);
    return std::sqrt (lo * hi);
}

EngineParams freeze (int n)
{
    EngineParams p;
    p.chordMode = ChordMode::Memory;
    p.memoryNotes = n;
    return p;
}
EngineParams harmonize (int key)
{
    EngineParams p;
    p.chordMode = ChordMode::Stack;
    p.key = key;
    return p;
}

EngineParams notedParams (int take)
{
    EngineParams p;
    const juce::String id = kTakes[take].id;
    if (id == "bass")
        p.octave = Octave::TripleSub;
    else if (id == "freeze3")
        p = freeze (3);
    else if (id == "harmonics")
        p = freeze (4);
    else if (id == "harmonize")
        p = harmonize (11);
    return p;
}

// The settings set, the same for every take but the key: one change at a time from the defaults.
std::vector<Setting> settingsFor (int take)
{
    std::vector<Setting> s;
    s.push_back ({ juce::String ("noted (") + kTakes[take].noted + ")", notedParams (take) });
    s.push_back ({ "defaults", EngineParams {} });
    for (Wave w : { Wave::Saw, Wave::Square, Wave::Pulse25, Wave::Triangle, Wave::Sine, Wave::HalfSine, Wave::Organ, Wave::Folded })
    {
        EngineParams p;
        p.wave = w;
        s.push_back ({ juce::String ("wave ") + waveName (w), p });
    }
    for (auto [o, name] : { std::pair { Octave::Sub, "octave -1" }, { Octave::DoubleSub, "octave -2" }, { Octave::TripleSub, "octave -3" } })
    {
        EngineParams p;
        p.octave = o;
        s.push_back ({ name, p });
    }
    for (auto [hz, name] : { std::pair { (double) latencyDetentHz (0), "latency G3 (detent 0)" },
                             { (double) latencyDetentHz (14), "latency A4 (detent 14)" },
                             { betweenDetentHz(), "latency between detents 6 and 7" } })
    {
        EngineParams p;
        p.windowHz = (float) hz;
        s.push_back ({ juce::String (name) + juce::String::formatted (" %.2f Hz, %.2f ms", hz, latencyMs (kFs, hz)), p });
    }
    for (auto [r, name] : { std::pair { RetuneSpeed::Fast, "retune fast" }, { RetuneSpeed::Slow, "retune slow" } })
    {
        EngineParams p;
        p.retuneSpeed = r;
        s.push_back ({ name, p });
    }
    for (auto [r, name] : { std::pair { NoteRelease::Fast, "release fast" }, { NoteRelease::Long, "release long" } })
    {
        EngineParams p;
        p.holdMs = noteReleaseMsFor (r);
        s.push_back ({ name, p });
    }
    for (float sus : { 0.0f, 100.0f })
    {
        EngineParams p;
        p.sustainPercent = sus;
        s.push_back ({ juce::String::formatted ("sustain %.0f%%", sus), p });
    }
    for (int n : { 2, 3, 4 })
        s.push_back ({ "freeze " + juce::String (n), freeze (n) });
    const int key = kTakes[take].harmonizeKey, other = key == 7 ? 2 : 7; // the take's key, and G (D for a take in G)
    s.push_back ({ "harmonize " + juce::String (keyLabel (key)), harmonize (key) });
    s.push_back ({ "harmonize " + juce::String (keyLabel (other)), harmonize (other) });
    return s;
}

juce::var settingsJson (const EngineParams& p, double fs)
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("windowHz", (double) p.windowHz);
    o->setProperty ("latencyMs", std::round (latencyMs (fs, p.windowHz) * 100.0) / 100.0);
    o->setProperty ("wave", waveName (p.wave));
    o->setProperty ("octave", -(int) p.octave);
    o->setProperty ("sustainPercent", (double) p.sustainPercent);
    o->setProperty ("retune", p.retuneSpeed == RetuneSpeed::Fast ? "fast" : p.retuneSpeed == RetuneSpeed::Medium ? "medium" : "slow");
    o->setProperty ("releaseMs", (double) p.holdMs);
    o->setProperty ("chord", p.chordMode == ChordMode::Off ? "off" : p.chordMode == ChordMode::Memory ? "freeze" : "harmonize");
    if (p.chordMode == ChordMode::Memory)
        o->setProperty ("chordSize", p.memoryNotes);
    if (p.chordMode == ChordMode::Stack)
        o->setProperty ("key", juce::String (keyLabel (p.key)));
    return juce::var (o);
}

// --- Rendering -----------------------------------------------------------------------------------------------------------------
// What the manifest says it was made from (set on each commit that changes the renders).
constexpr const char* kGoldenEngine = "v0.6.2: fixes 1, 2 and 4 (the bottom edge rule; glides that do not freeze; Sustain blends from the level sounding)";
constexpr int kBlock = 64; // the engine's output doesn't depend on the block (checked by --golden-write), but the manifest pins it

struct Job
{
    int take = 0;
    double fs = kFs;
    bool at441 = false;
    Setting setting;
    juce::String file; // the render's wav, in Tests/golden/renders/
};

std::vector<Job> jobs()
{
    std::vector<Job> out;
    for (int t = 0; t < kNumTakes; ++t)
    {
        const juce::String id = kTakes[t].id;
        for (bool at441 : { false, true })
        {
            const double fs = at441 ? 44100.0 : kFs;
            if (at441 && id != "flatpick" && id != "harmonize")
                continue; // the 44.1 kHz resample: two takes, a single-note one and a chord one
            int k = 0;
            for (auto& s : settingsFor (t))
            {
                Job j { t, fs, at441, s, {} };
                j.file = juce::String::formatted ("%s-%d-%02d-", kTakes[t].id, (int) fs, k++)
                         + s.name.replaceCharacters (" /():%,.#", "_________").removeCharacters ("+").replace ("__", "_").trimCharactersAtEnd ("_");
                out.push_back (j);
            }
        }
    }
    return out;
}

std::vector<float> render (const std::vector<float>& in, double fs, const EngineParams& p, int block = kBlock)
{
    Engine e;
    e.prepare (fs, block);
    std::vector<float> out (in.size());
    for (size_t start = 0; start < in.size(); start += (size_t) block)
    {
        const int n = (int) std::min ((size_t) block, in.size() - start);
        e.process (in.data() + start, out.data() + start, n, p);
    }
    return out;
}

const std::vector<float>& inputFor (const Job& j) { return j.at441 ? dry441 (j.take) : dry (j.take); }

juce::File manifestFile() { return repoDir().getChildFile ("Tests").getChildFile ("golden").getChildFile ("manifest.json"); }
juce::File rendersDir() { return repoDir().getChildFile ("Tests").getChildFile ("golden").getChildFile ("renders"); }

void writeFloatWav (const juce::File& file, const std::vector<float>& x, double fs)
{
    file.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream (file.createOutputStream().release());
    if (stream == nullptr)
        return;
    auto options = juce::AudioFormatWriterOptions().withSampleRate (fs).withNumChannels (1).withBitsPerSample (32).withSampleFormat (
        juce::AudioFormatWriterOptions::SampleFormat::floatingPoint);
    auto writer = wav.createWriterFor (stream, options);
    if (writer == nullptr)
        return;
    const float* chans[1] = { x.data() };
    writer->writeFromFloatArrays (chans, 1, (int) x.size());
}

struct Rendered
{
    juce::String hash;
    std::vector<float> out;
};

// Every job, in parallel; identical settings on the same input are rendered once.
std::vector<Rendered> renderAll (const std::vector<Job>& js, bool keep)
{
    // Load (and resample) the inputs first, in parallel.
    sputter::parallelFor (kNumTakes, [] (int t) { dry (t); });
    sputter::parallelFor (kNumTakes, [] (int t) {
        const juce::String id = kTakes[t].id;
        if (id == "flatpick" || id == "harmonize")
            dry441 (t);
    });
    std::vector<Rendered> r (js.size());
    sputter::parallelFor ((int) js.size(), [&] (int i) {
        const auto& j = js[(size_t) i];
        auto out = render (inputFor (j), j.fs, j.setting.p);
        r[(size_t) i].hash = sha256 (out);
        if (keep)
            r[(size_t) i].out = std::move (out);
    });
    return r;
}

int firstDifference (const std::vector<float>& a, const std::vector<float>& b)
{
    const size_t n = std::min (a.size(), b.size());
    for (size_t i = 0; i < n; ++i)
        if (std::memcmp (&a[i], &b[i], sizeof (float)) != 0)
            return (int) i;
    return a.size() == b.size() ? -1 : (int) n;
}

// --golden-write: the manifest and the renders. Also checks that the block size doesn't matter (64 / 1 / 512 / 1024 / 177 on
// one render of each take), so the manifest's 64 is what a host plays at any buffer size.
int write()
{
    const auto t0 = std::chrono::steady_clock::now();
    const auto js = jobs();
    auto r = renderAll (js, true);

    bool blockFree = true;
    sputter::parallelFor (kNumTakes, [&] (int t) {
        const auto& in = dry (t);
        const auto p = notedParams (t);
        const auto ref = render (in, kFs, p, 64);
        for (int b : { 1, 177, 512, 1024 })
            if (firstDifference (ref, render (in, kFs, p, b)) >= 0)
            {
                blockFree = false;
                std::printf ("  %s, noted settings: block %d differs from block 64\n", kTakes[t].id, b);
            }
    });
    std::printf ("  block size: %s\n", blockFree ? "the output at blocks 1, 177, 512 and 1024 = block 64, bit for bit, on every take's noted settings" : "DIFFERS (see above)");

    rendersDir().createDirectory();
    auto* root = new juce::DynamicObject();
    root->setProperty ("about", "ShortOrder RC1 golden renders: the engine's output on the dry takes in Recordings/Release Candidate Recordings/ "
                                "(channel 1), SHA-256 of the float32 output buffer (little-endian). GTTTests --golden compares; renders in "
                                "Tests/golden/renders/ (local).");
    root->setProperty ("engine", kGoldenEngine);
    root->setProperty ("blockSize", kBlock);
    juce::Array<juce::var> takes, renders;
    for (int t = 0; t < kNumTakes; ++t)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("id", kTakes[t].id);
        o->setProperty ("file", kTakes[t].file);
        o->setProperty ("noted", kTakes[t].noted);
        o->setProperty ("samples48k", (int) dry (t).size());
        o->setProperty ("inputSha256", sha256 (dry (t)));
        const juce::String id = kTakes[t].id;
        if (id == "flatpick" || id == "harmonize")
        {
            o->setProperty ("samples44k1", (int) dry441 (t).size());
            o->setProperty ("input44k1Sha256", sha256 (dry441 (t)));
        }
        takes.add (juce::var (o));
    }
    for (size_t i = 0; i < js.size(); ++i)
    {
        const auto& j = js[i];
        auto* o = new juce::DynamicObject();
        o->setProperty ("take", kTakes[j.take].id);
        o->setProperty ("settings", j.setting.name);
        o->setProperty ("params", settingsJson (j.setting.p, j.fs));
        o->setProperty ("sampleRate", (int) j.fs);
        o->setProperty ("samples", (int) r[i].out.size());
        o->setProperty ("sha256", r[i].hash);
        o->setProperty ("render", j.file + ".wav");
        renders.add (juce::var (o));
        writeFloatWav (rendersDir().getChildFile (j.file + ".wav"), r[i].out, j.fs);
    }
    root->setProperty ("takes", takes);
    root->setProperty ("renders", renders);
    manifestFile().getParentDirectory().createDirectory();
    manifestFile().replaceWithText (juce::JSON::toString (juce::var (root), false) + "\n");
    const double secs = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
    std::printf ("  wrote %d renders, %s (%lld bytes), renders in %s, %.1f s\n", (int) js.size(), manifestFile().getFullPathName().toRawUTF8(),
                 (long long) manifestFile().getSize(), rendersDir().getFullPathName().toRawUTF8(), secs);
    return blockFree ? 0 : 1;
}

// --golden: re-render and compare. A mismatch prints the take, the settings, and the first sample that differs (from the local
// render, if Tests/golden/renders/ has it), with the largest difference anywhere in the render (dBFS) and how many samples differ;
// the last line gives the largest over every mismatch (v0.7: to measure the x86_64 slice against the arm64 reference).
int compare()
{
    const auto t0 = std::chrono::steady_clock::now();
    const auto manifest = juce::JSON::parse (manifestFile());
    const auto* list = manifest["renders"].getArray();
    if (list == nullptr)
    {
        std::printf ("  no manifest at %s\n", manifestFile().getFullPathName().toRawUTF8());
        return 1;
    }
    const auto js = jobs();
    int bad = 0;
    // Inputs first: a changed take explains every mismatch on it.
    for (const auto& t : *manifest["takes"].getArray())
        for (int k = 0; k < kNumTakes; ++k)
            if (t["id"].toString() == kTakes[k].id && t["inputSha256"].toString() != sha256 (dry (k)))
            {
                std::printf ("  [FAIL] input %s changed (its file isn't the one the manifest was made from)\n", kTakes[k].id);
                ++bad;
            }
    if ((int) js.size() != list->size())
    {
        std::printf ("  [FAIL] the manifest has %d renders, this build's settings set %d\n", list->size(), (int) js.size());
        ++bad;
    }
    const auto r = renderAll (js, true);
    int matched = 0;
    double worstDiff = 0.0;
    juce::String worstWhere;
    for (size_t i = 0; i < js.size() && (int) i < list->size(); ++i)
    {
        const auto& m = (*list)[(int) i];
        const auto& j = js[i];
        if (m["take"].toString() != kTakes[j.take].id || m["settings"].toString() != j.setting.name || (int) m["sampleRate"] != (int) j.fs)
        {
            std::printf ("  [FAIL] render %d: the manifest has %s / %s, this build %s / %s\n", (int) i, m["take"].toString().toRawUTF8(),
                         m["settings"].toString().toRawUTF8(), kTakes[j.take].id, j.setting.name.toRawUTF8());
            ++bad;
            continue;
        }
        if (m["sha256"].toString() == r[i].hash && (int) m["samples"] == (int) r[i].out.size())
        {
            ++matched;
            continue;
        }
        ++bad;
        juce::String where = "no local render to locate it";
        const auto local = rendersDir().getChildFile (m["render"].toString());
        if (local.existsAsFile())
        {
            double rate = 0.0;
            const auto ref = readChannel (local, &rate);
            const int at = firstDifference (ref, r[i].out);
            double peak = 0.0;
            int differing = 0;
            for (size_t n = 0; n < std::min (ref.size(), r[i].out.size()); ++n)
            {
                const double d = std::abs ((double) ref[n] - (double) r[i].out[n]);
                peak = std::max (peak, d);
                differing += d > 0.0 ? 1 : 0;
            }
            if (peak > worstDiff)
            {
                worstDiff = peak;
                worstWhere = juce::String (kTakes[j.take].id) + " @ " + juce::String ((int) j.fs) + " Hz, " + j.setting.name;
            }
            where = at < 0 ? "the local render matches this build (the manifest is stale?)"
                           : juce::String::formatted ("first differing sample %d (%.4f s): reference %.9g, now %.9g; largest difference %.1f dBFS, %d samples differ",
                                                      at, at / j.fs, at < (int) ref.size() ? (double) ref[(size_t) at] : 0.0,
                                                      at < (int) r[i].out.size() ? (double) r[i].out[(size_t) at] : 0.0,
                                                      20.0 * std::log10 (std::max (peak, 1.0e-30)), differing);
        }
        std::printf ("  [FAIL] %s @ %d Hz, %s: %s\n", kTakes[j.take].id, (int) j.fs, j.setting.name.toRawUTF8(), where.toRawUTF8());
    }
    const double secs = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
    if (worstDiff > 0.0)
        std::printf ("  largest difference over every mismatch: %.1f dBFS (%.3g), %s\n", 20.0 * std::log10 (worstDiff), worstDiff, worstWhere.toRawUTF8());
    std::printf ("  %d of %d renders match the manifest bit for bit (%.1f s)\n", matched, list->size(), secs);
    return bad == 0 ? 0 : 1;
}

void selfTest()
{
    Sha256 s;
    s.add ("abc", 3);
    const auto h = s.hex();
    if (h != "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")
        std::printf ("  SHA-256 self-test FAILED: %s\n", h.toRawUTF8());
}
} // namespace golden
