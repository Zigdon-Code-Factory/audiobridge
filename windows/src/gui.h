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
    int jitterBufferMs = 10;  // Current jitter buffer target (ms)
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

    // Set callbacks
    void setJitterChangeCallback(JitterChangeCallback cb) { onJitterChange_ = cb; }
    void setPairApproveCallback(PairApproveCallback cb) { onPairApprove_ = cb; }
    void setPairDenyCallback(PairDenyCallback cb) { onPairDeny_ = cb; }
    void setRevokeCallback(RevokeCallback cb) { onRevoke_ = cb; }

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

    HFONT fontTitle_ = nullptr;
    HFONT fontNormal_ = nullptr;
    HFONT fontSmall_ = nullptr;
    HFONT fontMono_ = nullptr;
    HFONT fontBold_ = nullptr;
    HBRUSH bgBrush_ = nullptr;
    HBRUSH cardBrush_ = nullptr;
    HBRUSH accentBrush_ = nullptr;

    int jitterBufferMs_ = 10;

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

    // Control IDs
    static constexpr int IDC_JITTER_SLIDER = 1001;
    static constexpr int IDC_APPROVE_BTN = 1002;
    static constexpr int IDC_DENY_BTN = 1003;
    static constexpr int IDC_TIMER_REFRESH = 2001;

    // Colors
    static constexpr COLORREF CLR_BG = RGB(18, 18, 24);
    static constexpr COLORREF CLR_CARD = RGB(28, 28, 40);
    static constexpr COLORREF CLR_CARD_BORDER = RGB(45, 45, 65);
    static constexpr COLORREF CLR_TEXT = RGB(220, 220, 235);
    static constexpr COLORREF CLR_TEXT_DIM = RGB(130, 130, 155);
    static constexpr COLORREF CLR_ACCENT = RGB(99, 102, 241);   // Indigo
    static constexpr COLORREF CLR_ACCENT_LIGHT = RGB(129, 140, 248);
    static constexpr COLORREF CLR_GREEN = RGB(52, 211, 153);
    static constexpr COLORREF CLR_RED = RGB(248, 113, 113);
    static constexpr COLORREF CLR_YELLOW = RGB(251, 191, 36);
    static constexpr COLORREF CLR_ORANGE = RGB(251, 146, 60);
    static constexpr COLORREF CLR_SEPARATOR = RGB(40, 40, 58);
};
