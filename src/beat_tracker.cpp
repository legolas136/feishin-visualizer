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
      prevLog_(kFft / 2, 0.0f) {
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
    silent = rms < 1e-4f || rms < longRms_ * 0.08f;

    // --- Log-compressed spectral flux, full band (to ~10 kHz) and bass band (30-180 Hz)
    const float binHz = static_cast<float>(sampleRate_) / kFft;
    const int kMax = std::min(kFft / 2 - 1, static_cast<int>(10000.0f / binHz));
    const int lowLo = std::max(1, static_cast<int>(30.0f / binHz));
    const int lowHi = std::max(lowLo + 1, static_cast<int>(180.0f / binHz + 1.0f));
    float fluxFull = 0, fluxLow = 0;
    for (int k = 1; k <= kMax; k++) {
        const float mag = std::abs(buf_[k]) / (kFft * 0.25f);
        const float l = std::log1p(1000.0f * mag);
        const float d = l - prevLog_[k];
        prevLog_[k] = l;
        if (d > 0) {
            fluxFull += d;
            if (k >= lowLo && k <= lowHi) fluxLow += d;
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
        odfPhase[i] = 0.75f * low + 0.25f * full;
    } else {
        odfPhase[i] = 0;
    }
    odf[i] = value;
    flags[i] = silent ? FlagSilent : 0;

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
