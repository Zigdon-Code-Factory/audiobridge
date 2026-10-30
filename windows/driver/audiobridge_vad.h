#pragma once

// AudioBridge Ultra-Low-Latency WaveRT Virtual Speaker Driver (audiobridge_vad.sys)
// Replaces Voicemeeter / VB-Cable and bypasses Windows audiodg.exe's 10ms loopback APO batching
// by exposing a 1.0ms-2.67ms (48-128 frame) WaveRT cyclic DMA endpoint directly to audiobridge.exe
// via zero-copy shared memory (\\.\AudioBridgeTap).

#ifndef _KERNEL_MODE
#define _KERNEL_MODE 1
#endif
#include <ntifs.h>
#include <windef.h>
#include <ks.h>
#include <ksmedia.h>
#include <portcls.h>
#include <stdunk.h>
#include "../../common/audiobridge_tap.h"

#define AUDIOBRIDGE_POOL_TAG 'dVBA' // 'ABVd'

// Pin IDs for WaveRT & Topology subdevices
enum {
    KSPIN_WAVE_RENDER_SINK = 0,
    KSPIN_WAVE_RENDER_SOURCE = 1
};

enum {
    KSPIN_TOPO_WAVEOUT_SOURCE = 0,
    KSPIN_TOPO_LINEOUT_DEST = 1
};

// Global shared tap state accessed by WaveRT render stream and \\.\AudioBridgeTap control device
struct TapGlobalContext {
    KSPIN_LOCK                  lock;
    AudioBridgeTapSharedBuffer* sharedBuf;
    PMDL                        sharedMdl;
    ULONG                       totalBytes;
    PKEVENT                     notifyEvent; // Referenced kernel PKEVENT registered by audiobridge.exe
};

// Per-handle context stored in FileObject->FsContext for safe MDL unmapping on process exit
struct TapOpenContext {
    PVOID userMappedVa;
};
