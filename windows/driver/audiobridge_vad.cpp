#include "audiobridge_vad.h"

// ============================================================================
// Global Tap Control Device & PortCls Dispatch Table
// ============================================================================

static PDEVICE_OBJECT   g_TapControlDevice = nullptr;
static PDRIVER_DISPATCH g_PortClsDispatch[IRP_MJ_MAXIMUM_FUNCTION + 1] = {};
static PDRIVER_UNLOAD   g_PortClsUnload = nullptr;
static TapGlobalContext g_Tap = {};

static NTSTATUS InitializeGlobalTapBuffer() {
    KeInitializeSpinLock(&g_Tap.lock);
    g_Tap.notifyEvent = nullptr;

    const ULONG pcmBytes = AUDIOBRIDGE_TAP_CAPACITY_FRAMES * AUDIOBRIDGE_TAP_CHANNELS * sizeof(float);
    g_Tap.totalBytes = (ULONG)sizeof(AudioBridgeTapSharedBuffer) + pcmBytes;
    const ULONG alignedBytes = (g_Tap.totalBytes + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    PHYSICAL_ADDRESS lowAddr{};
    PHYSICAL_ADDRESS highAddr{};
    highAddr.QuadPart = -1LL;
    PHYSICAL_ADDRESS skipBytes{};

    g_Tap.sharedMdl = MmAllocatePagesForMdlEx(
        lowAddr, highAddr, skipBytes, alignedBytes,
        MmCached, MM_ALLOCATE_FULLY_REQUIRED | MM_ALLOCATE_ZERO_MEMORY);
    if (!g_Tap.sharedMdl) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    g_Tap.sharedBuf = static_cast<AudioBridgeTapSharedBuffer*>(
        MmGetSystemAddressForMdlSafe(g_Tap.sharedMdl, NormalPagePriority | MdlMappingNoExecute));
    if (!g_Tap.sharedBuf) {
        MmFreePagesFromMdl(g_Tap.sharedMdl);
        ExFreePool(g_Tap.sharedMdl);
        g_Tap.sharedMdl = nullptr;
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(g_Tap.sharedBuf, alignedBytes);
    g_Tap.sharedBuf->magic          = AUDIOBRIDGE_TAP_MAGIC;
    g_Tap.sharedBuf->version        = AUDIOBRIDGE_TAP_VERSION;
    g_Tap.sharedBuf->sampleRate     = AUDIOBRIDGE_TAP_SAMPLE_RATE;
    g_Tap.sharedBuf->channels       = AUDIOBRIDGE_TAP_CHANNELS;
    g_Tap.sharedBuf->bitsPerSample  = 32;
    g_Tap.sharedBuf->isFloat        = 1;
    g_Tap.sharedBuf->capacityFrames = AUDIOBRIDGE_TAP_CAPACITY_FRAMES;
    g_Tap.sharedBuf->quantumFrames  = AUDIOBRIDGE_TAP_DEFAULT_QUANTUM;
    g_Tap.sharedBuf->writeFrameCursor = 0;
    g_Tap.sharedBuf->activeStreams  = 0;

    return STATUS_SUCCESS;
}

static void DestroyGlobalTapBuffer() {
    KIRQL oldIrql;
    KeAcquireSpinLock(&g_Tap.lock, &oldIrql);
    PKEVENT ev = g_Tap.notifyEvent;
    g_Tap.notifyEvent = nullptr;
    KeReleaseSpinLock(&g_Tap.lock, oldIrql);

    if (ev) {
        ObDereferenceObject(ev);
    }
    if (g_Tap.sharedMdl) {
        MmFreePagesFromMdl(g_Tap.sharedMdl);
        ExFreePool(g_Tap.sharedMdl);
        g_Tap.sharedMdl = nullptr;
        g_Tap.sharedBuf = nullptr;
    }
}

// Push freshly rendered WaveRT DMA frames into the cyclic user-mode tap buffer
// and immediately signal audiobridge.exe's registered event (<0.1ms wake latency).
static void TapPushRenderedFrames(const BYTE* dmaBase, ULONG dmaBytes, ULONG byteOffset,
                                  ULONG framesToCopy, BOOLEAN isFloat32, ULONG bitsPerSample) {
    if (!g_Tap.sharedBuf || !dmaBase || dmaBytes == 0 || framesToCopy == 0) return;

    float* dstBase = AudioBridgeTapGetSamples(g_Tap.sharedBuf);
    const ULONG cap = g_Tap.sharedBuf->capacityFrames;
    LONG64 cursor = g_Tap.sharedBuf->writeFrameCursor;
    const ULONG bytesPerFrame = (bitsPerSample / 8) * AUDIOBRIDGE_TAP_CHANNELS;

    for (ULONG f = 0; f < framesToCopy; ++f) {
        const ULONG srcOff = (byteOffset + f * bytesPerFrame) % dmaBytes;
        const ULONG dstIdx = (ULONG)((cursor + f) % cap) * AUDIOBRIDGE_TAP_CHANNELS;

        if (isFloat32 && bitsPerSample == 32) {
            const float* src = reinterpret_cast<const float*>(dmaBase + srcOff);
            dstBase[dstIdx]     = src[0];
            dstBase[dstIdx + 1] = src[1];
        } else if (bitsPerSample == 16) {
            const SHORT* src = reinterpret_cast<const SHORT*>(dmaBase + srcOff);
            dstBase[dstIdx]     = src[0] * (1.0f / 32768.0f);
            dstBase[dstIdx + 1] = src[1] * (1.0f / 32768.0f);
        } else if (bitsPerSample == 32) {
            const LONG* src = reinterpret_cast<const LONG*>(dmaBase + srcOff);
            dstBase[dstIdx]     = (float)(src[0] * (1.0 / 2147483648.0));
            dstBase[dstIdx + 1] = (float)(src[1] * (1.0 / 2147483648.0));
        }
    }

    KeMemoryBarrier();
    InterlockedExchangeAdd64(&g_Tap.sharedBuf->writeFrameCursor, (LONG64)framesToCopy);

    KIRQL oldIrql;
    KeAcquireSpinLock(&g_Tap.lock, &oldIrql);
    PKEVENT ev = g_Tap.notifyEvent;
    if (ev) {
        KeSetEvent(ev, IO_SOUND_INCREMENT, FALSE);
    }
    KeReleaseSpinLock(&g_Tap.lock, oldIrql);
}

// ============================================================================
// Descriptor Tables: 48 kHz Stereo 16-bit PCM & 32-bit IEEE Float WaveRT
// ============================================================================

static KSDATARANGE_AUDIO g_WavePinDataRangesStream[] = {
    {
        {
            sizeof(KSDATARANGE_AUDIO), 0, 0, 0,
            STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
            STATICGUIDOF(KSDATAFORMAT_SUBTYPE_IEEE_FLOAT),
            STATICGUIDOF(KSDATAFORMAT_SPECIFIER_WAVEFORMATEX)
        },
        2, 32, 32, 48000, 48000
    },
    {
        {
            sizeof(KSDATARANGE_AUDIO), 0, 0, 0,
            STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
            STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM),
            STATICGUIDOF(KSDATAFORMAT_SPECIFIER_WAVEFORMATEX)
        },
        2, 16, 32, 48000, 48000
    }
};

static PKSDATARANGE g_WavePinDataRangePointersStream[] = {
    PKSDATARANGE(&g_WavePinDataRangesStream[0]),
    PKSDATARANGE(&g_WavePinDataRangesStream[1])
};

static KSDATARANGE g_WavePinDataRangesBridge[] = {
    {
        sizeof(KSDATARANGE), 0, 0, 0,
        STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
        STATICGUIDOF(KSDATAFORMAT_SUBTYPE_ANALOG),
        STATICGUIDOF(KSDATAFORMAT_SPECIFIER_NONE)
    }
};

static PKSDATARANGE g_WavePinDataRangePointersBridge[] = {
    &g_WavePinDataRangesBridge[0]
};

static PCPIN_DESCRIPTOR g_WaveMiniportPins[] = {
    // Pin 0: KSPIN_WAVE_RENDER_SINK (System / audiodg.exe -> WaveRT)
    {
        2, 2, 0, nullptr,
        {
            0, nullptr, 0, nullptr,
            ARRAYSIZE(g_WavePinDataRangePointersStream),
            g_WavePinDataRangePointersStream,
            KSPIN_DATAFLOW_IN,
            KSPIN_COMMUNICATION_SINK,
            &KSCATEGORY_AUDIO,
            nullptr, 0
        }
    },
    // Pin 1: KSPIN_WAVE_RENDER_SOURCE (Bridge to Topology Filter)
    {
        0, 0, 0, nullptr,
        {
            0, nullptr, 0, nullptr,
            ARRAYSIZE(g_WavePinDataRangePointersBridge),
            g_WavePinDataRangePointersBridge,
            KSPIN_DATAFLOW_OUT,
            KSPIN_COMMUNICATION_NONE,
            &KSCATEGORY_AUDIO,
            nullptr, 0
        }
    }
};

static PCCONNECTION_DESCRIPTOR g_WaveMiniportConnections[] = {
    { PCFILTER_NODE, KSPIN_WAVE_RENDER_SINK, PCFILTER_NODE, KSPIN_WAVE_RENDER_SOURCE }
};

static GUID g_WaveCategories[] = {
    STATICGUIDOF(KSCATEGORY_AUDIO),
    STATICGUIDOF(KSCATEGORY_RENDER),
    STATICGUIDOF(KSCATEGORY_REALTIME)
};

static PCFILTER_DESCRIPTOR g_WaveMiniportFilterDescriptor = {
    0, nullptr,
    sizeof(PCPIN_DESCRIPTOR), ARRAYSIZE(g_WaveMiniportPins), g_WaveMiniportPins,
    sizeof(PCNODE_DESCRIPTOR), 0, nullptr,
    ARRAYSIZE(g_WaveMiniportConnections), g_WaveMiniportConnections,
    ARRAYSIZE(g_WaveCategories), g_WaveCategories
};

// Topology Filter Descriptors (Speaker Endpoint)
static KSDATARANGE g_TopoBridgeRange[] = {
    {
        sizeof(KSDATARANGE), 0, 0, 0,
        STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
        STATICGUIDOF(KSDATAFORMAT_SUBTYPE_ANALOG),
        STATICGUIDOF(KSDATAFORMAT_SPECIFIER_NONE)
    }
};

static PKSDATARANGE g_TopoBridgeRangePtrs[] = { &g_TopoBridgeRange[0] };

static PCPIN_DESCRIPTOR g_TopoMiniportPins[] = {
    {
        0, 0, 0, nullptr,
        {
            0, nullptr, 0, nullptr,
            ARRAYSIZE(g_TopoBridgeRangePtrs), g_TopoBridgeRangePtrs,
            KSPIN_DATAFLOW_IN, KSPIN_COMMUNICATION_NONE,
            &KSCATEGORY_AUDIO, nullptr, 0
        }
    },
    {
        0, 0, 0, nullptr,
        {
            0, nullptr, 0, nullptr,
            ARRAYSIZE(g_TopoBridgeRangePtrs), g_TopoBridgeRangePtrs,
            KSPIN_DATAFLOW_OUT, KSPIN_COMMUNICATION_NONE,
            &KSNODETYPE_SPEAKER, nullptr, 0
        }
    }
};

static PCCONNECTION_DESCRIPTOR g_TopoMiniportConnections[] = {
    { PCFILTER_NODE, KSPIN_TOPO_WAVEOUT_SOURCE, PCFILTER_NODE, KSPIN_TOPO_LINEOUT_DEST }
};

static GUID g_TopoCategories[] = {
    STATICGUIDOF(KSCATEGORY_AUDIO),
    STATICGUIDOF(KSCATEGORY_TOPOLOGY)
};

static PCFILTER_DESCRIPTOR g_TopoMiniportFilterDescriptor = {
    0, nullptr,
    sizeof(PCPIN_DESCRIPTOR), ARRAYSIZE(g_TopoMiniportPins), g_TopoMiniportPins,
    sizeof(PCNODE_DESCRIPTOR), 0, nullptr,
    ARRAYSIZE(g_TopoMiniportConnections), g_TopoMiniportConnections,
    ARRAYSIZE(g_TopoCategories), g_TopoCategories
};

// ============================================================================
// CMiniportWaveRTStream: 1.0ms-2.67ms High-Resolution WaveRT Stream + Notifications
// ============================================================================

class CMiniportWaveRTStream : public IMiniportWaveRTStreamNotification, public CUnknown {
public:
    DECLARE_STD_UNKNOWN();

    CMiniportWaveRTStream(PUNKNOWN OuterUnknown)
        : CUnknown(OuterUnknown),
          portStream_(nullptr),
          dmaMdl_(nullptr),
          dmaKernelVa_(nullptr),
          dmaSizeBytes_(0),
          notificationCount_(0),
          notifyEvent1_(nullptr),
          notifyEvent2_(nullptr),
          hiResTimer_(nullptr),
          state_(KSSTATE_STOP),
          isFloat_(TRUE),
          bitsPerSample_(32),
          blockAlign_(8),
          playBytePos_(0),
          quantumFrames_(AUDIOBRIDGE_TAP_DEFAULT_QUANTUM) {
        KeInitializeSpinLock(&eventLock_);
    }

    ~CMiniportWaveRTStream() override {
        SetState(KSSTATE_STOP);
        if (hiResTimer_) {
            ExDeleteTimer(hiResTimer_, TRUE, TRUE, nullptr);
            hiResTimer_ = nullptr;
        }
        FreeAudioBuffer(dmaMdl_, dmaSizeBytes_);
        if (portStream_) {
            portStream_->Release();
            portStream_ = nullptr;
        }
    }

    NTSTATUS Init(PPORTWAVERTSTREAM PortStream, PKSDATAFORMAT DataFormat) {
        portStream_ = PortStream;
        portStream_->AddRef();

        if (DataFormat->FormatSize >= sizeof(KSDATAFORMAT_WAVEFORMATEX)) {
            PWAVEFORMATEX wf = &reinterpret_cast<PKSDATAFORMAT_WAVEFORMATEX>(DataFormat)->WaveFormatEx;
            bitsPerSample_ = wf->wBitsPerSample;
            blockAlign_ = wf->nBlockAlign;
            isFloat_ = IsEqualGUIDAligned(DataFormat->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
        }

        hiResTimer_ = ExAllocateTimer(TimerCallback, this, EX_TIMER_HIGH_RESOLUTION);
        if (!hiResTimer_) {
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        return STATUS_SUCCESS;
    }

    IMP_IMiniportWaveRTStream;
    IMP_IMiniportWaveRTStreamNotification;

private:
    static VOID NTAPI TimerCallback(PEX_TIMER Timer, PVOID Context) {
        UNREFERENCED_PARAMETER(Timer);
        auto* self = static_cast<CMiniportWaveRTStream*>(Context);
        self->OnQuantumTick();
    }

    void OnQuantumTick() {
        if (state_ != KSSTATE_RUN || !dmaKernelVa_ || dmaSizeBytes_ == 0 || blockAlign_ == 0) {
            return;
        }

        const ULONG quantumBytes = quantumFrames_ * blockAlign_;
        const ULONG prevPos = playBytePos_;
        playBytePos_ = (prevPos + quantumBytes) % dmaSizeBytes_;

        // Copy newly mixed 48-128 frame WaveRT quantum straight into user-mode tap buffer
        TapPushRenderedFrames(dmaKernelVa_, dmaSizeBytes_, prevPos, quantumFrames_, isFloat_, bitsPerSample_);

        // Signal Windows audio engine (audiodg.exe / IAudioClient3) WaveRT notification events
        KIRQL oldIrql;
        KeAcquireSpinLock(&eventLock_, &oldIrql);
        if (notifyEvent1_) KeSetEvent(notifyEvent1_, IO_SOUND_INCREMENT, FALSE);
        if (notifyEvent2_) KeSetEvent(notifyEvent2_, IO_SOUND_INCREMENT, FALSE);
        KeReleaseSpinLock(&eventLock_, oldIrql);
    }

    PPORTWAVERTSTREAM portStream_;
    PMDL              dmaMdl_;
    BYTE*             dmaKernelVa_;
    ULONG             dmaSizeBytes_;
    ULONG             notificationCount_;
    KSPIN_LOCK        eventLock_;
    PKEVENT           notifyEvent1_;
    PKEVENT           notifyEvent2_;
    PEX_TIMER         hiResTimer_;
    KSSTATE           state_;
    BOOLEAN           isFloat_;
    ULONG             bitsPerSample_;
    ULONG             blockAlign_;
    volatile ULONG    playBytePos_;
    ULONG             quantumFrames_;
};

STDMETHODIMP_(NTSTATUS) CMiniportWaveRTStream::NonDelegatingQueryInterface(REFIID Interface, PVOID* Object) {
    if (IsEqualGUIDAligned(Interface, IID_IUnknown)) {
        *Object = PVOID(PUNKNOWN(PMINIPORTWAVERTSTREAMNOTIFICATION(this)));
    } else if (IsEqualGUIDAligned(Interface, IID_IMiniportWaveRTStream)) {
        *Object = PVOID(PMINIPORTWAVERTSTREAM(this));
    } else if (IsEqualGUIDAligned(Interface, IID_IMiniportWaveRTStreamNotification)) {
        *Object = PVOID(PMINIPORTWAVERTSTREAMNOTIFICATION(this));
    } else {
        *Object = nullptr;
        return STATUS_INVALID_PARAMETER;
    }
    PUNKNOWN(*Object)->AddRef();
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) CMiniportWaveRTStream::AllocateAudioBuffer(
    ULONG RequestedSize, PMDL* AudioBufferMdl, ULONG* ActualSize,
    ULONG* OffsetFromFirstPage, MEMORY_CACHING_TYPE* CacheType) {
    return AllocateBufferWithNotification(1, RequestedSize, AudioBufferMdl, ActualSize, OffsetFromFirstPage, CacheType);
}

STDMETHODIMP_(VOID) CMiniportWaveRTStream::FreeAudioBuffer(PMDL AudioBufferMdl, ULONG BufferSize) {
    UNREFERENCED_PARAMETER(BufferSize);
    if (AudioBufferMdl && portStream_) {
        portStream_->FreePagesFromMdl(AudioBufferMdl);
    }
    if (AudioBufferMdl == dmaMdl_) {
        dmaMdl_ = nullptr;
        dmaKernelVa_ = nullptr;
        dmaSizeBytes_ = 0;
    }
}

STDMETHODIMP_(NTSTATUS) CMiniportWaveRTStream::AllocateBufferWithNotification(
    ULONG NotificationCount, ULONG RequestedSize, PMDL* AudioBufferMdl,
    ULONG* ActualSize, ULONG* OffsetFromFirstPage, MEMORY_CACHING_TYPE* CacheType) {
    if (RequestedSize == 0 || NotificationCount == 0) return STATUS_INVALID_PARAMETER;

    const ULONG alignedSize = (RequestedSize + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    PHYSICAL_ADDRESS highAddr{};
    highAddr.QuadPart = -1LL;

    PMDL mdl = portStream_->AllocatePagesForMdl(highAddr, alignedSize);
    if (!mdl) return STATUS_INSUFFICIENT_RESOURCES;

    dmaKernelVa_ = static_cast<BYTE*>(portStream_->MapAllocatedPages(mdl, MmCached));
    if (!dmaKernelVa_) {
        portStream_->FreePagesFromMdl(mdl);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(dmaKernelVa_, alignedSize);

    dmaMdl_            = mdl;
    dmaSizeBytes_      = RequestedSize;
    notificationCount_ = NotificationCount;
    playBytePos_       = 0;

    if (blockAlign_ > 0 && NotificationCount > 0) {
        ULONG qf = (RequestedSize / NotificationCount) / blockAlign_;
        if (qf < 48) qf = 48; // 1.0 ms floor @ 48 kHz
        quantumFrames_ = qf;
        if (g_Tap.sharedBuf) {
            g_Tap.sharedBuf->quantumFrames = quantumFrames_;
        }
    }

    *AudioBufferMdl      = mdl;
    *ActualSize          = RequestedSize;
    *OffsetFromFirstPage = 0;
    *CacheType           = MmCached;
    return STATUS_SUCCESS;
}

STDMETHODIMP_(VOID) CMiniportWaveRTStream::FreeBufferWithNotification(PMDL AudioBufferMdl, ULONG BufferSize) {
    FreeAudioBuffer(AudioBufferMdl, BufferSize);
}

STDMETHODIMP_(NTSTATUS) CMiniportWaveRTStream::RegisterNotificationEvent(PKEVENT NotificationEvent) {
    if (!NotificationEvent) return STATUS_INVALID_PARAMETER;
    KIRQL oldIrql;
    KeAcquireSpinLock(&eventLock_, &oldIrql);
    NTSTATUS status = STATUS_SUCCESS;
    if (!notifyEvent1_) notifyEvent1_ = NotificationEvent;
    else if (!notifyEvent2_) notifyEvent2_ = NotificationEvent;
    else status = STATUS_INSUFFICIENT_RESOURCES;
    KeReleaseSpinLock(&eventLock_, oldIrql);
    return status;
}

STDMETHODIMP_(NTSTATUS) CMiniportWaveRTStream::UnregisterNotificationEvent(PKEVENT NotificationEvent) {
    KIRQL oldIrql;
    KeAcquireSpinLock(&eventLock_, &oldIrql);
    if (notifyEvent1_ == NotificationEvent) notifyEvent1_ = nullptr;
    else if (notifyEvent2_ == NotificationEvent) notifyEvent2_ = nullptr;
    KeReleaseSpinLock(&eventLock_, oldIrql);
    return STATUS_SUCCESS;
}

STDMETHODIMP_(VOID) CMiniportWaveRTStream::GetHWLatency(KSRTAUDIO_HWLATENCY* hwLatency) {
    hwLatency->FifoSize     = 0; // Zero hardware FIFO latency
    hwLatency->ChipsetDelay = 0;
    hwLatency->CodecDelay   = 0;
}

STDMETHODIMP_(NTSTATUS) CMiniportWaveRTStream::GetPositionRegister(KSRTAUDIO_HWREGISTER* Register) {
    UNREFERENCED_PARAMETER(Register);
    return STATUS_UNSUCCESSFUL; // Uses GetPosition()
}

STDMETHODIMP_(NTSTATUS) CMiniportWaveRTStream::GetClockRegister(KSRTAUDIO_HWREGISTER* Register) {
    UNREFERENCED_PARAMETER(Register);
    return STATUS_UNSUCCESSFUL;
}

STDMETHODIMP_(NTSTATUS) CMiniportWaveRTStream::GetPosition(KSAUDIO_POSITION* Position) {
    Position->PlayOffset  = playBytePos_;
    Position->WriteOffset = (playBytePos_ + quantumFrames_ * blockAlign_) % (dmaSizeBytes_ ? dmaSizeBytes_ : 1);
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) CMiniportWaveRTStream::SetFormat(PKSDATAFORMAT Format) {
    UNREFERENCED_PARAMETER(Format);
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) CMiniportWaveRTStream::SetState(KSSTATE State) {
    if (State == state_) return STATUS_SUCCESS;

    if (State == KSSTATE_RUN && state_ != KSSTATE_RUN) {
        state_ = KSSTATE_RUN;
        if (g_Tap.sharedBuf) InterlockedIncrement(&g_Tap.sharedBuf->activeStreams);

        // Period in 100-ns units (negative = relative): e.g. 128 frames @ 48 kHz = -26667 (2.67 ms)
        const LONGLONG period100ns = -((LONGLONG)quantumFrames_ * 10000000LL / AUDIOBRIDGE_TAP_SAMPLE_RATE);
        EXT_SET_PARAMETERS params;
        ExInitializeSetTimerParameters(&params);
        ExSetTimer(hiResTimer_, period100ns, -period100ns, &params);
    } else if (State != KSSTATE_RUN && state_ == KSSTATE_RUN) {
        state_ = State;
        ExCancelTimer(hiResTimer_, nullptr);
        if (g_Tap.sharedBuf) InterlockedDecrement(&g_Tap.sharedBuf->activeStreams);
    } else {
        state_ = State;
    }
    return STATUS_SUCCESS;
}

// ============================================================================
// CMiniportWaveRT & CMiniportTopology
// ============================================================================

class CMiniportWaveRT : public IMiniportWaveRT, public CUnknown {
public:
    DECLARE_STD_UNKNOWN();
    CMiniportWaveRT(PUNKNOWN OuterUnknown) : CUnknown(OuterUnknown) {}
    ~CMiniportWaveRT() override = default;

    IMP_IMiniportWaveRT;
};

STDMETHODIMP_(NTSTATUS) CMiniportWaveRT::NonDelegatingQueryInterface(REFIID Interface, PVOID* Object) {
    if (IsEqualGUIDAligned(Interface, IID_IUnknown) || IsEqualGUIDAligned(Interface, IID_IMiniport) ||
        IsEqualGUIDAligned(Interface, IID_IMiniportWaveRT)) {
        *Object = PVOID(PMINIPORTWAVERT(this));
        PUNKNOWN(*Object)->AddRef();
        return STATUS_SUCCESS;
    }
    *Object = nullptr;
    return STATUS_INVALID_PARAMETER;
}

STDMETHODIMP_(NTSTATUS) CMiniportWaveRT::Init(PUNKNOWN UnknownAdapter, PRESOURCELIST ResourceList, PPORTWAVERT Port) {
    UNREFERENCED_PARAMETER(UnknownAdapter);
    UNREFERENCED_PARAMETER(ResourceList);
    UNREFERENCED_PARAMETER(Port);
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) CMiniportWaveRT::GetDescription(PPCFILTER_DESCRIPTOR* Description) {
    *Description = &g_WaveMiniportFilterDescriptor;
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) CMiniportWaveRT::DataRangeIntersection(
    ULONG PinId, PKSDATARANGE ClientDataRange, PKSDATARANGE MyDataRange,
    ULONG OutputBufferLength, PVOID ResultantFormat, PULONG ResultantFormatLength) {
    UNREFERENCED_PARAMETER(PinId);
    UNREFERENCED_PARAMETER(ClientDataRange);
    UNREFERENCED_PARAMETER(MyDataRange);

    const ULONG needed = sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE);
    *ResultantFormatLength = needed;
    if (OutputBufferLength == 0) return STATUS_BUFFER_OVERFLOW;
    if (OutputBufferLength < needed) return STATUS_BUFFER_TOO_SMALL;

    auto* fmt = static_cast<PKSDATAFORMAT_WAVEFORMATEXTENSIBLE>(ResultantFormat);
    RtlZeroMemory(fmt, needed);
    fmt->DataFormat.FormatSize = needed;
    fmt->DataFormat.SampleSize = 8;
    fmt->DataFormat.MajorFormat = KSDATAFORMAT_TYPE_AUDIO;
    fmt->DataFormat.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    fmt->DataFormat.Specifier = KSDATAFORMAT_SPECIFIER_WAVEFORMATEX;
    fmt->WaveFormatExt.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    fmt->WaveFormatExt.Format.nChannels = 2;
    fmt->WaveFormatExt.Format.nSamplesPerSec = 48000;
    fmt->WaveFormatExt.Format.wBitsPerSample = 32;
    fmt->WaveFormatExt.Format.nBlockAlign = 8;
    fmt->WaveFormatExt.Format.nAvgBytesPerSec = 48000 * 8;
    fmt->WaveFormatExt.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    fmt->WaveFormatExt.Samples.wValidBitsPerSample = 32;
    fmt->WaveFormatExt.dwChannelMask = KSAUDIO_SPEAKER_STEREO;
    fmt->WaveFormatExt.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) CMiniportWaveRT::NewStream(
    PMINIPORTWAVERTSTREAM* Stream, PPORTWAVERTSTREAM PortStream,
    ULONG Pin, BOOLEAN Capture, PKSDATAFORMAT DataFormat) {
    if (Capture || Pin != KSPIN_WAVE_RENDER_SINK) return STATUS_INVALID_PARAMETER;

    auto* s = new (NonPagedPoolNx, AUDIOBRIDGE_POOL_TAG) CMiniportWaveRTStream(nullptr);
    if (!s) return STATUS_INSUFFICIENT_RESOURCES;
    s->AddRef();

    NTSTATUS status = s->Init(PortStream, DataFormat);
    if (!NT_SUCCESS(status)) {
        s->Release();
        return status;
    }

    *Stream = PMINIPORTWAVERTSTREAM(s);
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) CMiniportWaveRT::GetDeviceDescription(PDEVICE_DESCRIPTION DeviceDescription) {
    RtlZeroMemory(DeviceDescription, sizeof(DEVICE_DESCRIPTION));
    DeviceDescription->Version = DEVICE_DESCRIPTION_VERSION2;
    DeviceDescription->Master = TRUE;
    DeviceDescription->ScatterGather = TRUE;
    DeviceDescription->Dma32BitAddresses = TRUE;
    DeviceDescription->Dma64BitAddresses = TRUE;
    DeviceDescription->MaximumLength = 0x100000;
    return STATUS_SUCCESS;
}

class CMiniportTopology : public IMiniportTopology, public CUnknown {
public:
    DECLARE_STD_UNKNOWN();
    CMiniportTopology(PUNKNOWN OuterUnknown) : CUnknown(OuterUnknown) {}
    ~CMiniportTopology() override = default;
    IMP_IMiniportTopology;
};

STDMETHODIMP_(NTSTATUS) CMiniportTopology::NonDelegatingQueryInterface(REFIID Interface, PVOID* Object) {
    if (IsEqualGUIDAligned(Interface, IID_IUnknown) || IsEqualGUIDAligned(Interface, IID_IMiniport) ||
        IsEqualGUIDAligned(Interface, IID_IMiniportTopology)) {
        *Object = PVOID(PMINIPORTTOPOLOGY(this));
        PUNKNOWN(*Object)->AddRef();
        return STATUS_SUCCESS;
    }
    *Object = nullptr;
    return STATUS_INVALID_PARAMETER;
}

STDMETHODIMP_(NTSTATUS) CMiniportTopology::Init(PUNKNOWN UnknownAdapter, PRESOURCELIST ResourceList, PPORTTOPOLOGY Port) {
    UNREFERENCED_PARAMETER(UnknownAdapter);
    UNREFERENCED_PARAMETER(ResourceList);
    UNREFERENCED_PARAMETER(Port);
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) CMiniportTopology::GetDescription(PPCFILTER_DESCRIPTOR* Description) {
    *Description = &g_TopoMiniportFilterDescriptor;
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) CMiniportTopology::DataRangeIntersection(
    ULONG PinId, PKSDATARANGE ClientDataRange, PKSDATARANGE MyDataRange,
    ULONG OutputBufferLength, PVOID ResultantFormat, PULONG ResultantFormatLength) {
    UNREFERENCED_PARAMETER(PinId);
    UNREFERENCED_PARAMETER(ClientDataRange);
    UNREFERENCED_PARAMETER(MyDataRange);
    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(ResultantFormat);
    UNREFERENCED_PARAMETER(ResultantFormatLength);
    return STATUS_NOT_IMPLEMENTED;
}

// ============================================================================
// Sideband Control Device Dispatch (\\.\AudioBridgeTap)
// ============================================================================

static void UnmapTapUserBuffer(PFILE_OBJECT fileObj) {
    if (!fileObj || !fileObj->FsContext) return;
    auto* ctx = static_cast<TapOpenContext*>(fileObj->FsContext);
    if (ctx->userMappedVa && g_Tap.sharedMdl) {
        MmUnmapLockedPages(ctx->userMappedVa, g_Tap.sharedMdl);
        ctx->userMappedVa = nullptr;
    }
}

static NTSTATUS TapControlDispatch(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    UNREFERENCED_PARAMETER(DeviceObject);
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS status = STATUS_SUCCESS;
    ULONG_PTR info = 0;

    switch (stack->MajorFunction) {
        case IRP_MJ_CREATE: {
            auto* ctx = static_cast<TapOpenContext*>(
                ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(TapOpenContext), AUDIOBRIDGE_POOL_TAG));
            if (!ctx) {
                status = STATUS_INSUFFICIENT_RESOURCES;
            } else {
                ctx->userMappedVa = nullptr;
                stack->FileObject->FsContext = ctx;
            }
            break;
        }
        case IRP_MJ_CLEANUP: {
            // Unmap MDL while still in the calling user process context!
            UnmapTapUserBuffer(stack->FileObject);
            break;
        }
        case IRP_MJ_CLOSE: {
            if (stack->FileObject && stack->FileObject->FsContext) {
                ExFreePoolWithTag(stack->FileObject->FsContext, AUDIOBRIDGE_POOL_TAG);
                stack->FileObject->FsContext = nullptr;
            }
            break;
        }
        case IRP_MJ_DEVICE_CONTROL: {
            const ULONG code = stack->Parameters.DeviceIoControl.IoControlCode;
            if (code == IOCTL_AUDIOBRIDGE_REGISTER_EVENT) {
                if (stack->Parameters.DeviceIoControl.InputBufferLength < sizeof(AudioBridgeTapEventRequest)) {
                    status = STATUS_BUFFER_TOO_SMALL;
                    break;
                }
                auto* req = static_cast<AudioBridgeTapEventRequest*>(Irp->AssociatedIrp.SystemBuffer);
                PKEVENT newEv = nullptr;
                if (req->eventHandle != 0) {
                    status = ObReferenceObjectByHandle(
                        (HANDLE)(ULONG_PTR)req->eventHandle, EVENT_MODIFY_STATE,
                        *ExEventObjectType, Irp->RequestorMode, (PVOID*)&newEv, nullptr);
                    if (!NT_SUCCESS(status)) break;
                }
                KIRQL oldIrql;
                KeAcquireSpinLock(&g_Tap.lock, &oldIrql);
                PKEVENT oldEv = g_Tap.notifyEvent;
                g_Tap.notifyEvent = newEv;
                KeReleaseSpinLock(&g_Tap.lock, oldIrql);
                if (oldEv) ObDereferenceObject(oldEv);
            } else if (code == IOCTL_AUDIOBRIDGE_MAP_BUFFER) {
                if (stack->Parameters.DeviceIoControl.OutputBufferLength < sizeof(AudioBridgeTapMapResponse)) {
                    status = STATUS_BUFFER_TOO_SMALL;
                    break;
                }
                auto* ctx = static_cast<TapOpenContext*>(stack->FileObject->FsContext);
                if (!ctx || !g_Tap.sharedMdl) {
                    status = STATUS_INVALID_DEVICE_STATE;
                    break;
                }
                if (!ctx->userMappedVa) {
                    __try {
                        ctx->userMappedVa = MmMapLockedPagesSpecifyCache(
                            g_Tap.sharedMdl, UserMode, MmCached, nullptr, FALSE,
                            NormalPagePriority | MdlMappingNoExecute);
                    } __except (EXCEPTION_EXECUTE_HANDLER) {
                        ctx->userMappedVa = nullptr;
                        status = GetExceptionCode();
                    }
                }
                if (NT_SUCCESS(status) && ctx->userMappedVa) {
                    auto* resp = static_cast<AudioBridgeTapMapResponse*>(Irp->AssociatedIrp.SystemBuffer);
                    resp->userAddress = (ULONG64)(ULONG_PTR)ctx->userMappedVa;
                    resp->totalBytes  = g_Tap.totalBytes;
                    resp->reserved    = 0;
                    info = sizeof(AudioBridgeTapMapResponse);
                } else if (NT_SUCCESS(status)) {
                    status = STATUS_INSUFFICIENT_RESOURCES;
                }
            } else if (code == IOCTL_AUDIOBRIDGE_UNMAP_BUFFER) {
                UnmapTapUserBuffer(stack->FileObject);
            } else {
                status = STATUS_INVALID_DEVICE_REQUEST;
            }
            break;
        }
        default:
            status = STATUS_INVALID_DEVICE_REQUEST;
            break;
    }

    Irp->IoStatus.Status = status;
    Irp->IoStatus.Information = info;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;
}

static NTSTATUS DriverDispatchRouter(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    if (DeviceObject == g_TapControlDevice) {
        return TapControlDispatch(DeviceObject, Irp);
    }
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
    PDRIVER_DISPATCH fn = g_PortClsDispatch[stack->MajorFunction];
    if (fn) {
        return fn(DeviceObject, Irp);
    }
    Irp->IoStatus.Status = STATUS_INVALID_DEVICE_REQUEST;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_INVALID_DEVICE_REQUEST;
}

static VOID DriverUnloadRouter(PDRIVER_OBJECT DriverObject) {
    UNICODE_STRING symLink = RTL_CONSTANT_STRING(AUDIOBRIDGE_TAP_DOS_DEVICE_NAME);
    IoDeleteSymbolicLink(&symLink);
    if (g_TapControlDevice) {
        IoDeleteDevice(g_TapControlDevice);
        g_TapControlDevice = nullptr;
    }
    DestroyGlobalTapBuffer();
    if (g_PortClsUnload) {
        g_PortClsUnload(DriverObject);
    }
}

// ============================================================================
// PortCls Adapter Startup & DriverEntry
// ============================================================================

static NTSTATUS StartDevice(PDEVICE_OBJECT DeviceObject, PIRP Irp, PRESOURCELIST ResourceList) {
    PPORT portWave = nullptr;
    PPORT portTopo = nullptr;
    PUNKNOWN unkWave = nullptr;
    PUNKNOWN unkTopo = nullptr;

    NTSTATUS status = PcNewPort(&portWave, CLSID_PortWaveRT);
    if (NT_SUCCESS(status)) {
        auto* mpWave = new (NonPagedPoolNx, AUDIOBRIDGE_POOL_TAG) CMiniportWaveRT(nullptr);
        if (!mpWave) status = STATUS_INSUFFICIENT_RESOURCES;
        else {
            mpWave->AddRef();
            unkWave = PMINIPORTWAVERT(mpWave);
            status = portWave->Init(DeviceObject, Irp, unkWave, nullptr, ResourceList);
            if (NT_SUCCESS(status)) {
                status = PcRegisterSubdevice(DeviceObject, L"Wave", portWave);
            }
        }
    }

    if (NT_SUCCESS(status)) {
        status = PcNewPort(&portTopo, CLSID_PortTopology);
        if (NT_SUCCESS(status)) {
            auto* mpTopo = new (NonPagedPoolNx, AUDIOBRIDGE_POOL_TAG) CMiniportTopology(nullptr);
            if (!mpTopo) status = STATUS_INSUFFICIENT_RESOURCES;
            else {
                mpTopo->AddRef();
                unkTopo = PMINIPORTTOPOLOGY(mpTopo);
                status = portTopo->Init(DeviceObject, Irp, unkTopo, nullptr, ResourceList);
                if (NT_SUCCESS(status)) {
                    status = PcRegisterSubdevice(DeviceObject, L"Topology", portTopo);
                }
            }
        }
    }

    if (NT_SUCCESS(status) && portWave && portTopo) {
        status = PcRegisterPhysicalConnection(
            DeviceObject, portWave, KSPIN_WAVE_RENDER_SOURCE,
            portTopo, KSPIN_TOPO_WAVEOUT_SOURCE);
    }

    if (unkWave) unkWave->Release();
    if (unkTopo) unkTopo->Release();
    if (portWave) portWave->Release();
    if (portTopo) portTopo->Release();
    return status;
}

static NTSTATUS AddDevice(PDRIVER_OBJECT DriverObject, PDEVICE_OBJECT PhysicalDeviceObject) {
    return PcAddAdapterDevice(DriverObject, PhysicalDeviceObject, StartDevice, 2, 0);
}

extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath) {
    NTSTATUS status = InitializeGlobalTapBuffer();
    if (!NT_SUCCESS(status)) return status;

    status = PcInitializeAdapterDriver(DriverObject, RegistryPath, AddDevice);
    if (!NT_SUCCESS(status)) {
        DestroyGlobalTapBuffer();
        return status;
    }

    // Create sideband user-mode tap control device \\.\AudioBridgeTap
    UNICODE_STRING devName = RTL_CONSTANT_STRING(AUDIOBRIDGE_TAP_DEVICE_NAME);
    UNICODE_STRING symLink = RTL_CONSTANT_STRING(AUDIOBRIDGE_TAP_DOS_DEVICE_NAME);
    status = IoCreateDevice(
        DriverObject, 0, &devName, FILE_DEVICE_SOUND,
        FILE_DEVICE_SECURE_OPEN, FALSE, &g_TapControlDevice);
    if (NT_SUCCESS(status)) {
        status = IoCreateSymbolicLink(&symLink, &devName);
        if (NT_SUCCESS(status)) {
            g_TapControlDevice->Flags |= DO_BUFFERED_IO;
            g_TapControlDevice->Flags &= ~DO_DEVICE_INITIALIZING;
        } else {
            IoDeleteDevice(g_TapControlDevice);
            g_TapControlDevice = nullptr;
        }
    }

    // Intercept dispatch routines so \\.\AudioBridgeTap IRPs route to TapControlDispatch
    // while PnP/KS audio IRPs forward transparently to PortCls.
    for (ULONG i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; ++i) {
        g_PortClsDispatch[i] = DriverObject->MajorFunction[i];
        DriverObject->MajorFunction[i] = DriverDispatchRouter;
    }
    g_PortClsUnload = DriverObject->DriverUnload;
    DriverObject->DriverUnload = DriverUnloadRouter;

    return STATUS_SUCCESS;
}
