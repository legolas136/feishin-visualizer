#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Captures what the default Windows output device plays (WASAPI loopback) and
// follows changes of the default device. On other platforms it captures nothing.
class AudioCapture {
public:
    AudioCapture();
    ~AudioCapture();

    AudioCapture(const AudioCapture&) = delete;
    AudioCapture& operator=(const AudioCapture&) = delete;

    // Moves all captured interleaved stereo float samples into `out` (cleared first).
    void drain(std::vector<float>& out);

    // Age of the newest drained sample when drain() was called (Windows timestamps), in ms.
    float captureAgeMs() const { return captureAgeMs_; }

    std::string deviceName();
    int sampleRate() const { return sampleRate_; }

private:
    void run();
    // endTime: capture time of the sample after the last one, in seconds (QPC clock)
    void push(const float* stereo, size_t frames, double endTime);
    void setDeviceName(const std::string& name);

    std::atomic<bool> running_{true};
    std::atomic<int> sampleRate_{48000};
    std::atomic<float> captureAgeMs_{0};
    double newestSampleTime_ = 0;
    std::mutex mutex_;
    std::vector<float> buffer_;
    std::string deviceName_;
    std::thread thread_;
};
