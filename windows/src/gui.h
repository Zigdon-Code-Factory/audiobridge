#pragma once
#include <Windows.h>
#include <CommCtrl.h>
#include <string>
#include <vector>
#include <mutex>
#include <functional>
#include <map>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "gdi32.lib")

// Forward declarations
struct ApprovedPeer;

struct ConnectedDevice {
    std::string name;
    std::string address;
    std::string clientId;
};

struct ServerStats {
    bool connected = false;
    bool paused = false;
    std::string clientName;
    std::string clientAddress;
    uint64_t packetsSent = 0;
    uint64_t bytesSent = 0;
    double kbps = 0.0;
    float peakLevel = 0.0f;
    int jitterBufferMs = 10;
    uint32_t sequenceNum = 0;
    double uptimeSeconds = 0.0;
    std::string serverName;
    std::string serverMac;
};

struct PairRequest {
    std::string clientId;
    std::string clientName;
    bool pending = false;
};

class ServerGui {
public:
    using JitterChangeCallback = std::function<void(int bufferMs)>;
    using PairApproveCallback = std::function<void(const std::string& clientId)>;
    using PairDenyCallback = std::function<void(const std::string& clientId)>;
    using RevokeCallback = std::function<void(const std::string& clientId)>;
    using DeviceChangeCallback = std::function<void(const std::wstring& deviceId)>;
    ServerGui();
    ~ServerGui();

    bool initialize(const std::string& title);

    // Non-blocking: process pending window messages, returns false if window closed
    bool processMessages();

    // Update data (thread-safe)
    void updateStats(const ServerStats& stats);
    void updateApprovedPeers(const std::map<std::string, ApprovedPeer>& peers);
    void showPairRequest(const std::string& clientId, const std::string& name);
    void clearPairRequest();
    void addLogMessage(const std::string& msg);
    void updateDevices(const std::vector<std::pair<std::wstring, std::wstring>>& devices,
                       const std::wstring& currentId);
    void updateOutDevices(const std::vector<std::pair<std::wstring, std::wstring>>& devices,
                       const std::wstring& currentId);

    // Set callbacks
    void setJitterChangeCallback(JitterChangeCallback cb) { onJitterChange_ = cb; }
    void setPairApproveCallback(PairApproveCallback cb) { onPairApprove_ = cb; }
    void setPairDenyCallback(PairDenyCallback cb) { onPairDeny_ = cb; }
    void setRevokeCallback(RevokeCallback cb) { onRevoke_ = cb; }
    void setDeviceChangeCallback(DeviceChangeCallback cb) { onDeviceChange_ = cb; }
    void setOutDeviceChangeCallback(DeviceChangeCallback cb) { onOutDeviceChange_ = cb; }
    int getJitterBufferMs() const { return jitterBufferMs_; }

    HWND getHwnd() const { return hwnd_; }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT handleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    void createControls(HWND hwnd);
    void onPaint(HWND hwnd);
    void onResize(HWND hwnd, int width, int height);
    void drawSection(HDC hdc, RECT rc, const wchar_t* title);
    void drawStatusCard(HDC hdc, RECT rc);
    void drawJitterSection(HDC hdc, RECT rc);
    void drawConnectedSection(HDC hdc, RECT rc);
    void drawPairedSection(HDC hdc, RECT rc);
    void drawPairRequestSection(HDC hdc, RECT rc);
    void drawLogSection(HDC hdc, RECT rc);
    void refreshDisplay();

    HWND hwnd_ = nullptr;
    HWND jitterSlider_ = nullptr;
    HWND jitterLabel_ = nullptr;
    HWND approveBtn_ = nullptr;
    HWND denyBtn_ = nullptr;
    HWND deviceCombo_ = nullptr;
    HWND outDeviceCombo_ = nullptr;
    HFONT fontTitle_ = nullptr;
    HFONT fontNormal_ = nullptr;
    HFONT fontSmall_ = nullptr;
    HFONT fontMono_ = nullptr;
    HFONT fontBold_ = nullptr;
    HBRUSH bgBrush_ = nullptr;
    HBRUSH cardBrush_ = nullptr;
    HBRUSH accentBrush_ = nullptr;

    int jitterBufferMs_ = 10;

    // Re-entrancy guards for device combos
    bool updatingDevices_ = false;
    bool updatingOutDevices_ = false;

    // Thread-safe data
    mutable std::mutex dataMutex_;
    ServerStats stats_;
    std::vector<std::pair<std::string, std::string>> approvedPeers_;  // id, name
    std::vector<int64_t> peerTimestamps_;
    PairRequest pairRequest_;
    std::vector<std::string> logMessages_;
    static constexpr int MAX_LOG_MESSAGES = 50;

    // Callbacks
    JitterChangeCallback onJitterChange_;
    PairApproveCallback onPairApprove_;
    PairDenyCallback onPairDeny_;
    RevokeCallback onRevoke_;
    DeviceChangeCallback onDeviceChange_;
    DeviceChangeCallback onOutDeviceChange_;
    // Audio device list
    std::vector<std::pair<std::wstring, std::wstring>> audioDevices_; // id, name
    std::wstring currentDeviceId_;
    std::vector<std::pair<std::wstring, std::wstring>> outAudioDevices_; // id, name
    std::wstring currentOutDeviceId_;

    // Control IDs
    static constexpr int IDC_JITTER_SLIDER = 1001;
    static constexpr int IDC_APPROVE_BTN = 1002;
    static constexpr int IDC_DENY_BTN = 1003;
    static constexpr int IDC_DEVICE_COMBO = 1004;
    static constexpr int IDC_OUT_DEVICE_COMBO = 1005;
    static constexpr int IDC_TIMER_REFRESH = 2001;

    // Colors
    static constexpr COLORREF CLR_BG = RGB(15, 15, 20);
    static constexpr COLORREF CLR_CARD = RGB(24, 24, 34);
    static constexpr COLORREF CLR_CARD_BORDER = RGB(38, 38, 55);
    static constexpr COLORREF CLR_TEXT = RGB(225, 225, 240);
    static constexpr COLORREF CLR_TEXT_DIM = RGB(120, 120, 150);
    static constexpr COLORREF CLR_ACCENT = RGB(99, 102, 241);   // Indigo
    static constexpr COLORREF CLR_ACCENT_LIGHT = RGB(129, 140, 248);
    static constexpr COLORREF CLR_GREEN = RGB(52, 211, 153);
    static constexpr COLORREF CLR_RED = RGB(248, 113, 113);
    static constexpr COLORREF CLR_YELLOW = RGB(251, 191, 36);
    static constexpr COLORREF CLR_ORANGE = RGB(251, 146, 60);
    static constexpr COLORREF CLR_SEPARATOR = RGB(35, 35, 50);
};
