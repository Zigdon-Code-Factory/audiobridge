#include "gui.h"
#include "network.h"
#include <cstdio>
#include <ctime>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <dwmapi.h>

#pragma comment(lib, "dwmapi.lib")

// Helper: UTF-8 to wide string
static std::wstring toWide(const std::string& s) {
    if (s.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring ws(len, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &ws[0], len);
    return ws;
}

// Helper: draw rounded rect
static void drawRoundRect(HDC hdc, RECT rc, int radius, HBRUSH fillBrush, COLORREF borderColor) {
    HPEN pen = CreatePen(PS_SOLID, 1, borderColor);
    HPEN oldPen = (HPEN)SelectObject(hdc, pen);
    HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, fillBrush);
    RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, radius, radius);
    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBrush);
    DeleteObject(pen);
}

// Helper: draw text with color
static void drawColorText(HDC hdc, const wchar_t* text, RECT& rc, COLORREF color, 
                           UINT format = DT_LEFT | DT_SINGLELINE | DT_NOPREFIX) {
    SetTextColor(hdc, color);
    DrawTextW(hdc, text, -1, &rc, format);
}

// Helper: format bytes to human readable
static std::string formatBytes(uint64_t bytes) {
    if (bytes < 1024) return std::to_string(bytes) + " B";
    if (bytes < 1024 * 1024) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.1f KB", bytes / 1024.0);
        return buf;
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "%.2f MB", bytes / (1024.0 * 1024.0));
    return buf;
}

// Helper: format uptime
static std::string formatUptime(double seconds) {
    int h = (int)(seconds / 3600);
    int m = (int)(fmod(seconds, 3600) / 60);
    int s = (int)(fmod(seconds, 60));
    char buf[32];
    if (h > 0)
        snprintf(buf, sizeof(buf), "%dh %02dm %02ds", h, m, s);
    else if (m > 0)
        snprintf(buf, sizeof(buf), "%dm %02ds", m, s);
    else
        snprintf(buf, sizeof(buf), "%ds", s);
    return buf;
}


ServerGui::ServerGui() {}

ServerGui::~ServerGui() {
    if (fontTitle_) DeleteObject(fontTitle_);
    if (fontNormal_) DeleteObject(fontNormal_);
    if (fontSmall_) DeleteObject(fontSmall_);
    if (fontMono_) DeleteObject(fontMono_);
    if (fontBold_) DeleteObject(fontBold_);
    if (bgBrush_) DeleteObject(bgBrush_);
    if (cardBrush_) DeleteObject(cardBrush_);
    if (accentBrush_) DeleteObject(accentBrush_);
    if (hwnd_) DestroyWindow(hwnd_);
}

bool ServerGui::initialize(const std::string& title) {
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_BAR_CLASSES | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"AudioBridgeServerGui";
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wc.hIconSm = LoadIcon(nullptr, IDI_APPLICATION);

    if (!RegisterClassExW(&wc)) {
        printf("Failed to register window class\n");
        return false;
    }

    // Create fonts
    fontTitle_ = CreateFontW(22, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    fontNormal_ = CreateFontW(15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                               CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    fontSmall_ = CreateFontW(12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    fontMono_ = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");
    fontBold_ = CreateFontW(14, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");

    // Create brushes
    bgBrush_ = CreateSolidBrush(CLR_BG);
    cardBrush_ = CreateSolidBrush(CLR_CARD);
    accentBrush_ = CreateSolidBrush(CLR_ACCENT);

    std::wstring wtitle = L"AudioBridge \u2014 " + toWide(title);
    hwnd_ = CreateWindowExW(
        0, L"AudioBridgeServerGui", wtitle.c_str(),
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 680, 820,
        nullptr, nullptr, GetModuleHandle(nullptr), this
    );

    if (!hwnd_) {
        printf("Failed to create window\n");
        return false;
    }

    // Enable dark title bar (Windows 10+)
    BOOL darkMode = TRUE;
    DwmSetWindowAttribute(hwnd_, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &darkMode, sizeof(darkMode));

    // Create controls
    createControls(hwnd_);

    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);

    // Start refresh timer (200ms)
    SetTimer(hwnd_, IDC_TIMER_REFRESH, 200, nullptr);

    return true;
}

void ServerGui::createControls(HWND hwnd) {
    // Jitter slider (trackbar) — will be positioned in onResize
    jitterSlider_ = CreateWindowExW(
        0, TRACKBAR_CLASSW, nullptr,
        WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS,
        0, 0, 200, 30,
        hwnd, (HMENU)IDC_JITTER_SLIDER, GetModuleHandle(nullptr), nullptr
    );
    SendMessage(jitterSlider_, TBM_SETRANGE, TRUE, MAKELPARAM(0, 50));  // 0-50ms
    SendMessage(jitterSlider_, TBM_SETPOS, TRUE, jitterBufferMs_);
    SendMessage(jitterSlider_, TBM_SETTICFREQ, 10, 0);

    // Audio device dropdown
    deviceCombo_ = CreateWindowExW(
        0, L"COMBOBOX", nullptr,
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
        0, 0, 200, 300,
        hwnd, (HMENU)IDC_DEVICE_COMBO, GetModuleHandle(nullptr), nullptr
    );
    SendMessage(deviceCombo_, CB_ADDSTRING, 0, (LPARAM)L"Default Device");
    SendMessage(deviceCombo_, CB_SETCURSEL, 0, 0);

    // Audio output device dropdown
    outDeviceCombo_ = CreateWindowExW(
        0, L"COMBOBOX", nullptr,
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
        0, 0, 200, 300,
        hwnd, (HMENU)IDC_OUT_DEVICE_COMBO, GetModuleHandle(nullptr), nullptr
    );
    SendMessage(outDeviceCombo_, CB_ADDSTRING, 0, (LPARAM)L"Default Device");
    SendMessage(outDeviceCombo_, CB_SETCURSEL, 0, 0);

    // Approve/Deny buttons (hidden until pair request)
    approveBtn_ = CreateWindowExW(
        0, L"BUTTON", L"\u2713 Approve",
        WS_CHILD | BS_PUSHBUTTON | BS_FLAT,
        0, 0, 100, 32, hwnd, (HMENU)IDC_APPROVE_BTN, GetModuleHandle(nullptr), nullptr
    );
    denyBtn_ = CreateWindowExW(
        0, L"BUTTON", L"\u2717 Deny",
        WS_CHILD | BS_PUSHBUTTON | BS_FLAT,
        0, 0, 100, 32, hwnd, (HMENU)IDC_DENY_BTN, GetModuleHandle(nullptr), nullptr
    );

    SendMessage(jitterSlider_, WM_SETFONT, (WPARAM)fontNormal_, TRUE);
    SendMessage(deviceCombo_, WM_SETFONT, (WPARAM)fontNormal_, TRUE);
    SendMessage(outDeviceCombo_, WM_SETFONT, (WPARAM)fontNormal_, TRUE);
    SendMessage(approveBtn_, WM_SETFONT, (WPARAM)fontBold_, TRUE);
    SendMessage(denyBtn_, WM_SETFONT, (WPARAM)fontBold_, TRUE);
}

bool ServerGui::processMessages() {
    MSG msg;
    while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) return false;
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return IsWindow(hwnd_) != FALSE;
}

void ServerGui::updateStats(const ServerStats& stats) {
    std::lock_guard<std::mutex> lock(dataMutex_);
    stats_ = stats;
}

void ServerGui::updateApprovedPeers(const std::map<std::string, ApprovedPeer>& peers) {
    std::lock_guard<std::mutex> lock(dataMutex_);
    approvedPeers_.clear();
    peerTimestamps_.clear();
    for (const auto& [id, peer] : peers) {
        approvedPeers_.push_back({peer.clientId, peer.clientName});
        peerTimestamps_.push_back(peer.lastConnected);
    }
}

void ServerGui::showPairRequest(const std::string& clientId, const std::string& name) {
    {
        std::lock_guard<std::mutex> lock(dataMutex_);
        pairRequest_.clientId = clientId;
        pairRequest_.clientName = name;
        pairRequest_.pending = true;
    }
    // Show approve/deny buttons
    ShowWindow(approveBtn_, SW_SHOW);
    ShowWindow(denyBtn_, SW_SHOW);

    // Flash the window to get attention
    FLASHWINFO fi = { sizeof(fi), hwnd_, FLASHW_ALL | FLASHW_TIMERNOFG, 5, 0 };
    FlashWindowEx(&fi);

    // Also bring to front
    SetForegroundWindow(hwnd_);
}

void ServerGui::clearPairRequest() {
    {
        std::lock_guard<std::mutex> lock(dataMutex_);
        pairRequest_.pending = false;
    }
    ShowWindow(approveBtn_, SW_HIDE);
    ShowWindow(denyBtn_, SW_HIDE);
}

void ServerGui::addLogMessage(const std::string& msg) {
    std::lock_guard<std::mutex> lock(dataMutex_);
    // Add timestamp
    time_t now = std::time(nullptr);
    struct tm t;
    localtime_s(&t, &now);
    char timeBuf[16];
    snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
    logMessages_.push_back(std::string(timeBuf) + "  " + msg);
    if (logMessages_.size() > MAX_LOG_MESSAGES) {
        logMessages_.erase(logMessages_.begin());
    }
}

void ServerGui::updateDevices(const std::vector<std::pair<std::wstring, std::wstring>>& devices,
                              const std::wstring& currentId) {
    {
        std::lock_guard<std::mutex> lock(dataMutex_);
        audioDevices_ = devices;
        currentDeviceId_ = currentId;
    }

    if (deviceCombo_) {
        SendMessage(deviceCombo_, CB_RESETCONTENT, 0, 0);
        SendMessage(deviceCombo_, CB_ADDSTRING, 0, (LPARAM)L"Default Device");

        int selectIndex = 0;
        for (size_t i = 0; i < devices.size(); i++) {
            SendMessage(deviceCombo_, CB_ADDSTRING, 0, (LPARAM)devices[i].second.c_str());
            if (devices[i].first == currentId) {
                selectIndex = (int)(i + 1);
            }
        }
        SendMessage(deviceCombo_, CB_SETCURSEL, selectIndex, 0);
    }
}

void ServerGui::updateOutDevices(const std::vector<std::pair<std::wstring, std::wstring>>& devices,
                               const std::wstring& currentId) {
    {
        std::lock_guard<std::mutex> lock(dataMutex_);
        outAudioDevices_ = devices;
        currentOutDeviceId_ = currentId;
    }

    if (outDeviceCombo_) {
        SendMessage(outDeviceCombo_, CB_RESETCONTENT, 0, 0);
        SendMessage(outDeviceCombo_, CB_ADDSTRING, 0, (LPARAM)L"Default Device");

        int selectIndex = 0;
        for (size_t i = 0; i < devices.size(); i++) {
            SendMessage(outDeviceCombo_, CB_ADDSTRING, 0, (LPARAM)devices[i].second.c_str());
            if (devices[i].first == currentId) {
                selectIndex = (int)(i + 1);
            }
        }
        SendMessage(outDeviceCombo_, CB_SETCURSEL, selectIndex, 0);
    }
}

LRESULT CALLBACK ServerGui::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    ServerGui* gui = nullptr;
    if (msg == WM_NCCREATE) {
        auto cs = reinterpret_cast<CREATESTRUCT*>(lParam);
        gui = reinterpret_cast<ServerGui*>(cs->lpCreateParams);
        SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(gui));
    } else {
        gui = reinterpret_cast<ServerGui*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    }
    if (gui) {
        return gui->handleMessage(hwnd, msg, wParam, lParam);
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

LRESULT ServerGui::handleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_PAINT:
        onPaint(hwnd);
        return 0;

    case WM_ERASEBKGND:
        return 1;  // We handle background in WM_PAINT

    case WM_SIZE:
        onResize(hwnd, LOWORD(lParam), HIWORD(lParam));
        return 0;

    case WM_HSCROLL:
        if ((HWND)lParam == jitterSlider_) {
            int pos = (int)SendMessage(jitterSlider_, TBM_GETPOS, 0, 0);
            jitterBufferMs_ = pos;
            if (onJitterChange_) onJitterChange_(pos);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;

    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_DEVICE_COMBO && HIWORD(wParam) == CBN_SELCHANGE) {
            int sel = (int)SendMessage(deviceCombo_, CB_GETCURSEL, 0, 0);
            if (sel >= 0 && onDeviceChange_) {
                std::wstring deviceId;
                bool valid = false;
                {
                    std::lock_guard<std::mutex> lock(dataMutex_);
                    if (sel == 0) {
                        valid = true;
                    } else if (sel - 1 < (int)audioDevices_.size()) {
                        deviceId = audioDevices_[sel - 1].first;
                        valid = true;
                    }
                }
                if (valid) onDeviceChange_(deviceId);
            }
        } else if (LOWORD(wParam) == IDC_OUT_DEVICE_COMBO && HIWORD(wParam) == CBN_SELCHANGE) {
            int sel = (int)SendMessage(outDeviceCombo_, CB_GETCURSEL, 0, 0);
            if (sel >= 0 && onOutDeviceChange_) {
                std::wstring deviceId;
                bool valid = false;
                {
                    std::lock_guard<std::mutex> lock(dataMutex_);
                    if (sel == 0) {
                        valid = true;
                    } else if (sel - 1 < (int)outAudioDevices_.size()) {
                        deviceId = outAudioDevices_[sel - 1].first;
                        valid = true;
                    }
                }
                if (valid) onOutDeviceChange_(deviceId);
            }
        } else if (LOWORD(wParam) == IDC_APPROVE_BTN) {
            std::string clientId;
            {
                std::lock_guard<std::mutex> lock(dataMutex_);
                clientId = pairRequest_.clientId;
            }
            if (!clientId.empty() && onPairApprove_) {
                onPairApprove_(clientId);
                clearPairRequest();
                addLogMessage("Approved pairing: " + clientId);
            }
        } else if (LOWORD(wParam) == IDC_DENY_BTN) {
            std::string clientId;
            {
                std::lock_guard<std::mutex> lock(dataMutex_);
                clientId = pairRequest_.clientId;
            }
            if (!clientId.empty() && onPairDeny_) {
                onPairDeny_(clientId);
                clearPairRequest();
                addLogMessage("Denied pairing: " + clientId);
            }
        }
        return 0;

    case WM_TIMER:
        if (wParam == IDC_TIMER_REFRESH) {
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;

    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLORLISTBOX: {
        HDC hdc = (HDC)wParam;
        SetBkColor(hdc, CLR_CARD);
        SetTextColor(hdc, CLR_TEXT);
        return (LRESULT)cardBrush_;
    }

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    default:
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
}

void ServerGui::onResize(HWND hwnd, int width, int height) {
    // Position jitter slider
    int sliderX = 150;
    int sliderY = 340;
    int sliderW = width - 190;
    if (jitterSlider_) {
        MoveWindow(jitterSlider_, sliderX, sliderY, sliderW, 25, TRUE);
    }

    // Position approve/deny buttons (in pair request section area)
    int btnY = height - 170;  // Rough position
    if (approveBtn_) MoveWindow(approveBtn_, 30, btnY, 120, 34, TRUE);
    if (denyBtn_) MoveWindow(denyBtn_, 160, btnY, 120, 34, TRUE);

    InvalidateRect(hwnd, nullptr, FALSE);
}

void ServerGui::onPaint(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);

    RECT clientRect;
    GetClientRect(hwnd, &clientRect);
    int w = clientRect.right;
    int h = clientRect.bottom;

    // Double-buffer
    HDC memDC = CreateCompatibleDC(hdc);
    HBITMAP memBmp = CreateCompatibleBitmap(hdc, w, h);
    HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, memBmp);

    // Background
    FillRect(memDC, &clientRect, bgBrush_);
    SetBkMode(memDC, TRANSPARENT);

    // Title bar area
    HFONT oldFont = (HFONT)SelectObject(memDC, fontTitle_);
    RECT titleRect = {20, 12, w - 20, 44};
    drawColorText(memDC, L"\u266B  AudioBridge Server", titleRect, CLR_ACCENT_LIGHT);

    // Version label
    SelectObject(memDC, fontSmall_);
    RECT verRect = {w - 80, 18, w - 20, 36};
    drawColorText(memDC, L"v1.0", verRect, CLR_TEXT_DIM, DT_RIGHT | DT_SINGLELINE);

    // Separator
    RECT sepRect = {20, 46, w - 20, 47};
    FillRect(memDC, &sepRect, CreateSolidBrush(CLR_SEPARATOR));

    int y = 56;
    int cardMargin = 16;
    int cardPad = 14;
    int cardWidth = w - cardMargin * 2;

    // === STATUS CARD ===
    {
        RECT cardRc = {cardMargin, y, w - cardMargin, y + 120};
        drawRoundRect(memDC, cardRc, 10, cardBrush_, CLR_CARD_BORDER);

        SelectObject(memDC, fontBold_);
        RECT headerRc = {cardRc.left + cardPad, cardRc.top + 10, cardRc.right - cardPad, cardRc.top + 28};
        drawColorText(memDC, L"\u25CF  STATUS", headerRc, CLR_TEXT_DIM);

        std::lock_guard<std::mutex> lock(dataMutex_);
        SelectObject(memDC, fontNormal_);

        // Connection status indicator
        COLORREF statusColor = stats_.connected ? (stats_.paused ? CLR_YELLOW : CLR_GREEN) : CLR_RED;
        std::wstring statusText = stats_.connected
            ? (stats_.paused ? L"Paused" : L"Streaming")
            : L"Waiting for client...";

        // Status dot
        HBRUSH dotBrush = CreateSolidBrush(statusColor);
        RECT dotRc = {cardRc.left + cardPad, cardRc.top + 38, cardRc.left + cardPad + 10, cardRc.top + 48};
        HBRUSH oldBr = (HBRUSH)SelectObject(memDC, dotBrush);
        HPEN noPen = CreatePen(PS_NULL, 0, 0);
        HPEN oldP = (HPEN)SelectObject(memDC, noPen);
        Ellipse(memDC, dotRc.left, dotRc.top, dotRc.right, dotRc.bottom);
        SelectObject(memDC, oldP);
        SelectObject(memDC, oldBr);
        DeleteObject(dotBrush);
        DeleteObject(noPen);

        RECT statusRc = {cardRc.left + cardPad + 16, cardRc.top + 34, cardRc.right - cardPad, cardRc.top + 52};
        drawColorText(memDC, statusText.c_str(), statusRc, statusColor);

        if (stats_.connected) {
            // Client name and address
            SelectObject(memDC, fontMono_);
            std::wstring clientInfo = toWide(stats_.clientName) + L"  \u2022  " + toWide(stats_.clientAddress);
            RECT clientRc = {cardRc.left + cardPad, cardRc.top + 56, cardRc.right - cardPad, cardRc.top + 72};
            drawColorText(memDC, clientInfo.c_str(), clientRc, CLR_TEXT);

            // Stats row
            SelectObject(memDC, fontSmall_);
            char statsBuf[256];
            snprintf(statsBuf, sizeof(statsBuf), 
                     "Packets: %llu    Data: %s    Rate: %.1f kbps    Seq: %u",
                     (unsigned long long)stats_.packetsSent, formatBytes(stats_.bytesSent).c_str(),
                     stats_.kbps, stats_.sequenceNum);
            std::wstring statsW = toWide(statsBuf);
            RECT statsRc = {cardRc.left + cardPad, cardRc.top + 78, cardRc.right - cardPad, cardRc.top + 94};
            drawColorText(memDC, statsW.c_str(), statsRc, CLR_TEXT_DIM);

            // Uptime and peak level
            char extraBuf[128];
            snprintf(extraBuf, sizeof(extraBuf), "Uptime: %s    Peak: %.3f%s",
                     formatUptime(stats_.uptimeSeconds).c_str(),
                     stats_.peakLevel,
                     stats_.peakLevel > 0.95f ? " (HOT!)" : "");
            std::wstring extraW = toWide(extraBuf);
            RECT extraRc = {cardRc.left + cardPad, cardRc.top + 94, cardRc.right - cardPad, cardRc.top + 110};
            drawColorText(memDC, extraW.c_str(), extraRc, CLR_TEXT_DIM);
        } else {
            // Server info when not connected
            SelectObject(memDC, fontSmall_);
            std::wstring serverInfo = L"Server: " + toWide(stats_.serverName) + L"  \u2022  MAC: " + toWide(stats_.serverMac);
            RECT infoRc = {cardRc.left + cardPad, cardRc.top + 56, cardRc.right - cardPad, cardRc.top + 72};
            drawColorText(memDC, serverInfo.c_str(), infoRc, CLR_TEXT_DIM);

            std::wstring portInfo = L"Discovery: :4011  \u2022  Stream: :4012";
            RECT portRc = {cardRc.left + cardPad, cardRc.top + 72, cardRc.right - cardPad, cardRc.top + 88};
            drawColorText(memDC, portInfo.c_str(), portRc, CLR_TEXT_DIM);
        }
    }
    y += 130;

    // === AUDIO LEVEL BAR ===
    {
        std::lock_guard<std::mutex> lock(dataMutex_);
        RECT barBg = {cardMargin, y, w - cardMargin, y + 6};
        HBRUSH barBgBr = CreateSolidBrush(CLR_SEPARATOR);
        FillRect(memDC, &barBg, barBgBr);
        DeleteObject(barBgBr);

        if (stats_.connected) {
            float level = stats_.peakLevel;
            if (level > 1.0f) level = 1.0f;
            int barWidth = (int)((w - cardMargin * 2) * level);
            COLORREF barColor = level > 0.9f ? CLR_RED : (level > 0.7f ? CLR_ORANGE : CLR_GREEN);
            RECT barFg = {cardMargin, y, cardMargin + barWidth, y + 6};
            HBRUSH barFgBr = CreateSolidBrush(barColor);
            FillRect(memDC, &barFg, barFgBr);
            DeleteObject(barFgBr);
        }
    }
    y += 16;

    // === AUDIO DEVICE CARD ===
    {
        RECT cardRc = {cardMargin, y, w - cardMargin, y + 100};
        drawRoundRect(memDC, cardRc, 10, cardBrush_, CLR_CARD_BORDER);

        SelectObject(memDC, fontBold_);
        RECT headerRc = {cardRc.left + cardPad, cardRc.top + 10, cardRc.right - cardPad, cardRc.top + 28};
        drawColorText(memDC, L"\u266B  AUDIO DEVICES", headerRc, CLR_TEXT_DIM);

        // Subheaders
        SelectObject(memDC, fontSmall_);
        RECT inRc = {cardRc.left + cardPad, cardRc.top + 34, cardRc.right - cardPad, cardRc.top + 50};
        drawColorText(memDC, L"Streaming from PC (Input Device):", inRc, CLR_TEXT_DIM);

        RECT outRc = {cardRc.left + cardPad, cardRc.top + 64, cardRc.right - cardPad, cardRc.top + 80};
        drawColorText(memDC, L"Mic from Phone (Output Device):", outRc, CLR_TEXT_DIM);

        // Position combo boxes inside this card
        int labelWidth = 240;
        if (deviceCombo_) {
            MoveWindow(deviceCombo_, cardRc.left + cardPad + labelWidth, cardRc.top + 30,
                       cardRc.right - cardRc.left - cardPad * 2 - labelWidth, 300, TRUE);
        }
        if (outDeviceCombo_) {
            MoveWindow(outDeviceCombo_, cardRc.left + cardPad + labelWidth, cardRc.top + 60,
                       cardRc.right - cardRc.left - cardPad * 2 - labelWidth, 300, TRUE);
        }
    }
    y += 110;

    // === JITTER BUFFER CARD ===
    {
        RECT cardRc = {cardMargin, y, w - cardMargin, y + 80};
        drawRoundRect(memDC, cardRc, 10, cardBrush_, CLR_CARD_BORDER);

        SelectObject(memDC, fontBold_);
        RECT headerRc = {cardRc.left + cardPad, cardRc.top + 10, cardRc.right - cardPad, cardRc.top + 28};
        drawColorText(memDC, L"\u23F1  JITTER BUFFER", headerRc, CLR_TEXT_DIM);

        // Show current value
        SelectObject(memDC, fontNormal_);
        wchar_t valBuf[32];
        swprintf(valBuf, 32, L"%d ms", jitterBufferMs_);
        RECT valRc = {cardRc.right - 80, cardRc.top + 10, cardRc.right - cardPad, cardRc.top + 28};
        drawColorText(memDC, valBuf, valRc, CLR_ACCENT_LIGHT, DT_RIGHT | DT_SINGLELINE);

        // Slider labels
        SelectObject(memDC, fontSmall_);
        RECT minRc = {cardRc.left + cardPad, cardRc.top + 60, cardRc.left + 60, cardRc.top + 74};
        drawColorText(memDC, L"0 ms", minRc, CLR_TEXT_DIM);
        RECT maxRc = {cardRc.right - 60, cardRc.top + 60, cardRc.right - cardPad, cardRc.top + 74};
        drawColorText(memDC, L"50 ms", maxRc, CLR_TEXT_DIM, DT_RIGHT | DT_SINGLELINE);

        // Reposition slider inside this card
        if (jitterSlider_) {
            MoveWindow(jitterSlider_, cardRc.left + cardPad, cardRc.top + 34,
                       cardRc.right - cardRc.left - cardPad * 2, 24, TRUE);
        }
    }
    y += 90;

    // === CONNECTED DEVICES CARD ===
    {
        RECT cardRc = {cardMargin, y, w - cardMargin, y + 80};
        drawRoundRect(memDC, cardRc, 10, cardBrush_, CLR_CARD_BORDER);

        SelectObject(memDC, fontBold_);
        RECT headerRc = {cardRc.left + cardPad, cardRc.top + 10, cardRc.right - cardPad, cardRc.top + 28};
        drawColorText(memDC, L"\u26A1  CONNECTED DEVICES", headerRc, CLR_TEXT_DIM);

        std::lock_guard<std::mutex> lock(dataMutex_);
        if (stats_.connected) {
            SelectObject(memDC, fontNormal_);
            std::wstring devLine = L"\u2022  " + toWide(stats_.clientName);
            RECT devRc = {cardRc.left + cardPad + 4, cardRc.top + 36, cardRc.right - 160, cardRc.top + 54};
            drawColorText(memDC, devLine.c_str(), devRc, CLR_TEXT);

            SelectObject(memDC, fontMono_);
            std::wstring addrLine = toWide(stats_.clientAddress);
            RECT addrRc = {cardRc.right - 200, cardRc.top + 36, cardRc.right - cardPad, cardRc.top + 54};
            drawColorText(memDC, addrLine.c_str(), addrRc, CLR_TEXT_DIM, DT_RIGHT | DT_SINGLELINE);

            // Status badge
            std::wstring badge = stats_.paused ? L"PAUSED" : L"STREAMING";
            COLORREF badgeColor = stats_.paused ? CLR_YELLOW : CLR_GREEN;
            SelectObject(memDC, fontSmall_);
            RECT badgeRc = {cardRc.left + cardPad + 20, cardRc.top + 56, cardRc.right - cardPad, cardRc.top + 70};
            drawColorText(memDC, badge.c_str(), badgeRc, badgeColor);
        } else {
            SelectObject(memDC, fontNormal_);
            RECT emptyRc = {cardRc.left + cardPad, cardRc.top + 36, cardRc.right - cardPad, cardRc.top + 56};
            drawColorText(memDC, L"No devices connected", emptyRc, CLR_TEXT_DIM);
        }
    }
    y += 90;

    // === PAIR REQUEST CARD (only if pending) ===
    bool hasPairRequest = false;
    {
        std::lock_guard<std::mutex> lock(dataMutex_);
        hasPairRequest = pairRequest_.pending;
    }
    if (hasPairRequest) {
        RECT cardRc = {cardMargin, y, w - cardMargin, y + 90};
        // Highlight border for attention
        drawRoundRect(memDC, cardRc, 10, cardBrush_, CLR_YELLOW);

        SelectObject(memDC, fontBold_);
        RECT headerRc = {cardRc.left + cardPad, cardRc.top + 10, cardRc.right - cardPad, cardRc.top + 28};
        drawColorText(memDC, L"\u26A0  PAIRING REQUEST", headerRc, CLR_YELLOW);

        std::lock_guard<std::mutex> lock(dataMutex_);
        SelectObject(memDC, fontNormal_);
        std::wstring reqText = L"\"" + toWide(pairRequest_.clientName) + L"\" wants to connect";
        RECT reqRc = {cardRc.left + cardPad, cardRc.top + 34, cardRc.right - cardPad, cardRc.top + 52};
        drawColorText(memDC, reqText.c_str(), reqRc, CLR_TEXT);

        SelectObject(memDC, fontMono_);
        std::wstring idText = L"ID: " + toWide(pairRequest_.clientId);
        RECT idRc = {cardRc.left + cardPad, cardRc.top + 54, cardRc.right - cardPad, cardRc.top + 68};
        drawColorText(memDC, idText.c_str(), idRc, CLR_TEXT_DIM);

        // Position buttons inside this card
        if (approveBtn_) MoveWindow(approveBtn_, cardRc.left + cardPad, cardRc.top + 72, 110, 30, TRUE);
        if (denyBtn_) MoveWindow(denyBtn_, cardRc.left + cardPad + 120, cardRc.top + 72, 100, 30, TRUE);

        // Extend card to fit buttons
        cardRc.bottom = cardRc.top + 110;
        // Redraw with extended size
        drawRoundRect(memDC, cardRc, 10, cardBrush_, CLR_YELLOW);
        // Redraw text on top
        SelectObject(memDC, fontBold_);
        drawColorText(memDC, L"\u26A0  PAIRING REQUEST", headerRc, CLR_YELLOW);
        SelectObject(memDC, fontNormal_);
        drawColorText(memDC, reqText.c_str(), reqRc, CLR_TEXT);
        SelectObject(memDC, fontMono_);
        drawColorText(memDC, idText.c_str(), idRc, CLR_TEXT_DIM);

        y += 120;
    }

    // === PAIRED DEVICES CARD ===
    {
        int peerCount = 0;
        {
            std::lock_guard<std::mutex> lock(dataMutex_);
            peerCount = (int)approvedPeers_.size();
        }
        int cardHeight = 40 + (std::max)(1, peerCount) * 22;
        RECT cardRc = {cardMargin, y, w - cardMargin, y + cardHeight};
        drawRoundRect(memDC, cardRc, 10, cardBrush_, CLR_CARD_BORDER);

        SelectObject(memDC, fontBold_);
        wchar_t peerHeader[64];
        swprintf(peerHeader, 64, L"\u2605  PAIRED DEVICES (%d)", peerCount);
        RECT headerRc = {cardRc.left + cardPad, cardRc.top + 10, cardRc.right - cardPad, cardRc.top + 28};
        drawColorText(memDC, peerHeader, headerRc, CLR_TEXT_DIM);

        std::lock_guard<std::mutex> lock(dataMutex_);
        if (approvedPeers_.empty()) {
            SelectObject(memDC, fontNormal_);
            RECT emptyRc = {cardRc.left + cardPad, cardRc.top + 34, cardRc.right - cardPad, cardRc.top + 52};
            drawColorText(memDC, L"No paired devices", emptyRc, CLR_TEXT_DIM);
        } else {
            int py = cardRc.top + 34;
            for (size_t i = 0; i < approvedPeers_.size(); i++) {
                SelectObject(memDC, fontNormal_);
                std::wstring name = L"\u2022  " + toWide(approvedPeers_[i].second);
                RECT nameRc = {cardRc.left + cardPad + 4, py, cardRc.left + 250, py + 18};
                drawColorText(memDC, name.c_str(), nameRc, CLR_TEXT);

                // Show last connected time
                if (i < peerTimestamps_.size()) {
                    int64_t age = std::time(nullptr) - peerTimestamps_[i];
                    int days = (int)(age / 86400);
                    int hours = (int)((age % 86400) / 3600);
                    char ageBuf[48];
                    if (days > 0)
                        snprintf(ageBuf, sizeof(ageBuf), "%dd ago (expires %dd)", days, 30 - days);
                    else
                        snprintf(ageBuf, sizeof(ageBuf), "%dh ago", hours);

                    SelectObject(memDC, fontSmall_);
                    std::wstring ageW = toWide(ageBuf);
                    RECT ageRc = {cardRc.right - 200, py + 2, cardRc.right - cardPad, py + 16};
                    drawColorText(memDC, ageW.c_str(), ageRc, CLR_TEXT_DIM, DT_RIGHT | DT_SINGLELINE);
                }

                // Truncated ID
                SelectObject(memDC, fontMono_);
                std::string shortId = approvedPeers_[i].first;
                if (shortId.length() > 17) shortId = shortId.substr(0, 17);
                std::wstring idW = toWide(shortId);
                RECT idRc = {cardRc.left + 250, py + 2, cardRc.right - 200, py + 16};
                drawColorText(memDC, idW.c_str(), idRc, CLR_TEXT_DIM);

                py += 22;
            }
        }

        y += cardHeight + 10;
    }

    // === LOG CARD ===
    {
        int logHeight = h - y - 10;
        if (logHeight < 60) logHeight = 60;
        RECT cardRc = {cardMargin, y, w - cardMargin, y + logHeight};
        drawRoundRect(memDC, cardRc, 10, cardBrush_, CLR_CARD_BORDER);

        SelectObject(memDC, fontBold_);
        RECT headerRc = {cardRc.left + cardPad, cardRc.top + 10, cardRc.right - cardPad, cardRc.top + 28};
        drawColorText(memDC, L"\u25B6  LOG", headerRc, CLR_TEXT_DIM);

        std::lock_guard<std::mutex> lock(dataMutex_);
        SelectObject(memDC, fontMono_);
        int ly = cardRc.top + 32;
        int maxLines = (logHeight - 40) / 16;
        int startIdx = (int)logMessages_.size() - maxLines;
        if (startIdx < 0) startIdx = 0;

        for (int i = startIdx; i < (int)logMessages_.size() && ly < cardRc.bottom - 10; i++) {
            std::wstring logW = toWide(logMessages_[i]);
            RECT logRc = {cardRc.left + cardPad, ly, cardRc.right - cardPad, ly + 16};
            drawColorText(memDC, logW.c_str(), logRc, CLR_TEXT_DIM);
            ly += 16;
        }

        if (logMessages_.empty()) {
            SelectObject(memDC, fontNormal_);
            RECT emptyRc = {cardRc.left + cardPad, cardRc.top + 36, cardRc.right - cardPad, cardRc.top + 54};
            drawColorText(memDC, L"No log messages", emptyRc, CLR_TEXT_DIM);
        }
    }

    SelectObject(memDC, oldFont);

    // Blit to screen
    BitBlt(hdc, 0, 0, w, h, memDC, 0, 0, SRCCOPY);

    SelectObject(memDC, oldBmp);
    DeleteObject(memBmp);
    DeleteDC(memDC);

    EndPaint(hwnd, &ps);
}

void ServerGui::refreshDisplay() {
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}
