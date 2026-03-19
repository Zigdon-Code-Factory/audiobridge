#include "audio_render.h"
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

AudioRender::AudioRender() {}

AudioRender::~AudioRender() {
    stop();
    cleanup();
}

std::vector<std::pair<std::wstring, std::wstring>> AudioRender::getDevices() {
    std::vector<std::pair<std::wstring, std::wstring>> devices;

    AudioObjectPropertyAddress prop = {
        kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };

    UInt32 dataSize = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &prop, 0, nullptr, &dataSize) != noErr)
        return devices;

    int deviceCount = dataSize / sizeof(AudioDeviceID);
    std::vector<AudioDeviceID> deviceIds(deviceCount);
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &prop, 0, nullptr, &dataSize, deviceIds.data()) != noErr)
        return devices;

    for (AudioDeviceID devId : deviceIds) {
        // Check if device has output streams
        AudioObjectPropertyAddress streamProp = {
            kAudioDevicePropertyStreams,
            kAudioDevicePropertyScopeOutput,
            kAudioObjectPropertyElementMain
        };

        UInt32 streamSize = 0;
        if (AudioObjectGetPropertyDataSize(devId, &streamProp, 0, nullptr, &streamSize) != noErr || streamSize == 0)
            continue;

        CFStringRef nameRef = nullptr;
        UInt32 nameSize = sizeof(nameRef);
        AudioObjectPropertyAddress nameProp = {
            kAudioObjectPropertyName,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        if (AudioObjectGetPropertyData(devId, &nameProp, 0, nullptr, &nameSize, &nameRef) != noErr || !nameRef)
            continue;

        char nameBuf[256];
        CFStringGetCString(nameRef, nameBuf, sizeof(nameBuf), kCFStringEncodingUTF8);
        CFRelease(nameRef);

        devices.push_back({utf8ToWide(std::to_string(devId)), utf8ToWide(nameBuf)});
    }

    return devices;
}

bool AudioRender::initialize(const std::wstring& deviceId) {
    if (!deviceId.empty()) {
        try {
            deviceId_ = std::stoul(wideToUtf8(deviceId));
        } catch (...) {
            deviceId_ = 0;
        }
    }

    if (deviceId_ == 0) {
        AudioObjectPropertyAddress prop = {
            kAudioHardwarePropertyDefaultOutputDevice,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        UInt32 size = sizeof(deviceId_);
        AudioObjectGetPropertyData(kAudioObjectSystemObject, &prop, 0, nullptr, &size, &deviceId_);
    }

    return deviceId_ != 0;
}

void AudioRender::cleanup() {
    stop();
    if (audioUnit_) {
        AudioComponentInstance au = (AudioComponentInstance)audioUnit_;
        AudioUnitUninitialize(au);
        AudioComponentInstanceDispose(au);
        audioUnit_ = nullptr;
    }
}

static OSStatus caRenderCallback(void* inRefCon,
                                  AudioUnitRenderActionFlags* ioActionFlags,
                                  const AudioTimeStamp* inTimeStamp,
                                  UInt32 inBusNumber,
                                  UInt32 inNumberFrames,
                                  AudioBufferList* ioData) {
    AudioRender* render = (AudioRender*)inRefCon;

    float* outBuf = (float*)ioData->mBuffers[0].mData;
    uint32_t outFrames = inNumberFrames;

    std::lock_guard<std::mutex> lock(render->bufferMutex_);

    if (render->audioBuffer_.size() >= outFrames) {
        memcpy(outBuf, render->audioBuffer_.data(), outFrames * sizeof(float));
        render->audioBuffer_.erase(render->audioBuffer_.begin(),
                                    render->audioBuffer_.begin() + outFrames);
    } else {
        // Underrun — output silence
        memset(outBuf, 0, outFrames * sizeof(float));
    }

    return noErr;
}

bool AudioRender::start() {
    if (running_) return false;

    AudioComponentDescription desc = {
        kAudioUnitType_Output,
        kAudioUnitSubType_DefaultOutput,
        kAudioUnitManufacturer_Apple,
        0, 0
    };

    AudioComponent comp = AudioComponentFindNext(nullptr, &desc);
    if (!comp) return false;

    AudioComponentInstance au;
    if (AudioComponentInstanceNew(comp, &au) != noErr) return false;
    audioUnit_ = au;

    // Set format: 48kHz mono float (mic audio from decoder)
    AudioStreamBasicDescription fmt = {};
    fmt.mSampleRate = 48000;
    fmt.mFormatID = kAudioFormatLinearPCM;
    fmt.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    fmt.mBitsPerChannel = 32;
    fmt.mChannelsPerFrame = 1;
    fmt.mFramesPerPacket = 1;
    fmt.mBytesPerFrame = 4;
    fmt.mBytesPerPacket = 4;

    AudioUnitSetProperty(au, kAudioUnitProperty_StreamFormat,
                         kAudioUnitScope_Input, 0, &fmt, sizeof(fmt));

    AURenderCallbackStruct callbackStruct;
    callbackStruct.inputProc = caRenderCallback;
    callbackStruct.inputProcRefCon = this;
    AudioUnitSetProperty(au, kAudioUnitProperty_SetRenderCallback,
                         kAudioUnitScope_Input, 0, &callbackStruct, sizeof(callbackStruct));

    if (AudioUnitInitialize(au) != noErr) return false;

    running_ = true;
    AudioOutputUnitStart(au);
    printf("Audio render started (48kHz mono)\n");
    return true;
}

void AudioRender::stop() {
    running_ = false;
    if (audioUnit_) {
        AudioOutputUnitStop((AudioComponentInstance)audioUnit_);
    }
}

void AudioRender::pushAudio(const float* data, uint32_t frameCount) {
    std::lock_guard<std::mutex> lock(bufferMutex_);
    audioBuffer_.insert(audioBuffer_.end(), data, data + frameCount);
    // Cap buffer at 1 second to prevent unbounded growth
    if (audioBuffer_.size() > 48000) {
        audioBuffer_.erase(audioBuffer_.begin(), audioBuffer_.end() - 48000);
    }
}
