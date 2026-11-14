#include "audio_capture.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>

#ifdef _WIN32
#include <windows.h>
#endif

// Keep at most ~1 s of stereo audio if the renderer stalls.
static constexpr size_t kMaxBufferedSamples = 48000 * 2;

// Seconds on the clock WASAPI uses for its capture timestamps (QueryPerformanceCounter).
static double clockSeconds() {
#ifdef _WIN32
    LARGE_INTEGER c, f;
    QueryPerformanceCounter(&c);
    QueryPerformanceFrequency(&f);
    return static_cast<double>(c.QuadPart) / static_cast<double>(f.QuadPart);
#else
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
#endif
}

AudioCapture::AudioCapture() : thread_([this] { run(); }) {}

AudioCapture::~AudioCapture() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

void AudioCapture::drain(std::vector<float>& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    out.swap(buffer_);
    buffer_.clear();
    if (!out.empty() && newestSampleTime_ > 0) {
        captureAgeMs_ = static_cast<float>((clockSeconds() - newestSampleTime_) * 1000.0);
    }
}

std::string AudioCapture::deviceName() {
    std::lock_guard<std::mutex> lock(mutex_);
    return deviceName_;
}

void AudioCapture::setDeviceName(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    deviceName_ = name;
}

void AudioCapture::push(const float* stereo, size_t frames, double endTime) {
    std::lock_guard<std::mutex> lock(mutex_);
    newestSampleTime_ = endTime;
    buffer_.insert(buffer_.end(), stereo, stereo + frames * 2);
    if (buffer_.size() > kMaxBufferedSamples) {
        buffer_.erase(buffer_.begin(), buffer_.end() - kMaxBufferedSamples);
    }
}

#ifdef _WIN32

// Defines (not just declares) PKEY_Device_FriendlyName and the KSDATAFORMAT GUIDs.
#include <initguid.h>

// Order matters: mmdeviceapi.h pulls in propkeydef.h, which defines DEFINE_PROPERTYKEY.
// clang-format off
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ksmedia.h>
// clang-format on

namespace {

template <typename T>
void release(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

std::string toUtf8(const wchar_t* w) {
    if (!w) return {};
    const int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1) return {};
    std::string s(static_cast<size_t>(len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), len, nullptr, nullptr);
    return s;
}

std::wstring defaultDeviceId(IMMDeviceEnumerator* enumerator) {
    IMMDevice* device = nullptr;
    if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device))) return {};
    LPWSTR id = nullptr;
    std::wstring result;
    if (SUCCEEDED(device->GetId(&id))) {
        result = id;
        CoTaskMemFree(id);
    }
    release(device);
    return result;
}

std::string friendlyName(IMMDevice* device) {
    IPropertyStore* props = nullptr;
    if (FAILED(device->OpenPropertyStore(STGM_READ, &props))) return "unknown device";
    PROPVARIANT value;
    PropVariantInit(&value);
    std::string name = "unknown device";
    if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &value)) && value.vt == VT_LPWSTR) {
        name = toUtf8(value.pwszVal);
    }
    PropVariantClear(&value);
    release(props);
    return name;
}

enum class SampleType { Float32, Int16, Int32, Unsupported };

SampleType sampleType(const WAVEFORMATEX* fmt) {
    GUID sub{};
    if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        sub = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(fmt)->SubFormat;
    } else if (fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        sub = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    } else if (fmt->wFormatTag == WAVE_FORMAT_PCM) {
        sub = KSDATAFORMAT_SUBTYPE_PCM;
    }

    if (sub == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT && fmt->wBitsPerSample == 32) return SampleType::Float32;
    if (sub == KSDATAFORMAT_SUBTYPE_PCM && fmt->wBitsPerSample == 16) return SampleType::Int16;
    if (sub == KSDATAFORMAT_SUBTYPE_PCM && fmt->wBitsPerSample == 32) return SampleType::Int32;
    return SampleType::Unsupported;
}

float readSample(const BYTE* p, SampleType type) {
    switch (type) {
        case SampleType::Float32: return *reinterpret_cast<const float*>(p);
        case SampleType::Int16: return *reinterpret_cast<const int16_t*>(p) / 32768.0f;
        case SampleType::Int32: return *reinterpret_cast<const int32_t*>(p) / 2147483648.0f;
        default: return 0.0f;
    }
}

}  // namespace

void AudioCapture::run() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    IMMDeviceEnumerator* enumerator = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator),
                                reinterpret_cast<void**>(&enumerator)))) {
        std::cerr << "Audio: cannot create device enumerator\n";
        CoUninitialize();
        return;
    }

    std::vector<float> stereo;

    while (running_) {
        IMMDevice* device = nullptr;
        IAudioClient* client = nullptr;
        IAudioCaptureClient* capture = nullptr;
        WAVEFORMATEX* fmt = nullptr;

        const std::wstring deviceId = defaultDeviceId(enumerator);
        bool ok = !deviceId.empty() &&
                  SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device)) &&
                  SUCCEEDED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                             reinterpret_cast<void**>(&client))) &&
                  SUCCEEDED(client->GetMixFormat(&fmt));

        SampleType type = SampleType::Unsupported;
        if (ok) {
            type = sampleType(fmt);
            ok = type != SampleType::Unsupported;
            if (!ok) std::cerr << "Audio: unsupported mix format (" << fmt->wBitsPerSample << " bit)\n";
        }

        // 1 s buffer, in 100 ns units. Shared mode is required for loopback.
        ok = ok &&
             SUCCEEDED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK,
                                          10000000, 0, fmt, nullptr)) &&
             SUCCEEDED(client->GetService(__uuidof(IAudioCaptureClient),
                                          reinterpret_cast<void**>(&capture))) &&
             SUCCEEDED(client->Start());

        if (ok) {
            const std::string name = friendlyName(device);
            setDeviceName(name);
            sampleRate_ = static_cast<int>(fmt->nSamplesPerSec);
            std::cout << "Audio: capturing \"" << name << "\" (" << fmt->nSamplesPerSec << " Hz, "
                      << fmt->nChannels << " ch, " << fmt->wBitsPerSample << " bit)\n";

            const int channels = fmt->nChannels;
            const int bytesPerSample = fmt->wBitsPerSample / 8;
            auto lastDeviceCheck = std::chrono::steady_clock::now();

            while (running_) {
                Sleep(5);

                // Follow a change of the default output device (e.g. Scarlett -> headset).
                const auto now = std::chrono::steady_clock::now();
                if (now - lastDeviceCheck > std::chrono::seconds(1)) {
                    lastDeviceCheck = now;
                    if (defaultDeviceId(enumerator) != deviceId) {
                        std::cout << "Audio: default device changed\n";
                        break;
                    }
                }

                UINT32 packet = 0;
                if (FAILED(capture->GetNextPacketSize(&packet))) break;  // e.g. device unplugged

                bool failed = false;
                while (packet > 0) {
                    BYTE* data = nullptr;
                    UINT32 frames = 0;
                    DWORD flags = 0;
                    UINT64 qpcPosition = 0;  // capture time of the first frame, 100 ns units
                    if (FAILED(capture->GetBuffer(&data, &frames, &flags, nullptr, &qpcPosition))) {
                        failed = true;
                        break;
                    }

                    stereo.resize(static_cast<size_t>(frames) * 2);
                    if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                        std::fill(stereo.begin(), stereo.end(), 0.0f);
                    } else {
                        for (UINT32 f = 0; f < frames; f++) {
                            const BYTE* frame = data + static_cast<size_t>(f) * channels * bytesPerSample;
                            const float l = readSample(frame, type);
                            const float r = channels > 1 ? readSample(frame + bytesPerSample, type) : l;
                            stereo[f * 2] = l;
                            stereo[f * 2 + 1] = r;
                        }
                    }
                    const double rate = static_cast<double>(fmt->nSamplesPerSec);
                    const double endTime = qpcPosition > 0 ? qpcPosition / 1e7 + frames / rate : clockSeconds();
                    push(stereo.data(), frames, endTime);

                    capture->ReleaseBuffer(frames);
                    if (FAILED(capture->GetNextPacketSize(&packet))) {
                        failed = true;
                        break;
                    }
                }
                if (failed) break;
            }

            client->Stop();
        } else if (running_) {
            Sleep(1000);  // no usable device yet, retry
        }

        setDeviceName({});
        if (fmt) CoTaskMemFree(fmt);
        release(capture);
        release(client);
        release(device);
    }

    release(enumerator);
    CoUninitialize();
}

#else

void AudioCapture::run() {
    std::cerr << "Audio: system capture is only implemented on Windows\n";
}

#endif
