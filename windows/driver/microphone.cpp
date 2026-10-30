// Capture-only PortCls/WaveRT adapter. User-mode PCM is copied through a
// SYSTEM-only, exclusive, buffered control handle; no user pointers are mapped.
#include <ntifs.h>
#include <windef.h>
#include <initguid.h>
#include <ks.h>
#define NOBITMAP
#include <mmreg.h>
#include <ksmedia.h>
#include <portcls.h>
#include <stdunk.h>
#include <wdmsec.h>
#include "../../common/audiobridge_mic.h"
#include "../../common/mic_ring.h"

#define AUDIOBRIDGE_POOL_TAG 'mBau'
enum { PIN_SYSTEM = 0, PIN_BRIDGE = 1, PIN_TOPO_BRIDGE = 0, PIN_MIC = 1 };
static PDEVICE_OBJECT g_Control = nullptr;
static PDRIVER_DISPATCH g_Dispatch[IRP_MJ_MAXIMUM_FUNCTION + 1]{};
static PDRIVER_UNLOAD g_Unload = nullptr;
static KSPIN_LOCK g_RingLock;
static MicPlaybackQueue g_Ring;
static volatile LONG g_StreamCount = 0;
static BOOLEAN g_Capturing = FALSE;

static bool ValidFormat(PKSDATAFORMAT format) {
    if (!format || format->FormatSize < sizeof(KSDATAFORMAT_WAVEFORMATEX) ||
        !IsEqualGUIDAligned(format->MajorFormat, KSDATAFORMAT_TYPE_AUDIO) ||
        !IsEqualGUIDAligned(format->SubFormat, KSDATAFORMAT_SUBTYPE_PCM) ||
        !IsEqualGUIDAligned(format->Specifier, KSDATAFORMAT_SPECIFIER_WAVEFORMATEX)) return false;
    const auto* wave = &reinterpret_cast<PKSDATAFORMAT_WAVEFORMATEX>(format)->WaveFormatEx;
    if (wave->nChannels != 1 || wave->nSamplesPerSec != AB_MIC_RATE ||
        wave->wBitsPerSample != 16 || wave->nBlockAlign != 2 || wave->nAvgBytesPerSec != AB_MIC_RATE * 2)
        return false;
    if (wave->wFormatTag == WAVE_FORMAT_PCM) return wave->cbSize == 0;
    if (wave->wFormatTag != WAVE_FORMAT_EXTENSIBLE ||
        format->FormatSize < sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE) ||
        wave->cbSize != sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) return false;
    const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wave);
    return ext->Samples.wValidBitsPerSample == 16 &&
        (ext->dwChannelMask == KSAUDIO_SPEAKER_MONO || ext->dwChannelMask == 0) &&
        IsEqualGUIDAligned(ext->SubFormat, KSDATAFORMAT_SUBTYPE_PCM);
}

#include "microphone_descriptors.h"

class CMiniportWaveRTStream : public IMiniportWaveRTStreamNotification, public CUnknown {
public:
    DECLARE_STD_UNKNOWN();
    explicit CMiniportWaveRTStream(PUNKNOWN outer) : CUnknown(outer) { KeInitializeSpinLock(&lock_); }
    ~CMiniportWaveRTStream() {
        SetState(KSSTATE_STOP);
        if (timer_) ExDeleteTimer(timer_, TRUE, TRUE, nullptr);
        FreeAudioBuffer(mdl_, bytes_);
        if (port_) port_->Release();
        InterlockedExchange(&g_StreamCount, 0);
    }
    NTSTATUS Init(PPORTWAVERTSTREAM port, PKSDATAFORMAT format) {
        if (!ValidFormat(format)) return STATUS_INVALID_PARAMETER;
        port_ = port; port_->AddRef();
        KeQueryPerformanceCounter(&frequency_);
        timer_ = ExAllocateTimer(Timer, this, EX_TIMER_HIGH_RESOLUTION);
        return timer_ ? STATUS_SUCCESS : STATUS_INSUFFICIENT_RESOURCES;
    }
    IMP_IMiniportWaveRTStream;
    IMP_IMiniportWaveRTStreamNotification;
private:
    static VOID NTAPI Timer(PEX_TIMER, PVOID context) {
        static_cast<CMiniportWaveRTStream*>(context)->tick();
    }
    void tick() {
        KIRQL irql;
        KeAcquireSpinLock(&lock_, &irql);
        if (state_ != KSSTATE_RUN || !samples_ || !bytes_) { KeReleaseSpinLock(&lock_, irql); return; }
        const ULONGLONG elapsed = KeQueryPerformanceCounter(nullptr).QuadPart - epoch_;
        // Divide first to avoid overflowing after long continuous sessions.
        const ULONGLONG target = baseFrames_ + (elapsed / frequency_.QuadPart) * AB_MIC_RATE +
            (elapsed % frequency_.QuadPart) * AB_MIC_RATE / frequency_.QuadPart;
        const ULONG capacity = bytes_ / sizeof(short);
        const ULONGLONG previous = frames_;
        KeAcquireSpinLockAtDpcLevel(&g_RingLock);
        if (target > frames_ + capacity) {
            // Scheduler stall: skip expired time, overwrite the entire DMA ring
            // with silence. Never replay old microphone samples after a stall.
            RtlZeroMemory(samples_, bytes_);
            g_Ring.clear();
            frames_ = target;
        } else {
            while (frames_ < target) { samples_[frames_ % capacity] = g_Ring.pop(); ++frames_; }
        }
        KeReleaseSpinLockFromDpcLevel(&g_RingLock);
        KeMemoryBarrier();
        if (packetFrames_ && previous / packetFrames_ != frames_ / packetFrames_) {
            for (auto event : events_) if (event) KeSetEvent(event, IO_SOUND_INCREMENT, FALSE);
        }
        KeReleaseSpinLock(&lock_, irql);
    }
    PPORTWAVERTSTREAM port_ = nullptr;
    PMDL mdl_ = nullptr;
    short* samples_ = nullptr;
    ULONG bytes_ = 0;
    ULONG packetFrames_ = 0;
    KSPIN_LOCK lock_;
    PKEVENT events_[2]{};
    PEX_TIMER timer_ = nullptr;
    KSSTATE state_ = KSSTATE_STOP;
    LARGE_INTEGER frequency_{};
    ULONGLONG epoch_ = 0, baseFrames_ = 0, frames_ = 0;
};

STDMETHODIMP_(NTSTATUS) CMiniportWaveRTStream::NonDelegatingQueryInterface(REFIID iid, PVOID* out) {
    *out = nullptr;
    if (IsEqualGUIDAligned(iid, IID_IUnknown) || IsEqualGUIDAligned(iid, IID_IMiniportWaveRTStream) ||
        IsEqualGUIDAligned(iid, IID_IMiniportWaveRTStreamNotification)) {
        *out = static_cast<PMINIPORTWAVERTSTREAMNOTIFICATION>(this);
        AddRef(); return STATUS_SUCCESS;
    }
    return STATUS_NOINTERFACE;
}

STDMETHODIMP_(NTSTATUS) CMiniportWaveRTStream::AllocateAudioBuffer(
    ULONG requested, PMDL* mdl, ULONG* actual, ULONG* offset, MEMORY_CACHING_TYPE* cache) {
    return AllocateBufferWithNotification(1, requested, mdl, actual, offset, cache);
}
STDMETHODIMP_(NTSTATUS) CMiniportWaveRTStream::AllocateBufferWithNotification(
    ULONG notifications, ULONG requested, PMDL* mdl, ULONG* actual, ULONG* offset, MEMORY_CACHING_TYPE* cache) {
    if (mdl_ || (notifications != 1 && notifications != 2) || requested < 96 || requested > 192000 ||
        requested % (notifications * sizeof(short))) return STATUS_INVALID_PARAMETER;
    PHYSICAL_ADDRESS high{}; high.QuadPart = MAXLONGLONG;
    const ULONG allocationBytes = (requested + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    PMDL allocated = port_->AllocatePagesForMdl(high, allocationBytes);
    if (!allocated) return STATUS_INSUFFICIENT_RESOURCES;
    auto* mapped = static_cast<short*>(port_->MapAllocatedPages(allocated, MmCached));
    if (!mapped) { port_->FreePagesFromMdl(allocated); return STATUS_INSUFFICIENT_RESOURCES; }
    RtlZeroMemory(mapped, allocationBytes);
    KIRQL irql; KeAcquireSpinLock(&lock_, &irql);
    mdl_ = allocated; samples_ = mapped; bytes_ = requested;
    frames_ = baseFrames_ = 0;
    packetFrames_ = requested / (notifications * sizeof(short));
    KeReleaseSpinLock(&lock_, irql);
    *mdl = allocated; *actual = requested; *offset = 0; *cache = MmCached;
    return STATUS_SUCCESS;
}
STDMETHODIMP_(VOID) CMiniportWaveRTStream::FreeAudioBuffer(PMDL mdl, ULONG) {
    KIRQL irql; KeAcquireSpinLock(&lock_, &irql);
    short* mapped = nullptr;
    if (mdl && mdl == mdl_) {
        mapped = samples_; samples_ = nullptr; mdl_ = nullptr; bytes_ = 0;
    }
    KeReleaseSpinLock(&lock_, irql);
    if (mapped) { port_->UnmapAllocatedPages(mapped, mdl); port_->FreePagesFromMdl(mdl); }
}
STDMETHODIMP_(VOID) CMiniportWaveRTStream::FreeBufferWithNotification(PMDL mdl, ULONG bytes) {
    FreeAudioBuffer(mdl, bytes);
}
STDMETHODIMP_(NTSTATUS) CMiniportWaveRTStream::RegisterNotificationEvent(PKEVENT event) {
    if (!event) return STATUS_INVALID_PARAMETER;
    NTSTATUS result = STATUS_INSUFFICIENT_RESOURCES;
    KIRQL irql; KeAcquireSpinLock(&lock_, &irql);
    for (auto existing : events_) if (existing == event) result = STATUS_SUCCESS;
    if (!NT_SUCCESS(result)) for (auto& slot : events_) if (!slot) { slot = event; result = STATUS_SUCCESS; break; }
    KeReleaseSpinLock(&lock_, irql); return result;
}
STDMETHODIMP_(NTSTATUS) CMiniportWaveRTStream::UnregisterNotificationEvent(PKEVENT event) {
    KIRQL irql; KeAcquireSpinLock(&lock_, &irql);
    for (auto& slot : events_) if (slot == event) slot = nullptr;
    KeReleaseSpinLock(&lock_, irql); return STATUS_SUCCESS;
}
STDMETHODIMP_(VOID) CMiniportWaveRTStream::GetHWLatency(KSRTAUDIO_HWLATENCY* latency) {
    RtlZeroMemory(latency, sizeof(*latency));
}
STDMETHODIMP_(NTSTATUS) CMiniportWaveRTStream::GetPositionRegister(KSRTAUDIO_HWREGISTER*) { return STATUS_NOT_SUPPORTED; }
STDMETHODIMP_(NTSTATUS) CMiniportWaveRTStream::GetClockRegister(KSRTAUDIO_HWREGISTER*) { return STATUS_NOT_SUPPORTED; }
STDMETHODIMP_(NTSTATUS) CMiniportWaveRTStream::GetPosition(KSAUDIO_POSITION* position) {
    KIRQL irql; KeAcquireSpinLock(&lock_, &irql);
    // For capture both positions refer to the end of fully written data.
    position->PlayOffset = position->WriteOffset = bytes_ ? (frames_ * sizeof(short)) % bytes_ : 0;
    KeReleaseSpinLock(&lock_, irql); return STATUS_SUCCESS;
}
STDMETHODIMP_(NTSTATUS) CMiniportWaveRTStream::SetFormat(PKSDATAFORMAT format) {
    return ValidFormat(format) ? STATUS_SUCCESS : STATUS_NO_MATCH;
}
STDMETHODIMP_(NTSTATUS) CMiniportWaveRTStream::SetState(KSSTATE state) {
    if (state < KSSTATE_STOP || state > KSSTATE_RUN) return STATUS_INVALID_PARAMETER;
    KIRQL irql; KeAcquireSpinLock(&lock_, &irql);
    if (state == KSSTATE_RUN && (!samples_ || !timer_)) { KeReleaseSpinLock(&lock_, irql); return STATUS_INVALID_DEVICE_STATE; }
    if (state == state_) { KeReleaseSpinLock(&lock_, irql); return STATUS_SUCCESS; }
    state_ = state;
    KeAcquireSpinLockAtDpcLevel(&g_RingLock);
    g_Ring.clear(); g_Capturing = state == KSSTATE_RUN;
    KeReleaseSpinLockFromDpcLevel(&g_RingLock);
    if (state == KSSTATE_STOP) {
        frames_ = baseFrames_ = 0;
        if (samples_) RtlZeroMemory(samples_, bytes_);
    }
    if (state == KSSTATE_RUN) {
        epoch_ = KeQueryPerformanceCounter(nullptr).QuadPart; baseFrames_ = frames_;
        ExSetTimer(timer_, -10000, 10000, nullptr);
    } else if (timer_) ExCancelTimer(timer_, nullptr);
    KeReleaseSpinLock(&lock_, irql); return STATUS_SUCCESS;
}

#include "microphone_miniports.h"
#include "microphone_adapter.h"

static NTSTATUS Control(PDEVICE_OBJECT device, PIRP irp) {
    if (device != g_Control) return g_Dispatch[IoGetCurrentIrpStackLocation(irp)->MajorFunction](device, irp);
    auto* stack = IoGetCurrentIrpStackLocation(irp);
    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    KIRQL irql;
    switch (stack->MajorFunction) {
    case IRP_MJ_CREATE:
    case IRP_MJ_CLEANUP:
        KeAcquireSpinLock(&g_RingLock, &irql); g_Ring.clear(); KeReleaseSpinLock(&g_RingLock, irql);
        status = STATUS_SUCCESS; break;
    case IRP_MJ_CLOSE: status = STATUS_SUCCESS; break;
    case IRP_MJ_DEVICE_CONTROL: {
        const ULONG code = stack->Parameters.DeviceIoControl.IoControlCode;
        const ULONG length = stack->Parameters.DeviceIoControl.InputBufferLength;
        if (code == IOCTL_AB_MIC_RESET && length == 0) {
            KeAcquireSpinLock(&g_RingLock, &irql); g_Ring.clear(); KeReleaseSpinLock(&g_RingLock, irql);
            status = STATUS_SUCCESS;
        } else if (code == IOCTL_AB_MIC_PUSH) {
            status = STATUS_INVALID_PARAMETER;
            if (length != sizeof(MicPacket)) break;
            auto* packet = static_cast<MicPacket*>(irp->AssociatedIrp.SystemBuffer);
            if (!packet || !validMicPacket(*packet)) break;
            KeAcquireSpinLock(&g_RingLock, &irql);
            if (g_Capturing) g_Ring.push(packet->samples, packet->frames);
            KeReleaseSpinLock(&g_RingLock, irql);
            status = STATUS_SUCCESS;
        }
        break;
    }
    }
    irp->IoStatus.Status = status; irp->IoStatus.Information = 0;
    IoCompleteRequest(irp, IO_NO_INCREMENT); return status;
}

static VOID Unload(PDRIVER_OBJECT driver) {
    UNICODE_STRING link = RTL_CONSTANT_STRING(AUDIOBRIDGE_MIC_DOS_DEVICE);
    IoDeleteSymbolicLink(&link);
    if (g_Control) { IoDeleteDevice(g_Control); g_Control = nullptr; }
    if (g_Unload) g_Unload(driver);
}

extern "C" DRIVER_INITIALIZE DriverEntry;
extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT driver, PUNICODE_STRING registry) {
    KeInitializeSpinLock(&g_RingLock); g_Ring.clear();
    NTSTATUS status = PcInitializeAdapterDriver(driver, registry, AddDevice);
    if (!NT_SUCCESS(status)) return status;
    UNICODE_STRING name = RTL_CONSTANT_STRING(AUDIOBRIDGE_MIC_NT_DEVICE);
    UNICODE_STRING link = RTL_CONSTANT_STRING(AUDIOBRIDGE_MIC_DOS_DEVICE);
    UNICODE_STRING security = RTL_CONSTANT_STRING(L"D:P(A;;GA;;;SY)");
    // Unique device class for this control object's security configuration.
    const GUID controlClass = {0x80dcbf43, 0x7ad5, 0x4766, {0xb1,0xf6,0x51,0x8d,0x23,0xe7,0x2e,0x3c}};
    status = IoCreateDeviceSecure(driver, 0, &name, FILE_DEVICE_SOUND, FILE_DEVICE_SECURE_OPEN,
                                   TRUE, &security, &controlClass, &g_Control);
    if (!NT_SUCCESS(status)) return status;
    status = IoCreateSymbolicLink(&link, &name);
    if (!NT_SUCCESS(status)) { IoDeleteDevice(g_Control); g_Control = nullptr; return status; }
    for (ULONG i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; ++i) {
        g_Dispatch[i] = driver->MajorFunction[i]; driver->MajorFunction[i] = Control;
    }
    g_Unload = driver->DriverUnload; driver->DriverUnload = Unload;
    g_Control->Flags |= DO_BUFFERED_IO;
    g_Control->Flags &= ~DO_DEVICE_INITIALIZING;
    return STATUS_SUCCESS;
}
