#pragma once

// Sound analysis and hard-cut / auto-switch timing modelled on MilkDrop 2.25c
// (CPlugin::DoCustomSoundAnalysis, the FFT class and the hard-cut logic in
// milkdropfs.cpp). Ported from the BSD-licensed MilkDrop 2 source,
// Copyright 2005-2013 Nullsoft, Inc. See THIRD_PARTY_NOTICES.md.

#include <array>
#include <cstddef>
#include <vector>

struct BeatSettings {
    // MilkDrop defaults (plugin.cpp MyPreInitialize)
    float timeBetweenPresets = 16.0f;      // fTimeBetweenPresets (s)
    float timeBetweenPresetsRand = 10.0f;  // fTimeBetweenPresetsRand (s, random extra)
    float blendTimeAuto = 2.7f;            // fBlendTimeAuto (s)
    float blendTimeUser = 1.7f;            // fBlendTimeUser (s, Space / menu)
    bool hardCutsEnabled = false;          // !bHardCutsDisabled (MilkDrop default: disabled)
    float hardCutLoudnessThresh = 2.5f;    // fHardCutLoudnessThresh
    float hardCutHalflife = 60.0f;         // fHardCutHalflife (s)
    bool stereoMix = false;                // false = left channel only, like MilkDrop
    int windowSamples = 576;               // samples per analysis (MilkDrop/Winamp: 576)
};

class BeatAnalysis {
public:
    static constexpr int kMaxWindow = 2048;

    BeatAnalysis();

    // Feed newly captured interleaved stereo samples (-1..1).
    void addSamples(const float* stereo, size_t frames, bool stereoMix);

    // Run once per rendered frame. Returns true if a hard cut should happen now.
    bool update(float fps, double now, bool locked, const BeatSettings& s);

    // True when the auto-switch timer expired. Call restartTimer() after switching.
    bool presetTimeUp(double now) const { return now >= nextPresetTime_; }
    void restartTimer(double now, const BeatSettings& s);

    void setSampleRate(int rate) { sampleRate_ = rate > 0 ? rate : 44100; }

    // Current values (as MilkDrop presets see them)
    std::array<float, 3> imm{}, avg{}, longAvg{}, immRel{}, avgRel{};
    float hardCutThresh = 0;  // current (decaying) threshold; a cut fires when sum(immRel) > 3 * this
    std::vector<float> spectrum;    // FFT output, 0 Hz .. sampleRate/2
    std::array<int, 4> bandEdges{};  // spectrum bin indices: bass [0,1), mid [1,2), treb [2,3)
    double nextPresetTime() const { return nextPresetTime_; }

private:
    void configure(int window);
    void fft(const float* in, float* out);

    std::vector<float> ring_;  // newest kMaxWindow samples
    size_t ringPos_ = 0;
    int sampleRate_ = 44100;
    long frame_ = 0;
    double nextPresetTime_ = -1;

    // FFT tables (fft.cpp), rebuilt when the window size changes
    int window_ = 0;  // samples in
    int n_ = 0;       // NFREQ (FFT size)
    std::vector<int> bitrev_;
    std::vector<std::array<float, 2>> cossin_;
    std::vector<float> envelope_;
    std::vector<float> equalize_;
    std::vector<float> real_, imag_, wave_;
};
