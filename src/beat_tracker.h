#pragma once

// Real beat tracking (independent of MilkDrop's bass/mid/treb):
// 1. onset detection function: log-compressed spectral flux (full band + bass band)
// 2. tempo estimate: autocorrelation of the onset function with a tempo prior
// 3. phase-locked loop: predicts beats, corrects phase/tempo on matching onsets,
//    keeps ticking through silence ("coasting") and re-locks on tempo changes / new tracks.

#include <array>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <vector>

struct TrackerSettings {
    float minBpm = 70.0f;
    float maxBpm = 180.0f;
    float preferredBpm = 120.0f;   // centre of the tempo prior (against half/double tempo errors)
    float sensitivity = 1.0f;      // onset detection: higher = more onsets
    float bassWeight = 0.5f;       // 0 = full spectrum flux, 1 = bass band (kick) only
    float phaseGain = 0.25f;       // how strongly a matching onset pulls the beat phase
    float tempoGain = 0.04f;       // how strongly it adjusts the tempo
    float acquireConfidence = 0.25f;  // tempo confidence needed to lock
    float coastSeconds = 10.0f;    // keep ticking through silence this long
    float relockSeconds = 4.0f;    // a different tempo must persist this long before re-locking
    bool alignSwitches = true;     // automatic preset switches wait for the next beat
};

enum class TrackState { Searching, Locked, Coasting };

class BeatTracker {
public:
    static constexpr int kFft = 2048;
    static constexpr int kHop = 512;
    static constexpr int kHistory = 1024;  // onset frames kept (~11 s at 48 kHz)

    enum Flags : uint8_t { FlagOnset = 1, FlagBeat = 2, FlagCorrected = 4, FlagSilent = 8, FlagMatched = 16 };

    BeatTracker();

    void setSampleRate(int rate);
    float frameRate() const { return static_cast<float>(sampleRate_) / kHop; }

    // Feed interleaved stereo samples. Returns the number of beats that occurred in them.
    int process(const float* stereo, size_t frames, const TrackerSettings& s);

    // State
    TrackState state = TrackState::Searching;
    float bpm() const;                   // tracked tempo (0 while searching)
    float estimateBpm = 0;               // latest autocorrelation estimate
    float estimateConfidence = 0;        // 0..1
    float confidence = 0;                // how well recent beats matched onsets, 0..1
    bool silent = true;
    long beatCount = 0;
    float beatPhase() const;             // 0..1 position between beats
    float mismatchSeconds = 0;           // how long the estimate has disagreed with the lock
    float phaseMismatchSeconds = 0;      // how long another beat phase has fitted clearly better

    // How present the beat is right now (0..1): kick strength at the latest beats relative to the
    // usual kick of this track. 0 at once on real silence, fades with a weak or missing kick.
    float beatPresence = 0;
    float silentSeconds = 0;  // how long the current silence lasts (0 while there is sound)

    // Held "beat is on" state for the beat pulse: on once the lock is sure, off only when the
    // rhythmic activity collapses for 2 beats or the tempo is lost. Silence is
    // handled by the caller (instant mute, instant return).
    bool beatActive = false;
    float presenceSlow = 0;   // kick presence averaged over ~8 beats (for real fade-outs)
    float beatActivity = 1;   // rhythmic activity over the last beat (1 = usual, <0.45 = stopped)
    float loudnessFade = 1;   // 1 = normal level, falls to 0 when the track fades out (-8 .. -20 dB)

    // Bar position ("the 1"): manual (markDownbeat) or estimated from where the sound changes
    int barPosition() const;             // 0 = last beat was the "1", 1..3, -1 = unknown
    void markDownbeat();                 // the nearest beat is the "1"
    bool downbeatManual = false;
    float downbeatConfidence = 0;        // automatic estimate, 0..1 (applied from 0.5)
    int suggestedDownbeat() const;       // bar position the automatic estimate prefers, -1 = none
    std::array<float, 4> downbeatVotes{};  // sound changes per bar position (relative to lock)

    // History for display, indexed by frame % kHistory; frame() is the newest frame + 1
    long frame() const { return n_; }
    std::vector<float> odf, threshold;
    std::vector<float> odfPhase;  // kick detector used to place the beats (lags the newest frame by ~80 ms)
    std::vector<uint8_t> flags;
    // Tempo curve for display: score per BPM from minBpm to maxBpm (1 BPM steps)
    std::vector<float> tempoCurve;
    float tempoCurveMinBpm = 0;

private:
    void hop(const TrackerSettings& s);
    void estimateTempo(const TrackerSettings& s);
    void acquire(const TrackerSettings& s, float period);
    void track(const TrackerSettings& s);
    float odfAt(double f) const;
    float phaseAt(double f) const;
    float combScore(double beatFrame, float period) const;
    double bestBeatFrame(float period, float* score) const;

    int sampleRate_ = 48000;
    std::vector<float> input_;  // last kFft mono samples (ring)
    size_t inputPos_ = 0;
    int pending_ = 0;            // samples since the last hop

    std::vector<float> window_;
    std::vector<std::complex<float>> buf_;
    std::vector<float> prevLog_;

    long n_ = 0;  // number of onset frames processed
    float emaFull_ = 0, emaLow_ = 0, longRms_ = 0, emaClick_ = 0, emaSubE_ = 0;
    float levelSlow_ = 0, levelRef_ = 0;
    std::vector<float> clickHist_, subHist_;  // normalised click onsets / sub-bass energy per frame
    int kickDelay_ = 2;                       // frames the kick detector lags behind

    // Beat presence / downbeat
    void beatEmitted();
    void updatePresence(float kick, bool quiet);
    long beatIndex_ = 0;        // beats emitted since the lock
    int downbeatOffset_ = -1;   // beatIndex_ % 4 of the "1"
    float kickAvg_ = 0;         // usual kick strength of this track
    static constexpr int kBands = 12;
    std::array<float, kBands> bandAcc_{}, prevBands_{};
    bool havePrevBands_ = false;
    long votedBeats_ = 0;
    int missedBeats_ = 0;   // consecutive beats without onset and kick
    int goodBeats_ = 0;     // consecutive beats with evidence
    long lastOnset_ = -1000;
    long coastSince_ = -1;  // frame when coasting started
    int beatsInHop_ = 0;

    // PLL
    float period_ = 0;        // frames per beat
    float lockPeriod_ = 0;    // period at lock time (limits drift)
    double nextBeat_ = 0;     // predicted frame of the next beat
    bool emitted_ = false;    // nextBeat_ already emitted
    long lastBeatFrame_ = -1;
    float lastEstimatePeriod_ = 0;
    int agreeingEstimates_ = 0;
};
