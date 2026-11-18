#include "Engine.h"

namespace gtt
{

void Engine::prepare (double sampleRate, int maxBlockSize)
{
    setPickSwitches (defaultPickSwitches);
    pickGlideOn = defaultPickGlide;
    fs = sampleRate;
    hop = hopFor (fs);
    gateInLen = msToSamples (fs, 5.0);
    gateOutLen = msToSamples (fs, 15.0);
    xfadeLen = msToSamples (fs, 10.0);
    windowFadeLen = msToSamples (fs, 20.0);
    settleLen = msToSamples (fs, kSettleMs);
    heldGraceLen = msToSamples (fs, kHeldLiveGraceMs);

    // History covers the detector window and the L delay for the largest L
    // (the playable zone's minimum window) at max(fs, 96 kHz).
    history.prepare (8 * maxTauFor (fs));

    detector.prepare (fs, maxBlockSize);
    ceiling = kDetectorMaxHz; // the edge rule's ceiling (tests: setCeilingHz after this)
    ceilingSure = 0.0f;
    synth.prepare (fs, maxBlockSize);
    onsets.prepare (fs, maxTauFor (fs));
    noteReference.prepare (fs);
    sustainLevel.prepare (fs);
    dynamicsGain.prepare (fs);
    chord.prepare (fs);
    headroom.prepare (fs);
    synth.shareFold (&foldShape); // Folded's drive, shared by every tone (copies of the live one)
    foldCoef = timeConstantCoef (fs, kFoldDriveSmoothMs);
    applyTail();
    reset();
}

void Engine::reset()
{
    history.reset();
    synth.reset();
    voiceGate.snap (0.0f);
    onsets.reset();
    noteReference.reset();
    sustainLevel.reset();
    dynamicsGain.reset();
    chord.reset();
    headroom.reset();
    voicePeak = predictedPeak = 0.0f;
    foldDrive = 1.0f;
    foldShape.set (1.0);
    sinceVoiced = kNeverVoiced;
    windowFade.snap (1.0f);
    windowState = WindowState::Idle;
    swapCount = 0;
    bypassMix.snap (0.0f);
    detector.reset();
    quantizer.reset();
    lastVoicedPeriod = (float) (fs / 220.0);
    lastVoicedEntry = -1;
    onsetsSeen = 0;
    chordLikeMs = notChordLikeMs = 0.0;
    chordFlag = false;
    lastHopVoiced = belowEnd = lastVoicedInTail = false;
    lastVoicedRms = liftEnv = 0.0f;
    liftWait = 0;
    sinceRepick = 1 << 30;
    sinceOnsetHops = 1 << 20;
    anchorNote = -1;
    locks = revivalsBlocked = mutes = edgeLags = graceDropouts = bottomEdgeCount = 0;
    glideLost = false;
    glideFromNote = glideToNote = -1;
    wasGraced = false;
    lastVoicedMidi = 0.0;
    gapFloor = 0.0f;
    pickWait = 0;
    pickAtHop = false;
    lastPickTime = -1;
    muteRing.fill (0.0f);
    muteRingPos = muteWakeWait = muteSureRun = 0;
    mutedFloor = 0.0f;
    muted = false;
    realEntry = lastOnsetEntry = -1;
    realT = 0.0;
    realNote = -1;
    realRing.fill (0.0f);
    realRingPos = pickGlideCount = 0;
    timeline.reset (PitchEntry { 0, false, false, lastVoicedPeriod, 1.0f, 57.0f });
    hopCounter = 0;
    setWindow (windowHz);
    firstBlock = true;
}

void Engine::setWindow (double hz)
{
    windowHz = clampWindow (hz);
    detector.setWindow (windowHz);
    latency = detector.windowLength();
    pendingTau = detector.tauMax();
    settleRemaining = 0;
}

Engine::Settings Engine::resolve (const EngineParams& p) const
{
    Settings s;
    // Window changes go through the debounced swap (stepWindowFade); Confidence, Hysteresis,
    // Hold and Sustain are read per hop / per sample, so they change continuously as they are.
    s.detection = effectiveDetection (p);
    s.retuneMs = retuneMsFor (p.retuneSpeed);
    s.holdSamples = holdSamplesFor (fs, s.detection.holdMs);
    s.octave = p.octave;
    s.sustain = std::clamp (p.sustainPercent * 0.01f, 0.0f, 1.0f);
    s.dynamics = std::clamp (p.dynamicsPercent * 0.01f, 0.0f, 1.0f);
    s.chord.mode = p.chordMode;
    s.chord.triad = p.triad;
    s.chord.voicing = p.voicing;
    s.chord.key = std::clamp (p.key, kKeyOff, 11);
    s.chord.scale = p.scale;
    s.chord.level = p.chordLevelPercent * 0.01f;
    s.chord.retuneMs = s.retuneMs;
    s.chord.memoryNotes = p.memoryNotes;
    s.folded = p.wave == Wave::Folded;
    return s;
}

void Engine::runHop (const Settings& s)
{
    const int64_t newest = history.newest();
    // Tests only: sticky voicing keeps a voiced note at a lower confidence / RMS than starting one needs.
    const bool keep = tail.keepConfidence > 0.0f && lastHopVoiced;
    auto det = detector.analyse (history, newest, keep ? std::min (tail.keepConfidence, s.detection.confidence) : s.detection.confidence,
                                 keep ? std::min (dbToGain (tail.keepRmsDb), kVoicedRmsFloor) : kVoicedRmsFloor);
    double midi = det.f0 > 0.0f ? midiFromHz (det.f0) : 0.0;
    const int64_t entryTime = newest + 1 - latency / 2;

    // The glide needs to know whether the voice will still be open when this entry plays: the
    // voice gate holds for Note Release after the last voiced sample, then fades over 15 ms, so it has
    // fully closed once the unvoiced stretch before this entry reaches holdSamples + that fade.
    NoteContext context;
    context.voiceOpen = lastVoicedEntry >= 0 && entryTime - lastVoicedEntry - hop < (int64_t) s.holdSamples + graceLen + gateOutLen;
    context.onset = onsets.foundCount() != onsetsSeen;
    onsetsSeen = onsets.foundCount();
    context.unvoicedHops = unvoicedRun;
    // Tests only (investigate-blips): the first hops from an onset unvoiced, or held to a stricter confidence.
    hopsSinceOnset = context.onset ? 0 : std::min (hopsSinceOnset + 1, 1 << 20);
    if (hopsSinceOnset < pickSw.ignoreHopsAfterOnset)
        det.voiced = false;
    if (pickSw.onsetConfidence > 0.0f && hopsSinceOnset < pickSw.onsetHops && det.confidence < pickSw.onsetConfidence)
        det.voiced = false;

    // The sputter fix (v0.5.5, Macro.h; TailSwitches says which part is which). 6, the edge rule: a reading above the
    // detector's top is its shortest lag, the range's edge, not a period.
    const double rawMidi = midi;
    bool edgeHit = false, revivalHit = false;
    if (tail.edgeLagUnvoiced && det.voiced && (det.f0 > ceiling || (ceilingSure > 0.0f && det.f0 > kDetectorMaxHz && det.confidence < ceilingSure)))
    {
        det.voiced = false;
        ++edgeLags;
        edgeHit = true;
    }
    // v0.6.2's fix 1, the bottom edge rule (Macro.h): a reading at the longest lag whose dip's own pitch rounds to another note is a
    // note under the window, not the note the floor's reading would play.
    if (bottomEdgeOn && det.voiced && det.atLongEdge && ! det.guardRejected && std::lround (midi - det.beyondCents / 100.0) != std::lround (midi))
    {
        det.voiced = false;
        ++bottomEdgeCount;
        edgeHit = true;
    }
    if (tail.endBelowDb < 0.0f) // tests only: the level floor ends the note
    {
        const float endFloor = dbToGain (tail.endBelowDb);
        if (belowEnd && (context.onset || det.rms > endFloor * dbToGain (6.0f)))
            belowEnd = false;
        else if (det.rms < endFloor)
            belowEnd = true;
        if (belowEnd)
            det.voiced = false;
    }
    // 4, revival: a tail whose gate has closed doesn't reopen unless the input comes back kReviveRiseDb louder, or a
    // pick starts a note (an onset rising reviveWakeRiseDb above the quietest since pitch was lost).
    const float levelNow = onsets.levelAhead();
    gapFloor = lastHopVoiced ? levelNow : std::min (gapFloor, levelNow);
    pickWait = context.onset ? muteWakeHops : std::max (0, pickWait - 1);
    const bool newPick = tail.reviveWakeRiseDb > 0.0f && pickWait > 0 && levelNow >= gapFloor * dbToGain (tail.reviveWakeRiseDb);
    pickAtHop = newPick;
    if (tail.reviveRiseDb > 0.0f && det.voiced && ! context.voiceOpen && lastVoicedInTail && entryTime - lastVoicedEntry < reviveLen
        && det.rms < lastVoicedRms * dbToGain (tail.reviveRiseDb) && ! newPick)
    {
        det.voiced = false;
        ++revivalsBlocked;
        revivalHit = true;
    }
    // 5, the mute gate: a mute (the undelayed level falling muteDropDb within kMuteWindowMs) unvoices until a pick (a rise,
    // or the level back near the note's reference), or kReviveWindowMs after the last voiced hop.
    if (tail.muteDropDb > 0.0f)
    {
        const float level = onsets.levelAhead();
        float recent = 0.0f;
        for (int k = 0; k < muteHops; ++k)
            recent = std::max (recent, muteRing[(size_t) k]);
        muteRing[(size_t) muteRingPos] = level;
        muteRingPos = (muteRingPos + 1) % muteHops;
        mutedFloor = std::min (mutedFloor, level);
        muteWakeWait = context.onset ? muteWakeHops : std::max (0, muteWakeWait - 1); // an onset's rise
        const bool picked = tail.muteWakeRiseDb > 0.0f && muteWakeWait > 0 && level >= mutedFloor * dbToGain (tail.muteWakeRiseDb);
        if (muted && (entryTime - lastVoicedEntry > reviveLen || picked || (noteReference.running() && level >= noteReference.level() * dbToGain (-tail.muteWakeUnderRefDb))))
            muted = false;
        else if (! muted && context.voiceOpen && level < recent * dbToGain (-tail.muteDropDb))
        {
            muted = true;
            mutedFloor = level;
            ++mutes;
        }
        // 8, the mute gate's pass (v0.5.10): a sure reading isn't a mute, and ends one.
        const bool sure = pickSw.muteKeepConfidence > 0.0f && det.voiced && det.confidence >= pickSw.muteKeepConfidence;
        muteSureRun = sure ? muteSureRun + 1 : 0;
        if (muted && sure && muteSureRun >= pickSw.muteKeepHops)
            muted = false;
        if (muted)
            det.voiced = false;
    }
    // 3, the harmonic lock. It leaves a note's start alone: an onset within the last kLockAfterOnsetMs (its pitch takes a
    // few hops to arrive).
    sinceOnsetHops = context.onset ? 0 : std::min (sinceOnsetHops + 1, 1 << 20);
    const bool recentOnset = (double) sinceOnsetHops * hop < kLockAfterOnsetMs * 0.001 * fs;
    // The note the pick established (kLockAfterOnsetMs after it): the lock always lets the voice back to it.
    if (recentOnset || ! quantizer.hasNote())
        anchorNote = -1;
    else if (anchorNote < 0)
        anchorNote = quantizer.note();
    const bool lockLevel = (tail.lockBelowDb < 0.0f && det.rms < dbToGain (tail.lockBelowDb))
                           || (tail.lockUnderRefDb > 0.0f && noteReference.running() && onsets.levelAhead() < noteReference.level() * dbToGain (-tail.lockUnderRefDb));
    bool hold = lockLevel && det.voiced && ! recentOnset && ! context.onset && quantizer.hasNote()
                && (lastHopVoiced || (tail.lockUnderRefDb > 0.0f && context.voiceOpen)) && (int) std::lround (midi) != anchorNote
                && std::abs (midi - quantizer.note()) > 0.5 + s.detection.hysteresisCents / 100.0;
    if (hold && (tail.lockPersistMs > 0.0f || tail.lockHarmonicsOnly))
    {
        const int next = (int) std::lround (midi), d = std::abs (next - quantizer.note());
        bool harmonic = d == 12 || d == 19 || d == 24;
        if (tail.lockOnPitch && lastVoicedMidi > 0.0)
        {
            const double j = std::abs (midi - lastVoicedMidi);
            harmonic = std::abs (j - 12.0) < 0.35 || std::abs (j - 19.0) < 0.35 || std::abs (j - 24.0) < 0.35;
        }
        if (! harmonic)
        {
            pendingHops = next == pendingNote ? pendingHops + 1 : 1;
            pendingNote = next;
            hold = ! tail.lockHarmonicsOnly && pendingHops * hop < tail.lockPersistMs * 0.001 * fs;
        }
    }
    else if (! hold)
    {
        pendingNote = -1;
    }
    // 7, the fall lock (v0.5.10, Macro.h): no note change without an onset while the window RMS is falling away (a damp before
    // a re-pick), unless the reading is sure or the change is small. (PickSwitches also has the investigation's variant on
    // the undelayed level, tests only.)
    {
        const int n = std::clamp ((int) std::lround (pickSw.fallWindowMs * 0.001 * fs / hop), 1, (int) fallRing.size());
        float recentMax = 0.0f;
        for (int k = 0; k < n; ++k)
            recentMax = std::max (recentMax, fallRing[(size_t) k]);
        const float lv = pickSw.fallOnWindowRms ? det.rms : onsets.levelAhead();
        fallRing[(size_t) fallRingPos] = lv;
        fallRingPos = (fallRingPos + 1) % n;
        if (pickSw.fallLockDb > 0.0f && det.voiced && ! context.onset && quantizer.hasNote() && lv < recentMax * dbToGain (-pickSw.fallLockDb)
            && ! (pickSw.fallPassConfidence > 0.0f && det.confidence >= pickSw.fallPassConfidence)
            && std::abs (midi - quantizer.note()) > 0.5 + s.detection.hysteresisCents / 100.0
            && std::abs (midi - quantizer.note()) >= pickSw.fallMinSemis - 0.5)
            hold = true;
    }
    if (det.voiced && ! hold)
        lastVoicedMidi = midi;
    if (hold)
    {
        midi = quantizer.note();
        det.f0 = (float) hzFromMidi (midi);
        det.period = (float) (fs / det.f0);
        ++locks;
    }
    lastHopVoiced = det.voiced;
    if (det.voiced)
    {
        lastVoicedRms = det.rms;
        lastVoicedInTail = noteReference.running() && (tail.reviveLevelAhead ? onsets.levelAhead() : synth.meanAbsLevel()) < noteReference.level() * dbToGain (-kTailUnderReferenceDb);
    }

    // v0.5.11, the pick glide: a pick within kPickGlideWindowMs of the last real note glides in from it, the voice closed or not.
    // A real note's hop: voiced, not held by a lock, the window RMS not 9 dB under its highest of the last 50 ms (no damp); and
    // after one, a voiced hop that keeps its note (it carries on into the damp). Only the damp's junk (another note) isn't.
    float realMax = 0.0f;
    for (int k = 0; k < realRingLen; ++k)
        realMax = std::max (realMax, realRing[(size_t) k]);
    realRing[(size_t) realRingPos] = det.rms;
    realRingPos = (realRingPos + 1) % realRingLen;
    if (context.onset)
        lastOnsetEntry = entryTime;
    if (pickGlideOn && realEntry >= 0 && lastOnsetEntry > realEntry && entryTime - realEntry <= (int64_t) pickGlideLen)
    {
        context.pickGlide = true;
        context.pickFromT = realT;
    }
    const bool wasMidGlide = quantizer.hasNote() && std::abs (quantizer.target() - quantizer.note()) * 100.0 > kGlideToRealMinCents;
    quantizer.update (det.voiced, midi, s.detection.hysteresisCents, s.retuneMs, (double) hop / fs, context);
    // v0.6.2's fix 2 (Macro.h): pitch lost mid-glide with the voice open: T goes on to the last real note, not frozen where it was.
    if (det.voiced || ! context.voiceOpen)
        glideLost = false;
    else if (unvoicedRun == 0 && wasMidGlide) // the first unvoiced hop: only toward a note the glide was between
        glideLost = glideToRealOn && realEntry >= 0 && (realNote == quantizer.note() || realNote == glideFromNote);
    if (glideLost)
    {
        quantizer.glideToReal (realNote, s.retuneMs, (double) hop / fs);
    }
    unvoicedRun = det.voiced ? 0 : unvoicedRun + 1;
    if (quantizer.note() != glideToNote) // fix 2: the note before the current one (where a glide to it started)
    {
        glideFromNote = glideToNote;
        glideToNote = quantizer.note();
    }
    pickGlideCount += quantizer.event() == Quantizer::Event::PickGlide ? 1 : 0;
    const bool realHop = det.voiced && ((! hold && det.rms >= realMax * dbToGain (-kFallLockDropDb)) || (realEntry >= 0 && quantizer.note() == realNote));
    if (realHop)
    {
        realEntry = entryTime; // a real note, or the same note carrying on into the damp
        realT = quantizer.target();
        realNote = quantizer.note();
    }

    if (det.voiced)
    {
        lastVoicedPeriod = det.period;
        lastVoicedEntry = entryTime;
    }

    // 2+ NOTES? hint: loud but unsure for > kOnMs sets it; quiet or sure for kOffMs clears it.
    const double hopMs = 1000.0 * hop / fs;
    if (det.rms > dbToGain (chordHint.minRmsDb) && det.confidence < chordHint.maxConfidence)
    {
        chordLikeMs += hopMs;
        notChordLikeMs = 0.0;
        if (chordLikeMs > chordHint.onMs)
            chordFlag = true;
    }
    else
    {
        notChordLikeMs += hopMs;
        chordLikeMs = 0.0;
        if (notChordLikeMs >= chordHint.offMs)
            chordFlag = false;
    }

    PitchEntry e;
    e.time = entryTime;
    e.voiced = det.voiced;
    e.hasNote = quantizer.hasNote();
    e.period = lastVoicedPeriod;
    e.ratio = (float) quantizer.ratio();
    e.target = (float) quantizer.target();
    e.note = quantizer.note();
    e.muted = muted && tail.muteSkipsGrace;
    e.pick = pickAtHop && tail.liftOnPick;
    e.inMidi = det.voiced ? (float) rawMidi : -1.0f; // the pitch trace's input line (display only)
    e.inConfidence = det.confidence;
    timeline.push (e);

    const auto relaxed = std::memory_order_relaxed;
    readoutState.voiced.store (det.voiced, relaxed);
    readoutState.hasNote.store (quantizer.hasNote(), relaxed);
    readoutState.noteN.store (quantizer.note(), relaxed);
    readoutState.outNote.store ((int) std::lround (quantizer.target()), relaxed);
    readoutState.cents.store ((float) ((midi - quantizer.note()) * 100.0), relaxed);
    readoutState.f0.store (det.f0, relaxed);
    readoutState.confidence.store (det.confidence, relaxed);
    readoutState.twoPlusNotes.store (chordFlag, relaxed);
    readoutState.voiceNote.store ((int) std::lround (quantizer.target()) - 12 * (int) s.octave, relaxed);
    readoutState.voiceHz.store ((float) (hzFromMidi (quantizer.target()) * octaveRatio (s.octave)), relaxed);

    if (hopListener)
    {
        HopInfo h { entryTime, det, midi, quantizer.note(), quantizer.target(), quantizer.ratio(), quantizer.hasNote(), chordFlag };
        h.onset = context.onset;
        h.voiceOpen = context.voiceOpen;
        h.lockHeld = hold;
        h.muted = muted;
        h.revivalBlocked = revivalHit;
        h.edgeLag = edgeHit;
        h.rawMidi = rawMidi;
        h.event = (int) quantizer.event();
        hopListener (h);
    }
}

float Engine::stepWindowFade (double requestedHz)
{
    switch (windowState)
    {
        case WindowState::Idle:
        {
            // Coalesce knob motion: restart the settle timer whenever the requested
            // tauMax changes; swap only after it has been still for kSettleMs.
            const int requestedTau = tauMaxFor (fs, requestedHz);
            if (requestedTau == detector.tauMax())
            {
                pendingTau = requestedTau;
            }
            else if (requestedTau != pendingTau)
            {
                pendingTau = requestedTau;
                settleRemaining = settleLen;
            }
            else if (settleRemaining > 0)
            {
                --settleRemaining;
            }
            else
            {
                windowState = WindowState::FadingOut;
                windowFade.setTarget (0.0f, windowFadeLen);
            }
            break;
        }
        case WindowState::FadingOut:
            if (! windowFade.isSmoothing())
            {
                setWindow (requestedHz); // the latest value, even if it moved during the fade
                ++swapCount;
                windowState = WindowState::FadingIn;
                windowFade.setTarget (1.0f, windowFadeLen);
            }
            break;
        case WindowState::FadingIn:
            if (! windowFade.isSmoothing())
                windowState = WindowState::Idle;
            break;
    }
    return windowFade.next();
}

float Engine::processVoice (int64_t t, float delayedIn, int holdSamples, const Settings& s)
{
    const PitchEntry& e = timeline.lookup (t);
    const bool wasClosed = voiceGate.current <= 0.0f && ! voiceGate.isSmoothing();

    // Lost pitch: keep sounding the held target for holdSamples after the last voiced
    // sample (at Sustain 0 the envelope keeps following the guitar), then fade out over 15 ms. Pitch
    // back during the hold: no fade. Pitch back after the fade: fade in over 5 ms.
    // holdSamples = 0 is v0.2's Mute; silent before the first note.
    if (e.voiced)
        sinceVoiced = 0;
    // 1, the grace: within it a dropout holds the level (and the gate's hold starts after it). 5: a mute skips it.
    const int grace = e.muted ? 0 : graceLen;
    const bool graced = ! e.voiced && grace > 0 && e.hasNote && sinceVoiced < grace && ! wasClosed;
    graceDropouts += graced && ! wasGraced ? 1 : 0;
    wasGraced = graced;
    const bool open = e.voiced || (e.hasNote && sinceVoiced < holdSamples + grace);
    if (! e.voiced && sinceVoiced < kNeverVoiced)
        ++sinceVoiced;
    voiceGate.setTarget (open ? 1.0f : 0.0f, open ? gateInLen : gateOutLen);
    const float gate = voiceGate.next();

    // The level: the guitar's (envelope follower), or with Sustain on, less of its fade while the
    // note is detected and a fade over Note Release once it's lost (SustainLevel). The gate above
    // holds the pitch for Note Release either way. Sustain 0 bypasses it. Then the Dynamics gain on
    // the note's level (bypassed at 100%). Sustain and Dynamics share the note's reference
    // level (NoteReference). It always runs (it never touches the output on its own), so either can
    // come on mid-note with the note's own level.
    const float env = synth.follow (delayedIn, e.period);
    const bool onset = onsets.process (t, synth.meanAbsLevel());
    if (tail.pickRestartUnderDb > 0.0f) // tests only: a pick in the tail starts afresh
        noteReference.setRestartOnOnset (noteReference.running() && env < noteReference.level() * dbToGain (-tail.pickRestartUnderDb));
    noteReference.process (onsets.level(), onset, e.voiced, wasClosed, e.note);
    const bool dynamicsOn = dynamicsGain.active (s.dynamics);
    float level = env;
    float sustain = s.sustain;
    if (tail.taperTopDb < 0.0f && sustain > 0.0f) // tests only: Sustain tapers off near the noise floor
        sustain *= std::clamp ((20.0f * std::log10 (std::max (env, 1.0e-9f)) - tail.taperTopDb + tail.taperRangeDb) / tail.taperRangeDb, 0.0f, 1.0f);
    const bool needUnderRef = tail.taperUnderRefDb > 0.0f || tail.oneWayUnderDb >= 0.0f;
    const float underRefDb = needUnderRef && noteReference.running() ? 20.0f * std::log10 (std::max (noteReference.level(), 1.0e-9f) / std::max (env, 1.0e-9f)) : 0.0f;
    if (tail.taperUnderRefDb > 0.0f && sustain > 0.0f)
        sustain *= std::clamp ((tail.taperUnderRefDb + tail.taperRangeDb - underRefDb) / tail.taperRangeDb, 0.0f, 1.0f);
    // 2, the one-way release in the tail. A re-pick lifts its cap: an onset, and the input up kRepickRiseDb within
    // kRepickWindowMs of it (noise in a gap fires onsets too); for kRepickProtectMs after one, pitch back (the pick's own
    // dropout ending) isn't capped.
    if (onset)
    {
        liftWait = liftLen;
        liftEnv = env;
    }
    if (liftWait > 0)
    {
        --liftWait;
        if (env > liftEnv * dbToGain (kRepickRiseDb))
        {
            sustainLevel.liftCap();
            liftWait = 0;
            sinceRepick = 0;
        }
    }
    if (e.pick && e.time != lastPickTime) // a real pick lifts the cap too (liftOnPick), once per hop's entry
    {
        lastPickTime = e.time;
        sustainLevel.liftCap();
        liftWait = 0;
        sinceRepick = 0;
    }
    if (sinceRepick < repickLen)
        ++sinceRepick;
    sustainLevel.setOneWay (tail.oneWayUnderDb >= 0.0f && underRefDb >= tail.oneWayUnderDb && sinceRepick >= repickLen);
    const bool levelVoiced = e.voiced || graced;
    if (s.sustain > 0.0f)
        level = sustainLevel.process (noteReference, env, sustain, levelVoiced, levelVoiced ? 0 : sinceVoiced - grace, holdSamples);
    else if (sustainLevel.inUse())
        level = sustainLevel.handBack (env);
    // Folded (v0.5.8): the drive follows the pick's own level, after Sustain and before the Dynamics gain (v0.5.9; v0.5.8 took it
    // after Dynamics), so Dynamics evens out the loudness and the brightness follows the picking at full range.
    // The live note's; the chord's tones share the shape.
    if (s.folded)
    {
        foldDrive += foldCoef * (foldDriveFor (level) - foldDrive);
        foldShape.set (foldDrive);
    }
    float dynamics = 1.0f;
    if (dynamicsOn)
        level *= (dynamics = dynamicsGain.process (noteReference, s.dynamics));
    traceLevel = gate * level; // the pitch trace (display only)
    if (voiceListener)
    {
        probe = { t, e.voiced, open, onset, graced, sustainLevel.capped(), e.note, gate, env, noteReference.level(), level, dynamics };
        probe.foldDrive = s.folded ? foldDrive : 1.0f;
    }
    // voicePeak: the bound on what this returns (Tone::peak through the same gains), for the headroom gain.
    if (! chord.active()) // Chord Off, nothing fading: v0.4.6 exactly
    {
        const float out = gate * synth.render (e.target, level);
        voicePeak = gate * synth.tone().peak();
        return out;
    }

    // The chord: the played note is the voice as above; the other tones follow its level x Chord Level, and the gain law
    // scales the sum.
    chord.step (synth.tone(), e.target, e.note, e.voiced, wasClosed, wasClosed && ! open);
    const float lg = chord.liveGain();
    const float live = lg * synth.render (e.target, level);
    const float others = chord.renderOthers (level);
    voicePeak = gate * chord.gain() * (lg * synth.tone().peak() + chord.othersPeak());
    return gate * chord.gain() * (live + others);
}

// v0.5.11, the pitch trace (display only; reads the voice's state, changes none of it): the moment the voice is playing (t on its
// delayed timeline, so the input's reading lines up with the output it became), the input's raw pitch there, the voice's sounding
// pitch and level, and the chord's other tones.
void Engine::pushTrace (int64_t t, float fade, const Settings& s)
{
    const PitchEntry& e = timeline.lookup (t);
    TraceFrame f;
    f.time = t;
    f.inMidi = e.inMidi;
    f.inConfidence = e.inConfidence;
    const float shift = -12.0f * (float) (int) s.octave; // Octave: the voice's pitch
    const float level = traceLevel * fade;
    if (e.hasNote && voiceGate.current > 0.0f)
    {
        f.outMidi = e.target + shift;
        f.level = level;
    }
    if (chord.active())
    {
        int n = 0;
        for (int k = 0; k < ChordVoice::kSlots && n < TraceFrame::kChordTones; ++k)
            if (chord.slotSounding (k))
            {
                f.chordMidi[(size_t) n] = chord.slotPitch (k) + shift;
                f.chordLevel[(size_t) n] = level * chord.slotPresence (k);
                ++n;
            }
    }
    trace->push (f);
}

void Engine::process (const float* in, float* out, int numSamples, const EngineParams& p)
{
    const Settings s = resolve (p);
    const float bypassTarget = p.bypass ? 1.0f : 0.0f;

    if (firstBlock)
    {
        bypassMix.snap (bypassTarget);
        setWindow (s.detection.windowHz);
        firstBlock = false;
    }

    bypassMix.setTarget (bypassTarget, xfadeLen);

    VoiceSettings v;
    v.wave = p.wave;
    v.octave = s.octave;
    v.filterCutoffHz = p.filterCutoffHz;
    synth.setVoice (v);
    chord.setVoice (v);
    chord.setSettings (s.chord);
    const int holdSamples = s.holdSamples;
    graceLen = holdSamples > 0 ? graceLenHeld : graceLenInstant; // the sputter fix's grace (Instant has its own)
    float lowestGain = 1.0f;
    const bool tracing = trace != nullptr && trace->readers.load (std::memory_order_relaxed) > 0; // the pitch trace: an editor is open

    for (int i = 0; i < numSamples; ++i)
    {
        const float x = in[i];
        history.push (x);
        onsets.push (x, history.newest(), lastVoicedPeriod); // L samples ahead of the voice
        if (++hopCounter >= hop)
        {
            hopCounter = 0;
            runHop (s);
        }

        const float fade = stepWindowFade (s.detection.windowHz);

        // The synth runs on the L-delayed timeline (its envelope follows the delayed input).
        const int64_t t = history.newest() - latency;
        const float voice = processVoice (t, history.at (t), holdSamples, s) * fade;
        if (tracing && ++traceCounter >= hop)
        {
            traceCounter = 0;
            pushTrace (t, fade, s);
        }
        // The headroom gain (exactly 1 unless the predicted peak nears the clip), then the clip.
        predictedPeak = voicePeak * fade;
        const float h = headroomOn ? headroom.process (predictedPeak) : 1.0f;
        lowestGain = std::min (lowestGain, h);
        const float y = safetyClip (voice * h);
        if (voiceListener)
        {
            probe.headroomGain = h;
            probe.predictedPeak = predictedPeak;
            probe.memoryHeld = chord.heldNotes (sinceVoiced < heldGraceLen);
            voiceListener (probe);
        }

        // Bypass: hard reference switch to the raw, undelayed input (10 ms crossfade).
        const float b = bypassMix.next();
        out[i] = (b <= 0.0f && ! bypassMix.isSmoothing()) ? y : (1.0f - b) * y + b * x;
    }

    const auto relaxed = std::memory_order_relaxed;
    readoutState.latencyMs.store ((float) (1000.0 * latency / fs), relaxed);
    readoutState.windowLength.store (latency, relaxed);
    if (lowestGain < 1.0f)
        readoutState.gainReductionDb.store (std::min (readoutState.gainReductionDb.load (relaxed), 20.0f * std::log10 (lowestGain)), relaxed);
    // Voice state on the delayed (sounding) timeline, at the end of the block.
    const bool sounding = voiceGate.target > 0.0f;
    readoutState.voiceOpen.store (sounding, relaxed);
    readoutState.holdRemainingMs.store (sounding ? (float) (1000.0 * std::max (0, holdSamples - sinceVoiced) / fs) : 0.0f, relaxed);
    uint64_t packed = ~uint64_t { 0 };
    if (sounding)
    {
        std::array<int, 8> notes {};
        const int n = chord.tones (timeline.lookup (history.newest() - latency).note, notes);
        packed = 0;
        for (int k = 0; k < 8; ++k)
            packed |= (uint64_t) (k < n ? std::clamp (notes[(size_t) k], 0, 127) : 0xff) << (8 * k);
    }
    readoutState.chordTones.store (packed, relaxed);
    readoutState.memoryHeld.store (chord.heldNotes (sinceVoiced < heldGraceLen), relaxed);
}

} // namespace gtt
