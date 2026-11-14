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

BeatAnalysis::BeatAnalysis() : ring_(kSamples, 0.0f) {
    // FFT::Init(576, MY_FFT_SAMPLES, -1): equalize on (-1 != 0), envelope power 1.0
    for (int i = 0; i < kN; i++) bitrev_[i] = i;
    for (int i = 0, j = 0; i < kN; i++) {
        if (j > i) std::swap(bitrev_[i], bitrev_[j]);
        int m = kN >> 1;
        while (m >= 1 && j >= m) {
            j -= m;
            m >>= 1;
        }
        j += m;
    }
    for (int dftsize = 2; dftsize <= kN; dftsize <<= 1) {
        const float theta = -2.0f * kPi / static_cast<float>(dftsize);
        cossin_.push_back({std::cos(theta), std::sin(theta)});
    }
    const float mult = 1.0f / kSamples * 6.2831853f;
    for (int i = 0; i < kSamples; i++) envelope_[i] = 0.5f + 0.5f * std::sin(i * mult - 1.5707963268f);
    for (int i = 0; i < kFreqs; i++) equalize_[i] = -0.02f * std::log(static_cast<float>(kFreqs - i) / kFreqs);

    avg.fill(0);
    longAvg.fill(0);
    immRel.fill(1);
    avgRel.fill(1);
}

void BeatAnalysis::addSamples(const float* stereo, size_t frames, bool stereoMix) {
    // We keep the newest 576 samples, like Winamp hands them to the plugin each frame.
    // Scaled to the 8-bit range MilkDrop worked with (-128..127).
    for (size_t f = 0; f < frames; f++) {
        const float v = stereoMix ? 0.5f * (stereo[f * 2] + stereo[f * 2 + 1]) : stereo[f * 2];
        ring_[ringPos_] = v * 128.0f;
        ringPos_ = (ringPos_ + 1) % kSamples;
    }
}

void BeatAnalysis::fft(const float* in, float* out) {
    float real[kN], imag[kN];
    for (int i = 0; i < kN; i++) {
        const int idx = bitrev_[i];
        real[i] = idx < kSamples ? in[idx] * envelope_[idx] : 0.0f;
        imag[i] = 0.0f;
    }
    int t = 0;
    for (int dftsize = 2; dftsize <= kN; dftsize <<= 1, t++) {
        const float wpr = cossin_[t][0];
        const float wpi = cossin_[t][1];
        float wr = 1.0f, wi = 0.0f;
        const int half = dftsize >> 1;
        for (int m = 0; m < half; m++) {
            for (int i = m; i < kN; i += dftsize) {
                const int j = i + half;
                const float tr = wr * real[j] - wi * imag[j];
                const float ti = wr * imag[j] + wi * real[j];
                real[j] = real[i] - tr;
                imag[j] = imag[i] - ti;
                real[i] += tr;
                imag[i] += ti;
            }
            const float wtemp = wr;
            wr = wr * wpr - wi * wpi;
            wi = wi * wpr + wtemp * wpi;
        }
    }
    for (int i = 0; i < kFreqs; i++) out[i] = equalize_[i] * std::sqrt(real[i] * real[i] + imag[i] * imag[i]);
}

bool BeatAnalysis::update(float fps, double now, bool locked, const BeatSettings& s) {
    if (fps < 1.0f) fps = 30.0f;

    // Waveform in chronological order
    float wave[kSamples];
    for (int i = 0; i < kSamples; i++) wave[i] = ring_[(ringPos_ + i) % kSamples];
    fft(wave, spectrum.data());

    // MilkDrop sums the bottom half of the spectrum in 3 equal parts. With 576 samples
    // padded to 1024 that is 0-11025 Hz at 44.1 kHz; scale the edges for other rates.
    const float scale = 44100.0f / static_cast<float>(sampleRate_);
    for (int b = 0; b <= 3; b++) {
        bandEdges[b] = std::min(kFreqs, static_cast<int>(kFreqs * b / 6.0f * scale + 0.5f));
    }

    for (int i = 0; i < 3; i++) {
        imm[i] = 0;
        for (int j = bandEdges[i]; j < bandEdges[i + 1]; j++) imm[i] += spectrum[j];

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
