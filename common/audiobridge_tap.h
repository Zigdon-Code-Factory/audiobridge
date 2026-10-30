#pragma once

// Shared kernel-to-user contract between AudioBridge Virtual Speakers (audiobridge_vad.sys)
// and the AudioBridge desktop server (audiobridge.exe).
// Compiles cleanly in both Windows Kernel-Mode (WDK) and User-Mode (Win32 SDK).

#ifdef _KERNEL_MODE
#include <ntddk.h>
#else
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <winioctl.h>
#include <cstdint>
typedef uint32_t UINT32;
typedef int64_t  LONG64;
typedef uint64_t ULONG64;
#endif

#define AUDIOBRIDGE_TAP_DEVICE_NAME     L"\\Device\\AudioBridgeTap"
#define AUDIOBRIDGE_TAP_DOS_DEVICE_NAME L"\\DosDevices\\AudioBridgeTap"
#define AUDIOBRIDGE_TAP_USER_PATH       L"\\\\.\\AudioBridgeTap"

#define AUDIOBRIDGE_TAP_MAGIC           0x41425450u // 'ABTP'
#define AUDIOBRIDGE_TAP_VERSION         1u

// Default cyclic tap ring buffer: 4800 stereo float frames (100 ms @ 48 kHz),
// notified every 128 frames (2.67 ms Shared IAudioClient3) or 48 frames (1.0 ms Exclusive).
#define AUDIOBRIDGE_TAP_SAMPLE_RATE     48000u
#define AUDIOBRIDGE_TAP_CHANNELS        2u
#define AUDIOBRIDGE_TAP_CAPACITY_FRAMES 4800u
#define AUDIOBRIDGE_TAP_DEFAULT_QUANTUM 128u

// IOCTL definitions (FILE_DEVICE_SOUND = 0x0000001D)
#define IOCTL_AUDIOBRIDGE_REGISTER_EVENT \
    CTL_CODE(FILE_DEVICE_SOUND, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define IOCTL_AUDIOBRIDGE_MAP_BUFFER \
    CTL_CODE(FILE_DEVICE_SOUND, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define IOCTL_AUDIOBRIDGE_UNMAP_BUFFER \
    CTL_CODE(FILE_DEVICE_SOUND, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)

#pragma pack(push, 8)

// Input to IOCTL_AUDIOBRIDGE_REGISTER_EVENT: 64-bit widened Win32 event HANDLE
// (Pass 0 to unregister the notification event.)
struct AudioBridgeTapEventRequest {
    ULONG64 eventHandle;
};

// Output from IOCTL_AUDIOBRIDGE_MAP_BUFFER: user-mode virtual address of AudioBridgeTapSharedBuffer
struct AudioBridgeTapMapResponse {
    ULONG64 userAddress;
    UINT32  totalBytes;
    UINT32  reserved;
};

// Header at offset 0 of the mapped shared memory region, followed immediately at offset 64
// by `capacityFrames * channels` interleaved IEEE 754 float32 samples (`float pcmData[]`).
struct AudioBridgeTapSharedBuffer {
    UINT32          magic;             // AUDIOBRIDGE_TAP_MAGIC ('ABTP')
    UINT32          version;           // AUDIOBRIDGE_TAP_VERSION (1)
    UINT32          sampleRate;        // 48000
    UINT32          channels;          // 2
    UINT32          bitsPerSample;     // 32 (IEEE float stereo normalized [-1.0, +1.0])
    UINT32          isFloat;           // 1
    UINT32          capacityFrames;    // Cyclic capacity in stereo frames (e.g. 4800)
    UINT32          quantumFrames;     // Active DMA notification quantum (e.g. 48 or 128)
    volatile LONG64 writeFrameCursor;  // Monotonically increasing total frames written
    volatile LONG   activeStreams;     // >0 while Windows audio engine has an active stream
    UINT32          reserved[5];       // Pad header to 64 bytes (cache-line aligned)
};

#pragma pack(pop)

static inline float* AudioBridgeTapGetSamples(AudioBridgeTapSharedBuffer* hdr) {
    return reinterpret_cast<float*>(reinterpret_cast<unsigned char*>(hdr) + sizeof(AudioBridgeTapSharedBuffer));
}

static inline const float* AudioBridgeTapGetSamplesConst(const AudioBridgeTapSharedBuffer* hdr) {
    return reinterpret_cast<const float*>(reinterpret_cast<const unsigned char*>(hdr) + sizeof(AudioBridgeTapSharedBuffer));
}
