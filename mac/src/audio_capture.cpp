#include "audio_capture.h"
#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <cstdio>
#include <cstring>

static std::wstring utf8ToWide(const std::string& s) {
    std::wstring ws;
    ws.reserve(s.size());
    for (unsigned char c : s) ws += (wchar_t)c;
    return ws;
}

static std::string wideToUtf8(const std::wstring& ws) {
    std::string s;
    s.reserve(ws.size());
    for (wchar_t wc : ws) {
        if (wc < 0x80) s += (char)wc;
        else s += '?';
    }
    return s;
}

AudioCapture::AudioCapture() {}

AudioCapture::~AudioCapture() {
    stop();
    cleanup();
}

std::vector<AudioDeviceInfo> AudioCapture::getDevices() {
    std::vector<AudioDeviceInfo> devices;

    AudioObjectPropertyAddress prop = {
        kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };

    UInt32 dataSize = 0;
    OSStatus status = AudioObjectGetPropertyDataSize(
        kAudioObjectSystemObject, &prop, 0, nullptr, &dataSize);
    if (status != noErr) return devices;

    int deviceCount = dataSize / sizeof(AudioDeviceID);
    std::vector<AudioDeviceID> deviceIds(deviceCount);

    status = AudioObjectGetPropertyData(
        kAudioObjectSystemObject, &prop, 0, nullptr, &dataSize, deviceIds.data());
    if (status != noErr) return devices;

    for (AudioDeviceID devId : deviceIds) {
        // Check if device has input streams
        AudioObjectPropertyAddress streamProp = {
            kAudioDevicePropertyStreams,
            kAudioDevicePropertyScopeInput,
            kAudioObjectPropertyElementMain
        };

        UInt32 streamSize = 0;
        status = AudioObjectGetPropertyDataSize(devId, &streamProp, 0, nullptr, &streamSize);
        if (status != noErr || streamSize == 0) continue;

        // Get device name
        CFStringRef nameRef = nullptr;
        UInt32 nameSize = sizeof(nameRef);
        AudioObjectPropertyAddress nameProp = {
            kAudioObjectPropertyName,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        status = AudioObjectGetPropertyData(devId, &nameProp, 0, nullptr, &nameSize, &nameRef);
        if (status != noErr || !nameRef) continue;

        char nameBuf[256];
        CFStringGetCString(nameRef, nameBuf, sizeof(nameBuf), kCFStringEncodingUTF8);
        CFRelease(nameRef);

        AudioDeviceInfo info;
        info.id = utf8ToWide(std::to_string(devId));
        info.name = utf8ToWide(nameBuf);
        devices.push_back(info);
    }

    return devices;
}

bool AudioCapture::initialize(const std::wstring& deviceId) {
    if (!deviceId.empty()) {
        try {
            deviceId_ = std::stoul(wideToUtf8(deviceId));
        } catch (...) {
            deviceId_ = 0;
        }
    }

    if (deviceId_ == 0) {
        // Get default input device
        AudioObjectPropertyAddress prop = {
            kAudioHardwarePropertyDefaultInputDevice,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        UInt32 size = sizeof(deviceId_);
        AudioObjectGetPropertyData(kAudioObjectSystemObject, &prop, 0, nullptr, &size, &deviceId_);
    }

    if (deviceId_ == 0) {
        printf("No input device found\n");
        return false;
    }

    printf("Using input device ID: %u\n", deviceId_);
    return true;
}

void AudioCapture::cleanup() {
    stop();
    if (audioUnit_) {
        AudioComponentInstance au = (AudioComponentInstance)audioUnit_;
        AudioUnitUninitialize(au);
        AudioComponentInstanceDispose(au);
        audioUnit_ = nullptr;
    }
}

// CoreAudio input callback
static OSStatus caInputCallback(void* inRefCon,
                                 AudioUnitRenderActionFlags* ioActionFlags,
                                 const AudioTimeStamp* inTimeStamp,
                                 UInt32 inBusNumber,
                                 UInt32 inNumberFrames,
                                 AudioBufferList* ioData) {
    AudioCapture* capture = (AudioCapture*)inRefCon;
    AudioComponentInstance au = (AudioComponentInstance)capture->audioUnit_;

    // Allocate buffer for capture
    AudioBufferList bufList;
    bufList.mNumberBuffers = 1;
    bufList.mBuffers[0].mDataByteSize = inNumberFrames * 2 * sizeof(float); // stereo
    bufList.mBuffers[0].mNumberChannels = 2;

    std::vector<float> buffer(inNumberFrames * 2);
    bufList.mBuffers[0].mData = buffer.data();

    OSStatus status = AudioUnitRender(au, ioActionFlags, inTimeStamp,
                                       inBusNumber, inNumberFrames, &bufList);
    if (status != noErr) return status;

    if (capture->callback_ && capture->running_) {
        // Deliver in 480-sample chunks (10ms at 48kHz)
        const float* data = buffer.data();
        uint32_t remaining = inNumberFrames;
        while (remaining >= 480) {
            capture->callback_(data, 480);
            data += 480 * 2;
            remaining -= 480;
        }
    }

    return noErr;
}

bool AudioCapture::start(FrameCallback callback) {
    if (running_) return false;
    callback_ = callback;

    // Set up AudioUnit for input
    AudioComponentDescription desc = {
        kAudioUnitType_Output,
        kAudioUnitSubType_HALOutput,
        kAudioUnitManufacturer_Apple,
        0, 0
    };

    AudioComponent comp = AudioComponentFindNext(nullptr, &desc);
    if (!comp) {
        printf("Failed to find HAL output component\n");
        return false;
    }

    AudioComponentInstance au;
    OSStatus status = AudioComponentInstanceNew(comp, &au);
    if (status != noErr) {
        printf("Failed to create audio unit: %d\n", (int)status);
        return false;
    }
    audioUnit_ = au;

    // Enable input, disable output
    UInt32 enableIO = 1;
    AudioUnitSetProperty(au, kAudioOutputUnitProperty_EnableIO,
                         kAudioUnitScope_Input, 1, &enableIO, sizeof(enableIO));
    enableIO = 0;
    AudioUnitSetProperty(au, kAudioOutputUnitProperty_EnableIO,
                         kAudioUnitScope_Output, 0, &enableIO, sizeof(enableIO));

    // Set device
    AudioUnitSetProperty(au, kAudioOutputUnitProperty_CurrentDevice,
                         kAudioUnitScope_Global, 0, &deviceId_, sizeof(deviceId_));

    // Set format: 48kHz stereo float
    AudioStreamBasicDescription fmt = {};
    fmt.mSampleRate = 48000;
    fmt.mFormatID = kAudioFormatLinearPCM;
    fmt.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    fmt.mBitsPerChannel = 32;
    fmt.mChannelsPerFrame = 2;
    fmt.mFramesPerPacket = 1;
    fmt.mBytesPerFrame = 8;
    fmt.mBytesPerPacket = 8;

    AudioUnitSetProperty(au, kAudioUnitProperty_StreamFormat,
                         kAudioUnitScope_Output, 1, &fmt, sizeof(fmt));

    // Set callback
    AURenderCallbackStruct callbackStruct;
    callbackStruct.inputProc = caInputCallback;
    callbackStruct.inputProcRefCon = this;
    AudioUnitSetProperty(au, kAudioOutputUnitProperty_SetInputCallback,
                         kAudioUnitScope_Global, 0, &callbackStruct, sizeof(callbackStruct));

    status = AudioUnitInitialize(au);
    if (status != noErr) {
        printf("Failed to initialize audio unit: %d\n", (int)status);
        return false;
    }

    running_ = true;

    status = AudioOutputUnitStart(au);
    if (status != noErr) {
        printf("Failed to start audio unit: %d\n", (int)status);
        running_ = false;
        return false;
    }

    printf("Audio capture started (48kHz stereo)\n");
    return true;
}

void AudioCapture::stop() {
    running_ = false;
    if (audioUnit_) {
        AudioOutputUnitStop((AudioComponentInstance)audioUnit_);
    }
}
