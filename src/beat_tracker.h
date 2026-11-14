#pragma once

// Real beat tracking (independent of MilkDrop's bass/mid/treb):
// 1. onset detection function: log-compressed spectral flux (full band + bass band)
// 2. tempo estimate: autocorrelation of the onset function with a tempo prior
// 3. phase-locked loop: predicts beats, corrects phase/tempo on matching onsets,
//    keeps ticking through silence ("coasting") and re-locks on tempo changes / new tracks.

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

    // History for display, indexed by frame % kHistory; frame() is the newest frame + 1
    long frame() const { return n_; }
    std::vector<float> odf, threshold;
    std::vector<float> odfPhase;  // bass-weighted onset function used to place the beats (kick = beat)
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
    float emaFull_ = 0, emaLow_ = 0, longRms_ = 0;
    long lastOnset_ = -1000;
    long silentSince_ = -1;
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
