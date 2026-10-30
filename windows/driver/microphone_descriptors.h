// Capture-only descriptors: 48 kHz mono signed 16-bit PCM.
static KSDATARANGE_AUDIO g_WavePinDataRangesStream[] = {
    {{ sizeof(KSDATARANGE_AUDIO), 0, 0, 0,
       STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO), STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM),
       STATICGUIDOF(KSDATAFORMAT_SPECIFIER_WAVEFORMATEX) }, 1, 16, 16, 48000, 48000}
};
static PKSDATARANGE g_WavePinDataRangePointersStream[] = { PKSDATARANGE(&g_WavePinDataRangesStream[0]) };
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
    // System capture pin
    {
        1, 1, 0, nullptr,
        {
            0, nullptr, 0, nullptr,
            ARRAYSIZE(g_WavePinDataRangePointersStream),
            g_WavePinDataRangePointersStream,
            KSPIN_DATAFLOW_OUT,
            KSPIN_COMMUNICATION_SINK,
            &KSCATEGORY_AUDIO,
            nullptr, 0
        }
    },
    // Bridge from microphone topology
    {
        0, 0, 0, nullptr,
        {
            0, nullptr, 0, nullptr,
            ARRAYSIZE(g_WavePinDataRangePointersBridge),
            g_WavePinDataRangePointersBridge,
            KSPIN_DATAFLOW_IN,
            KSPIN_COMMUNICATION_NONE,
            &KSCATEGORY_AUDIO,
            nullptr, 0
        }
    }
};

static PCCONNECTION_DESCRIPTOR g_WaveMiniportConnections[] = {
    { PCFILTER_NODE, PIN_BRIDGE, PCFILTER_NODE, PIN_SYSTEM }
};

static GUID g_WaveCategories[] = {
    STATICGUIDOF(KSCATEGORY_AUDIO),
    STATICGUIDOF(KSCATEGORY_CAPTURE),
    STATICGUIDOF(KSCATEGORY_REALTIME)
};

static PCFILTER_DESCRIPTOR g_WaveMiniportFilterDescriptor = {
    0, nullptr,
    sizeof(PCPIN_DESCRIPTOR), ARRAYSIZE(g_WaveMiniportPins), g_WaveMiniportPins,
    sizeof(PCNODE_DESCRIPTOR), 0, nullptr,
    ARRAYSIZE(g_WaveMiniportConnections), g_WaveMiniportConnections,
    ARRAYSIZE(g_WaveCategories), g_WaveCategories
};

// Microphone topology
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
            KSPIN_DATAFLOW_OUT, KSPIN_COMMUNICATION_NONE,
            &KSCATEGORY_AUDIO, nullptr, 0
        }
    },
    {
        0, 0, 0, nullptr,
        {
            0, nullptr, 0, nullptr,
            ARRAYSIZE(g_TopoBridgeRangePtrs), g_TopoBridgeRangePtrs,
            KSPIN_DATAFLOW_IN, KSPIN_COMMUNICATION_NONE,
            &KSNODETYPE_MICROPHONE, nullptr, 0
        }
    }
};

static PCCONNECTION_DESCRIPTOR g_TopoMiniportConnections[] = {
    { PCFILTER_NODE, PIN_MIC, PCFILTER_NODE, PIN_TOPO_BRIDGE }
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
