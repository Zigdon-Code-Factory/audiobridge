#pragma once
#ifdef _KERNEL_MODE
#include <ntddk.h>
#else
#include <Windows.h>
#include <winioctl.h>
#endif

#define AUDIOBRIDGE_MIC_DEVICE L"\\\\.\\AudioBridgeMic"
#define AUDIOBRIDGE_MIC_NT_DEVICE L"\\Device\\AudioBridgeMic"
#define AUDIOBRIDGE_MIC_DOS_DEVICE L"\\DosDevices\\AudioBridgeMic"
#define AUDIOBRIDGE_MIC_PIPE L"\\\\.\\pipe\\AudioBridge.Microphone.v1"
#define AUDIOBRIDGE_MIC_SERVICE L"AudioBridgeMicService"
#define AUDIOBRIDGE_MIC_HARDWARE_ID L"AudioBridge\\Microphone"

constexpr ULONG AB_MIC_VERSION = 1;
constexpr ULONG AB_MIC_MAGIC = 0x434d4241;
constexpr ULONG AB_MIC_RATE = 48000;
constexpr ULONG AB_MIC_MAX_FRAMES = 960;
// A bounded 40 ms FIFO; underflow is silence, overload discards oldest samples.
constexpr ULONG AB_MIC_CAPACITY = 1920;
#define IOCTL_AB_MIC_PUSH CTL_CODE(FILE_DEVICE_SOUND, 0x810, METHOD_BUFFERED, FILE_WRITE_DATA)
#define IOCTL_AB_MIC_RESET CTL_CODE(FILE_DEVICE_SOUND, 0x811, METHOD_BUFFERED, FILE_WRITE_DATA)

// Fixed-size, pointer-free protocol, shared by the pipe and buffered driver IOCTL.
// frames == 0 is a heartbeat; AB_MIC_RESET is a separate driver operation.
struct MicPacket {
    ULONG magic = AB_MIC_MAGIC;
    ULONG version = AB_MIC_VERSION;
    ULONG frames = 0;
    ULONG reserved = 0;
    short samples[AB_MIC_MAX_FRAMES]{};
};
struct MicReply {
    ULONG magic = AB_MIC_MAGIC;
    ULONG version = AB_MIC_VERSION;
    ULONG error = 0;
};
static_assert(sizeof(MicPacket) == 1936, "microphone ABI changed");
static_assert(sizeof(MicReply) == 12, "microphone ABI changed");
inline bool validMicPacket(const MicPacket& p) {
    return p.magic == AB_MIC_MAGIC && p.version == AB_MIC_VERSION &&
           p.frames <= AB_MIC_MAX_FRAMES && p.reserved == 0;
}
