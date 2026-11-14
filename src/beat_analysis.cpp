// Ported from the MilkDrop 2.25c source (BSD license, Copyright 2005-2013 Nullsoft, Inc.).
// See THIRD_PARTY_NOTICES.md for the full license text.

#include "beat_analysis.h"

#include <algorithm>
#include <cmath>
#include <random>
#include <utility>

namespace {

constexpr float kPi = 3.14159265358979f;

// utility.cpp AdjustRateToFPS
float adjustRateToFps(float perFrameDecayAtFps1, float fps1, float actualFps) {
    const float perSecond = std::pow(perFrameDecayAtFps1, fps1);
    return std::pow(perSecond, 1.0f / actualFps);
}

}  // namespace

BeatAnalysis::BeatAnalysis() : ring_(kMaxWindow, 0.0f) {
    configure(576);
    avg.fill(0);
    longAvg.fill(0);
    immRel.fill(1);
    avgRel.fill(1);
}

// FFT::Init(samples_in, samples_out, bEqualize=-1, envelope_power=1.0) as MilkDrop calls it
// with (576, 512): NFREQ = 1024, equalizer and envelope on. Larger windows use the next power of 2.
void BeatAnalysis::configure(int window) {
    window_ = std::clamp(window, 64, kMaxWindow);
    n_ = 1024;
    while (n_ < window_ * 16 / 9) n_ <<= 1;  // 576 -> 1024, 1024 -> 2048, 2048 -> 4096

    bitrev_.resize(n_);
    for (int i = 0; i < n_; i++) bitrev_[i] = i;
    for (int i = 0, j = 0; i < n_; i++) {
        if (j > i) std::swap(bitrev_[i], bitrev_[j]);
        int m = n_ >> 1;
        while (m >= 1 && j >= m) {
            j -= m;
            m >>= 1;
        }
        j += m;
    }
    cossin_.clear();
    for (int dftsize = 2; dftsize <= n_; dftsize <<= 1) {
        const float theta = -2.0f * kPi / static_cast<float>(dftsize);
        cossin_.push_back({std::cos(theta), std::sin(theta)});
    }
    envelope_.resize(window_);
    const float mult = 1.0f / window_ * 6.2831853f;
    for (int i = 0; i < window_; i++) envelope_[i] = 0.5f + 0.5f * std::sin(i * mult - 1.5707963268f);
    const int half = n_ / 2;
    equalize_.resize(half);
    for (int i = 0; i < half; i++) equalize_[i] = -0.02f * std::log(static_cast<float>(half - i) / half);

    spectrum.assign(half, 0.0f);
    real_.resize(n_);
    imag_.resize(n_);
    wave_.resize(window_);
}

void BeatAnalysis::addSamples(const float* stereo, size_t frames, bool stereoMix) {
    // Scaled to the 8-bit range MilkDrop worked with (-128..127).
    for (size_t f = 0; f < frames; f++) {
        const float v = stereoMix ? 0.5f * (stereo[f * 2] + stereo[f * 2 + 1]) : stereo[f * 2];
        ring_[ringPos_] = v * 128.0f;
        ringPos_ = (ringPos_ + 1) % kMaxWindow;
    }
}

void BeatAnalysis::fft(const float* in, float* out) {
    for (int i = 0; i < n_; i++) {
        const int idx = bitrev_[i];
        real_[i] = idx < window_ ? in[idx] * envelope_[idx] : 0.0f;
        imag_[i] = 0.0f;
    }
    int t = 0;
    for (int dftsize = 2; dftsize <= n_; dftsize <<= 1, t++) {
        const float wpr = cossin_[t][0];
        const float wpi = cossin_[t][1];
        float wr = 1.0f, wi = 0.0f;
        const int half = dftsize >> 1;
        for (int m = 0; m < half; m++) {
            for (int i = m; i < n_; i += dftsize) {
                const int j = i + half;
                const float tr = wr * real_[j] - wi * imag_[j];
                const float ti = wr * imag_[j] + wi * real_[j];
                real_[j] = real_[i] - tr;
                imag_[j] = imag_[i] - ti;
                real_[i] += tr;
                imag_[i] += ti;
            }
            const float wtemp = wr;
            wr = wr * wpr - wi * wpi;
            wi = wi * wpr + wtemp * wpi;
        }
    }
    for (int i = 0; i < n_ / 2; i++) out[i] = equalize_[i] * std::sqrt(real_[i] * real_[i] + imag_[i] * imag_[i]);
}

bool BeatAnalysis::update(float fps, double now, bool locked, const BeatSettings& s) {
    if (fps < 1.0f) fps = 30.0f;
    if (s.windowSamples != window_) configure(s.windowSamples);

    // Newest `window_` samples in chronological order
    for (int i = 0; i < window_; i++) {
        wave_[i] = ring_[(ringPos_ + kMaxWindow - window_ + i) % kMaxWindow];
    }
    fft(wave_.data(), spectrum.data());

    // MilkDrop sums the bottom half of its 512 bins in 3 equal parts: 0-11025 Hz at 44.1 kHz,
    // i.e. 0-3675-7350-11025 Hz. Convert those frequencies to bins for any rate / FFT size.
    const float binHz = static_cast<float>(sampleRate_) / n_;
    for (int b = 0; b <= 3; b++) {
        bandEdges[b] = std::min(n_ / 2, static_cast<int>(b * 3675.0f / binHz + 0.5f));
    }
    // Keep absolute levels comparable between window sizes (576 = MilkDrop)
    const float norm = 576.0f / window_;

    for (int i = 0; i < 3; i++) {
        imm[i] = 0;
        for (int j = bandEdges[i]; j < bandEdges[i + 1]; j++) imm[i] += spectrum[j];
        imm[i] *= norm;

        float rate = imm[i] > avg[i] ? 0.2f : 0.5f;
        rate = adjustRateToFps(rate, 30.0f, fps);
        avg[i] = avg[i] * rate + imm[i] * (1 - rate);

        rate = frame_ < 50 ? 0.9f : 0.992f;
        rate = adjustRateToFps(rate, 30.0f, fps);
        longAvg[i] = longAvg[i] * rate + imm[i] * (1 - rate);

        immRel[i] = std::fabs(longAvg[i]) < 0.001f ? 1.0f : imm[i] / longAvg[i];
        avgRel[i] = std::fabs(longAvg[i]) < 0.001f ? 1.0f : avg[i] / longAvg[i];
    }

    // Hard cuts (milkdropfs.cpp)
    bool cut = false;
    if (frame_ == 0) hardCutThresh = s.hardCutLoudnessThresh * 2.0f;
    if (s.hardCutsEnabled && !locked) {
        if (immRel[0] + immRel[1] + immRel[2] > hardCutThresh * 3.0f) {
            cut = true;
            hardCutThresh *= 2.0f;
        } else {
            const float k = -1.3863f / (s.hardCutHalflife * fps);
            hardCutThresh = (hardCutThresh - s.hardCutLoudnessThresh) * std::exp(k) + s.hardCutLoudnessThresh;
        }
    }

    if (nextPresetTime_ < 0) restartTimer(now, s);
    frame_++;
    return cut;
}

void BeatAnalysis::restartTimer(double now, const BeatSettings& s) {
    static std::mt19937 rng{std::random_device{}()};
    const float dt = s.timeBetweenPresetsRand * std::uniform_real_distribution<float>(0.0f, 1.0f)(rng);
    nextPresetTime_ = now + s.blendTimeAuto + s.timeBetweenPresets + dt;
}
