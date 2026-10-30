// PortCls subdevice registration. Included once by microphone.cpp.
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
            DeviceObject, portTopo, PIN_TOPO_BRIDGE,
            portWave, PIN_BRIDGE);
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
