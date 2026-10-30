// PortCls WaveRT and topology miniports. Included once by microphone.cpp.
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
    if (PinId != PIN_SYSTEM || ClientDataRange->FormatSize < sizeof(KSDATARANGE_AUDIO) ||
        !IsEqualGUIDAligned(ClientDataRange->MajorFormat, KSDATAFORMAT_TYPE_AUDIO) ||
        !IsEqualGUIDAligned(ClientDataRange->SubFormat, KSDATAFORMAT_SUBTYPE_PCM) ||
        !IsEqualGUIDAligned(ClientDataRange->Specifier, KSDATAFORMAT_SPECIFIER_WAVEFORMATEX))
        return STATUS_NO_MATCH;
    UNREFERENCED_PARAMETER(MyDataRange);
    auto* range = reinterpret_cast<PKSDATARANGE_AUDIO>(ClientDataRange);
    if (range->MaximumChannels < 1 || range->MinimumBitsPerSample > 16 || range->MaximumBitsPerSample < 16 ||
        range->MinimumSampleFrequency > AB_MIC_RATE || range->MaximumSampleFrequency < AB_MIC_RATE) return STATUS_NO_MATCH;
    const ULONG needed = sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE);
    *ResultantFormatLength = needed;
    if (!OutputBufferLength) return STATUS_BUFFER_OVERFLOW;
    if (OutputBufferLength < needed) return STATUS_BUFFER_TOO_SMALL;
    auto* fmt = static_cast<PKSDATAFORMAT_WAVEFORMATEXTENSIBLE>(ResultantFormat);
    RtlZeroMemory(fmt, needed);
    fmt->DataFormat.FormatSize = needed;
    fmt->DataFormat.SampleSize = 2;
    fmt->DataFormat.MajorFormat = KSDATAFORMAT_TYPE_AUDIO;
    fmt->DataFormat.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
    fmt->DataFormat.Specifier = KSDATAFORMAT_SPECIFIER_WAVEFORMATEX;
    auto& wf = fmt->WaveFormatExt.Format;
    wf.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    wf.nChannels = 1; wf.nSamplesPerSec = AB_MIC_RATE;
    wf.wBitsPerSample = 16; wf.nBlockAlign = 2; wf.nAvgBytesPerSec = AB_MIC_RATE * 2;
    wf.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    fmt->WaveFormatExt.Samples.wValidBitsPerSample = 16;
    fmt->WaveFormatExt.dwChannelMask = KSAUDIO_SPEAKER_MONO;
    fmt->WaveFormatExt.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) CMiniportWaveRT::NewStream(
    PMINIPORTWAVERTSTREAM* Stream, PPORTWAVERTSTREAM PortStream,
    ULONG Pin, BOOLEAN Capture, PKSDATAFORMAT DataFormat) {
    if (!Capture || Pin != PIN_SYSTEM || !ValidFormat(DataFormat)) return STATUS_INVALID_PARAMETER;
    *Stream = nullptr;
    if (InterlockedCompareExchange(&g_StreamCount, 1, 0) != 0) return STATUS_DEVICE_BUSY;

    auto* s = new (NonPagedPoolNx, AUDIOBRIDGE_POOL_TAG) CMiniportWaveRTStream(nullptr);
    if (!s) { InterlockedExchange(&g_StreamCount, 0); return STATUS_INSUFFICIENT_RESOURCES; }
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
