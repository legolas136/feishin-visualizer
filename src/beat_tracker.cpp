#include "beat_tracker.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr float kPi = 3.14159265358979f;

void fftInPlace(std::vector<std::complex<float>>& a) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; i++) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const float ang = -2.0f * kPi / static_cast<float>(len);
        const std::complex<float> wl(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            std::complex<float> w(1.0f, 0.0f);
            for (size_t j = 0; j < len / 2; j++) {
                const auto u = a[i + j];
                const auto v = a[i + j + len / 2] * w;
                a[i + j] = u + v;
                a[i + j + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

}  // namespace

BeatTracker::BeatTracker()
    : odf(kHistory, 0.0f),
      threshold(kHistory, 0.0f),
      odfPhase(kHistory, 0.0f),
      flags(kHistory, FlagSilent),
      input_(kFft, 0.0f),
      window_(kFft),
      buf_(kFft),
      prevLog_(kFft / 2, 0.0f),
      clickHist_(kHistory, 0.0f),
      subHist_(kHistory, 0.0f) {
    for (int i = 0; i < kFft; i++) window_[i] = 0.5f - 0.5f * std::cos(2.0f * kPi * i / kFft);
}

void BeatTracker::setSampleRate(int rate) {
    if (rate > 0) sampleRate_ = rate;
}

float BeatTracker::bpm() const {
    return (state == TrackState::Searching || period_ <= 0) ? 0.0f : 60.0f * frameRate() / period_;
}

float BeatTracker::beatPhase() const {
    if (state == TrackState::Searching || period_ <= 0 || lastBeatFrame_ < 0) return 0.0f;
    return std::clamp(static_cast<float>(n_ - lastBeatFrame_) / period_, 0.0f, 1.0f);
}

static float historyAt(const std::vector<float>& h, double f, long n, int size) {
    if (f < 0 || f > n - 1 || f < n - size + 1) return 0.0f;
    const long i = static_cast<long>(f);
    const float t = static_cast<float>(f - i);
    const float a = h[i % size];
    const float b = (i + 1 < n) ? h[(i + 1) % size] : a;
    return a + (b - a) * t;
}

float BeatTracker::odfAt(double f) const { return historyAt(odf, f, n_, kHistory); }
float BeatTracker::phaseAt(double f) const { return historyAt(odfPhase, f, n_, kHistory); }

// How much (bass-weighted) onset energy a beat grid through beatFrame collects over 8 beats
float BeatTracker::combScore(double beatFrame, float period) const {
    float sc = 0;
    for (int k = 0; k < 8; k++) {
        const double f = beatFrame - k * period;
        sc += std::max({phaseAt(f - 1), phaseAt(f), phaseAt(f + 1)});
    }
    return sc;
}

// Beat frame (within the last period) whose grid fits best
double BeatTracker::bestBeatFrame(float period, float* score) const {
    const long last = n_ - 1;
    float best = -1;
    double frame = last;
    for (float ph = 0; ph < period; ph += 0.5f) {
        const float sc = combScore(last - ph, period);
        if (sc > best) {
            best = sc;
            frame = last - ph;
        }
    }
    if (score) *score = best;
    return frame;
}

int BeatTracker::process(const float* stereo, size_t frames, const TrackerSettings& s) {
    int beats = 0;
    for (size_t f = 0; f < frames; f++) {
        input_[inputPos_] = 0.5f * (stereo[f * 2] + stereo[f * 2 + 1]);
        inputPos_ = (inputPos_ + 1) % kFft;
        if (++pending_ >= kHop) {
            pending_ = 0;
            beatsInHop_ = 0;
            hop(s);
            beats += beatsInHop_;
        }
    }
    return beats;
}

void BeatTracker::hop(const TrackerSettings& s) {
    const float fr = frameRate();
    const long i = n_ % kHistory;

    // --- Spectrum of the newest kFft samples
    double energy = 0;
    for (int k = 0; k < kFft; k++) {
        const float v = input_[(inputPos_ + k) % kFft];
        energy += v * v;
        buf_[k] = std::complex<float>(v * window_[k], 0.0f);
    }
    fftInPlace(buf_);
    const float rms = static_cast<float>(std::sqrt(energy / kFft));

    // --- Silence gate: absolute floor or far below the recent level
    longRms_ = longRms_ * 0.995f + rms * 0.005f;
    // Real silence only (pause, track gap): about -70 dBFS or far below the recent level.
    // Quiet breakdowns are not silence.
    silent = rms < 3e-4f || rms < longRms_ * 0.02f;

    // --- Log-compressed spectral flux: full band (to ~10 kHz), bass (30-180 Hz), click (1.5-5 kHz);
    //     sub-bass (30-90 Hz) energy
    const float binHz = static_cast<float>(sampleRate_) / kFft;
    const int kMax = std::min(kFft / 2 - 1, static_cast<int>(10000.0f / binHz));
    const int lowLo = std::max(1, static_cast<int>(30.0f / binHz));
    const int lowHi = std::max(lowLo + 1, static_cast<int>(180.0f / binHz + 1.0f));
    const int subHi = std::max(lowLo + 1, static_cast<int>(90.0f / binHz + 1.0f));
    const int clickLo = static_cast<int>(1500.0f / binHz), clickHi = static_cast<int>(5000.0f / binHz);
    float fluxFull = 0, fluxLow = 0, fluxClick = 0, subEnergy = 0;
    for (int k = 1; k <= kMax; k++) {
        const float mag = std::abs(buf_[k]) / (kFft * 0.25f);
        const float l = std::log1p(1000.0f * mag);
        const float d = l - prevLog_[k];
        prevLog_[k] = l;
        if (k >= lowLo && k <= subHi) subEnergy += mag * mag;
        if (d > 0) {
            fluxFull += d;
            if (k >= lowLo && k <= lowHi) fluxLow += d;
            if (k >= clickLo && k <= clickHi) fluxClick += d;
        }
    }

    float value = 0;
    if (!silent) {
        // Normalise both bands by their recent mean so the mix is loudness independent
        const float a = 1.0f / (3.0f * fr);  // ~3 s
        emaFull_ += (fluxFull - emaFull_) * a;
        emaLow_ += (fluxLow - emaLow_) * a;
        const float full = fluxFull / (emaFull_ + 1e-6f);
        const float low = fluxLow / (emaLow_ + 1e-6f);
        value = (1.0f - s.bassWeight) * full + s.bassWeight * low;
        emaClick_ += (fluxClick - emaClick_) * a;
        emaSubE_ += (subEnergy - emaSubE_) * a;
        clickHist_[i] = fluxClick / (emaClick_ + 1e-6f);
        subHist_[i] = subEnergy / (emaSubE_ + 1e-9f);
    } else {
        clickHist_[i] = 0;
        subHist_[i] = 0;
    }

    // Kick detector for placing the beats: a click (1.5-5 kHz onset) followed within ~80 ms by
    // rising sub-bass (30-90 Hz). The bassline has sub without the click, hi-hats click without
    // sub, so both stop pulling the beat onto the off-beats. Needs D frames of look-ahead, so the
    // value is written D frames back into the history.
    const int D = std::max(2, static_cast<int>(0.08f * fr));
    kickDelay_ = D;
    odfPhase[i] = 0;
    if (n_ >= D) {
        const long j = (n_ - D) % kHistory;
        float rise = 0;
        for (int k = 1; k <= D; k++) rise = std::max(rise, subHist_[(n_ - D + k) % kHistory] - subHist_[j]);
        odfPhase[j] = std::sqrt(std::max(0.0f, clickHist_[j]) * rise);
    }

    odf[i] = value;
    flags[i] = silent ? FlagSilent : 0;

    // Spectrum summary per beat (12 log-spaced bands, 60 Hz - 8 kHz) for the downbeat estimate
    if (silent) {
        // Short gaps (stutter edits) keep the kick level; a real pause starts over
        silentSeconds += kHop / static_cast<float>(sampleRate_);
        if (silentSeconds > 1.0f) beatPresence = 0;
    } else {
        silentSeconds = 0;
        for (int b = 0; b < kBands; b++) {
            const float lo = 60.0f * std::pow(2.0f, b * 7.0f / kBands);
            const float hi = 60.0f * std::pow(2.0f, (b + 1) * 7.0f / kBands);
            const int k0 = std::max(1, static_cast<int>(lo / binHz));
            const int k1 = std::max(k0 + 1, static_cast<int>(hi / binHz));
            float e = 0;
            for (int k = k0; k < k1 && k < kFft / 2; k++) e += std::norm(buf_[k]);
            bandAcc_[b] += e;
        }
    }

    // --- Adaptive threshold: mean of the last ~0.4 s, scaled by sensitivity
    const int h = std::max(4, static_cast<int>(0.4f * fr));
    float mean = 0;
    int cnt = 0;
    for (int k = 1; k <= h && n_ - k >= 0; k++, cnt++) mean += odf[(n_ - k) % kHistory];
    mean = cnt ? mean / cnt : 0.0f;
    threshold[i] = mean * (1.0f + 0.6f / std::max(0.1f, s.sensitivity)) + 0.1f;

    // --- Onset = local peak above threshold (decided one frame late), at least 100 ms apart
    if (n_ >= 2) {
        const long p = n_ - 1;
        const float vp = odf[p % kHistory];
        if (vp > threshold[p % kHistory] && vp >= odf[(p - 1) % kHistory] && vp > value &&
            (p - lastOnset_) >= static_cast<long>(0.1f * fr)) {
            flags[p % kHistory] |= FlagOnset;
            lastOnset_ = p;
        }
    }

    n_++;

    if (n_ % 24 == 0) estimateTempo(s);
    track(s);
}

void BeatTracker::estimateTempo(const TrackerSettings& s) {
    const float fr = frameRate();
    const long len = std::min<long>(static_cast<long>(6.0f * fr), std::min<long>(n_, kHistory));
    if (len < static_cast<long>(3.0f * fr)) return;

    // Centered onset function of the last `len` frames
    std::vector<float> x(len);
    float mean = 0;
    for (long k = 0; k < len; k++) {
        x[k] = odf[(n_ - len + k) % kHistory];
        mean += x[k];
    }
    mean /= len;
    for (auto& v : x) v -= mean;

    const int lagMin = std::max(2, static_cast<int>(60.0f * fr / s.maxBpm));
    const int lagMax = std::min(static_cast<int>(len / 2) - 1, static_cast<int>(60.0f * fr / s.minBpm + 1.0f));
    if (lagMax <= lagMin) return;
    const int rMax = std::min(static_cast<int>(len) - 1, 2 * lagMax + 2);

    std::vector<float> r(rMax + 1, 0.0f);
    for (int l = 0; l <= rMax; l++) {
        float acc = 0;
        for (long k = 0; k + l < len; k++) acc += x[k] * x[k + l];
        r[l] = acc / static_cast<float>(len - l);
    }
    if (r[0] <= 1e-9f) return;

    // Score: lag + its double (comb), weighted by a log-tempo prior around preferredBpm
    auto rawScore = [&](float lag) {
        auto at = [&](float L) {
            const int a = static_cast<int>(L);
            if (a + 1 > rMax) return 0.0f;
            const float t = L - a;
            return r[a] + (r[a + 1] - r[a]) * t;
        };
        return at(lag) + 0.5f * at(2.0f * lag);
    };
    auto prior = [&](float bpmValue) {
        const float o = std::log2(bpmValue / s.preferredBpm) / 0.9f;
        return std::exp(-0.5f * o * o);
    };

    int best = lagMin;
    float bestScore = -1e30f;
    for (int l = lagMin; l <= lagMax; l++) {
        const float sc = rawScore(static_cast<float>(l)) * prior(60.0f * fr / l);
        if (sc > bestScore) {
            bestScore = sc;
            best = l;
        }
    }
    // Parabolic refinement
    float lag = static_cast<float>(best);
    if (best > lagMin && best < lagMax) {
        const float a = rawScore(best - 1.0f), b = rawScore(static_cast<float>(best)), c = rawScore(best + 1.0f);
        const float denom = a - 2 * b + c;
        if (std::fabs(denom) > 1e-9f) lag += std::clamp(0.5f * (a - c) / denom, -0.5f, 0.5f);
    }

    estimateBpm = 60.0f * fr / lag;
    estimateConfidence = std::clamp(rawScore(lag) / (1.5f * r[0]), 0.0f, 1.0f);

    // Tempo curve for display
    tempoCurveMinBpm = s.minBpm;
    const int nb = std::max(1, static_cast<int>(s.maxBpm - s.minBpm) + 1);
    tempoCurve.assign(nb, 0.0f);
    for (int b = 0; b < nb; b++) {
        const float bv = s.minBpm + b;
        const float L = 60.0f * fr / bv;
        if (L >= 1.0f && L <= rMax / 2.0f) tempoCurve[b] = std::max(0.0f, rawScore(L) / (1.5f * r[0])) * prior(bv);
    }

    // Agreement between consecutive estimates (needed to lock)
    if (lastEstimatePeriod_ > 0 && std::fabs(lag / lastEstimatePeriod_ - 1.0f) < 0.03f) agreeingEstimates_++;
    else agreeingEstimates_ = 0;
    lastEstimatePeriod_ = lag;

    if (silent) return;

    if (state == TrackState::Searching) {
        if (estimateConfidence >= s.acquireConfidence && agreeingEstimates_ >= 2) acquire(s, lag);
        return;
    }

    // Locked/coasting: does the estimate disagree with the locked tempo (ignoring 2x / 0.5x)?
    const float ratio = period_ / lag;
    const bool sameTempo = std::fabs(ratio - 1.0f) < 0.05f;
    // Related tempi (half/double, 2:3 / 3:2, third/triple) are metrical levels of the same beat:
    // breakdowns and fills make them win briefly, but they are no reason to re-lock.
    bool octave = false;
    for (const float r : {2.0f, 0.5f, 1.5f, 2.0f / 3.0f, 3.0f, 1.0f / 3.0f}) {
        if (std::fabs(ratio / r - 1.0f) < 0.04f) octave = true;
    }
    // Switching away from a lock needs a clearly better estimate than the first lock did
    const float relockConfidence = s.acquireConfidence + 0.15f;
    if (estimateConfidence >= relockConfidence && !sameTempo && !octave && agreeingEstimates_ >= 1) {
        mismatchSeconds += 24.0f / fr;
        if (mismatchSeconds >= s.relockSeconds) acquire(s, lag);
    } else {
        mismatchSeconds = 0;
    }

    // Locked on the wrong phase (e.g. off-beats)? Compare with the best fitting beat grid.
    if (state == TrackState::Locked) {
        float bestScore = 0;
        const double best = bestBeatFrame(period_, &bestScore);
        const double current = nextBeat_ - period_;
        double diff = std::fmod(best - current, static_cast<double>(period_));
        if (diff < 0) diff += period_;
        const double dist = std::min(diff, period_ - diff);
        if (dist > 0.2 * period_ && bestScore > 1.2f * combScore(current, period_)) {
            phaseMismatchSeconds += 24.0f / fr;
            if (phaseMismatchSeconds >= 0.5f * s.relockSeconds) {
                double nb = best;
                while (nb <= n_ - 1) nb += period_;
                nextBeat_ = nb;
                emitted_ = false;
                phaseMismatchSeconds = 0;
            }
        } else {
            phaseMismatchSeconds = 0;
        }
    }
}

void BeatTracker::acquire(const TrackerSettings& s, float period) {
    (void)s;
    // Best phase: the beat grid over the last 8 periods that collects the most (bass) onset energy
    const double best = bestBeatFrame(period, nullptr);
    period_ = period;
    lockPeriod_ = period;
    nextBeat_ = best + period;
    phaseMismatchSeconds = 0;
    emitted_ = false;
    confidence = 0.5f;
    mismatchSeconds = 0;
    state = TrackState::Locked;
    // New lock (new track / tempo): bar position and kick level start over
    beatIndex_ = 0;
    downbeatOffset_ = -1;
    downbeatManual = false;
    downbeatConfidence = 0;
    downbeatVotes.fill(0);
    votedBeats_ = 0;
    havePrevBands_ = false;
    bandAcc_.fill(0);
    kickAvg_ = 0;
    beatPresence = 0;
}

int BeatTracker::barPosition() const {
    if (downbeatOffset_ < 0 || beatIndex_ == 0 || state == TrackState::Searching) return -1;
    return static_cast<int>(((beatIndex_ - 1 - downbeatOffset_) % 4 + 4) % 4);
}

int BeatTracker::suggestedDownbeat() const {
    if (votedBeats_ < 16 || beatIndex_ == 0) return -1;
    int best = 0;
    for (int p = 1; p < 4; p++) {
        if (downbeatVotes[p] > downbeatVotes[best]) best = p;
    }
    // as a bar position of the last beat, like barPosition()
    return static_cast<int>(((beatIndex_ - 1 - best) % 4 + 4) % 4);
}

void BeatTracker::markDownbeat() {
    if (state == TrackState::Searching || period_ <= 0) return;
    // Pressed shortly after a beat (reaction time) -> that beat, otherwise the upcoming one
    const bool lastOne = lastBeatFrame_ >= 0 && (n_ - 1 - lastBeatFrame_) < 0.5f * period_;
    const long b = lastOne ? beatIndex_ - 1 : beatIndex_;
    downbeatOffset_ = static_cast<int>(((b % 4) + 4) % 4);
    downbeatManual = true;
}

void BeatTracker::beatEmitted() {
    beatIndex_++;

    // The spectrum of the beat interval that just ended vs the one before: a big change means
    // something new started at that interval's beat. New parts mostly start on the "1".
    std::array<float, kBands> v{};
    for (int b = 0; b < kBands; b++) v[b] = std::log1p(bandAcc_[b] * 1e-3f);
    bandAcc_.fill(0);
    if (havePrevBands_ && beatIndex_ >= 3) {
        float dot = 0, na = 0, nb = 0;
        for (int b = 0; b < kBands; b++) {
            dot += v[b] * prevBands_[b];
            na += v[b] * v[b];
            nb += prevBands_[b] * prevBands_[b];
        }
        const float change = (na > 0 && nb > 0) ? 1.0f - dot / std::sqrt(na * nb) : 0.0f;
        const int pos = static_cast<int>((beatIndex_ - 2) % 4);  // the beat that started the interval
        for (auto& vote : downbeatVotes) vote *= 0.995f;
        downbeatVotes[pos] += change * change * 1000.0f;
        votedBeats_++;

        int best = 0;
        for (int p = 1; p < 4; p++) {
            if (downbeatVotes[p] > downbeatVotes[best]) best = p;
        }
        float second = 0;
        for (int p = 0; p < 4; p++) {
            if (p != best) second = std::max(second, downbeatVotes[p]);
        }
        downbeatConfidence = downbeatVotes[best] > 0 ? 1.0f - second / downbeatVotes[best] : 0.0f;
        // Only with enough evidence, and only move an existing "1" if the new one is clearly better
        if (!downbeatManual && votedBeats_ >= 64 && downbeatConfidence > 0.5f) {
            if (downbeatOffset_ < 0 || downbeatVotes[best] > 1.5f * downbeatVotes[downbeatOffset_]) downbeatOffset_ = best;
        }
    }
    prevBands_ = v;
    havePrevBands_ = true;
}

void BeatTracker::updatePresence(float kick, bool quiet) {
    if (quiet) return;  // silence is handled per frame (silentSeconds), the caller mutes the pulse
    if (kickAvg_ <= 1e-6f) kickAvg_ = kick;
    // 1.25: a normal kick (with some jitter around the average) counts as fully present
    const float raw = kickAvg_ > 1e-6f ? std::min(1.0f, 1.25f * kick / kickAvg_) : 0.0f;
    // Weak or missing kicks inside the music fade the presence over a few beats (no hard drop;
    // only real silence does that), a returning kick brings it back a little slower.
    if (raw < beatPresence) beatPresence += (raw - beatPresence) * 0.3f;
    else beatPresence += (raw - beatPresence) * 0.25f;
    // Learn the usual kick level from clear beats; adapt slowly to a weaker (but present) kick
    if (raw > 0.6f) kickAvg_ += (kick - kickAvg_) * 0.06f;
    else if (raw >= 0.3f) kickAvg_ += (kick - kickAvg_) * 0.02f;
}

void BeatTracker::track(const TrackerSettings& s) {
    if (state == TrackState::Searching || period_ <= 0) return;
    const float fr = frameRate();
    const long cur = n_ - 1;  // newest frame

    // Emit the predicted beat when the audio reaches it. Onset frames describe the centre of the
    // FFT window, kFft/2 samples before the newest sample, so emit that much earlier.
    constexpr float kLatencyFrames = static_cast<float>(kFft / 2) / kHop;
    if (!emitted_ && cur >= nextBeat_ - kLatencyFrames) {
        emitted_ = true;
        beatCount++;
        beatsInHop_++;
        lastBeatFrame_ = cur;
        flags[cur % kHistory] |= FlagBeat;
        beatEmitted();
    }

    // Once the window around the prediction is complete, correct phase and tempo
    const float win = 0.15f * period_;
    if (cur < nextBeat_ + win + 1) return;

    long bestFrame = -1;
    float bestDist = 1e9f;
    for (long f = static_cast<long>(std::ceil(nextBeat_ - win)); f <= static_cast<long>(nextBeat_ + win); f++) {
        if (f < 0 || f >= n_ || f < n_ - kHistory) continue;
        if (flags[f % kHistory] & FlagOnset) {
            const float d = std::fabs(static_cast<float>(f - nextBeat_));
            if (d < bestDist) {
                bestDist = d;
                bestFrame = f;
            }
        }
    }

    bool quiet = silent;

    // Kick strength at this beat (the kick detector lags kickDelay_ frames)
    float kick = 0;
    for (long f = static_cast<long>(std::ceil(nextBeat_ - win)); f <= static_cast<long>(nextBeat_) + 2; f++) {
        if (f < 0 || f > n_ - 1 - kickDelay_ || f < n_ - kHistory) continue;
        kick = std::max(kick, odfPhase[f % kHistory]);
    }
    updatePresence(kick, quiet);

    double beatPos = nextBeat_;
    if (bestFrame >= 0) {
        const float e = static_cast<float>(bestFrame - nextBeat_);
        beatPos = nextBeat_ + s.phaseGain * e;
        period_ = std::clamp(period_ + s.tempoGain * e, lockPeriod_ * 0.9f, lockPeriod_ * 1.1f);
        confidence = confidence * 0.85f + 0.15f;
        flags[bestFrame % kHistory] |= FlagMatched;
        if (state == TrackState::Coasting && confidence >= 0.45f) state = TrackState::Locked;
    } else if (!quiet) {
        confidence *= 0.85f;
    }
    const long bp = static_cast<long>(beatPos + 0.5);
    if (bp >= 0 && bp < n_ && bp >= n_ - kHistory) flags[bp % kHistory] |= FlagCorrected;

    nextBeat_ = beatPos + period_;
    emitted_ = false;

    // State changes (soft lock): silence or weak matches only make us coast on the last tempo.
    // A new tempo takes over via the re-lock check in estimateTempo(); coasting too long gives up.
    if (state == TrackState::Locked && (quiet || confidence < 0.3f)) {
        state = TrackState::Coasting;
        coastSince_ = n_;
    }
    if (state == TrackState::Coasting && (n_ - coastSince_) / fr > s.coastSeconds) {
        state = TrackState::Searching;
        agreeingEstimates_ = 0;
    }
}
