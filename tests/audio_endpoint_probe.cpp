// Manual hardware validation: observe samples without recording or saving audio.
#include "../windows/src/audio_capture.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

int wmain(int argc, wchar_t** argv) {
    if (argc < 2 || argc > 4) {
        std::puts("Usage: audio_endpoint_probe <capture device name substring> [seconds] [--silence]");
        return 2;
    }
    const int seconds = argc > 2 ? _wtoi(argv[2]) : 5;
    if (seconds < 2 || seconds > 600) return 2;
    const bool silence = argc > 3 && std::wstring(argv[3]) == L"--silence";
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(com)) return 2;
    int result = 2;
    {
        AudioCapture capture;
        std::wstring deviceId;
        for (const auto& device : AudioCapture::getDevices(false)) {
            if (device.name.find(argv[1]) != std::wstring::npos) {
                if (!deviceId.empty()) { std::puts("Ambiguous device name; use a more specific name."); CoUninitialize(); return 2; }
                deviceId = device.id;
            }
        }
        if (deviceId.empty() || !capture.initialize(deviceId, false)) {
            std::puts("Requested recording endpoint is unavailable.");
        } else {
            uint64_t frames = 0;
            double squared = 0, peak = 0;
            if (capture.start([&](const float* samples, uint32_t count) {
                frames += count;
                for (uint32_t i = 0; i < count * 2; ++i) {
                    squared += double(samples[i]) * samples[i];
                    peak = (std::max)(peak, std::abs(double(samples[i])));
                }
            })) {
                std::this_thread::sleep_for(std::chrono::seconds(seconds));
                capture.stop(); // Joins the callback thread before reading totals.
                const double rms = frames ? std::sqrt(squared / (frames * 2)) : 0;
                const bool enough = frames >= uint64_t(seconds - 1) * 48000;
                const bool level = silence ? peak < 0.00001 : peak > 0.0001;
                std::printf("frames=%llu seconds=%d peak=%.8f rms=%.8f expected=%s result=%s\n",
                    static_cast<unsigned long long>(frames), seconds, peak, rms,
                    silence ? "silence" : "signal", enough && level ? "PASS" : "FAIL");
                result = enough && level ? 0 : 1;
            }
        }
    }
    CoUninitialize();
    return result;
}
