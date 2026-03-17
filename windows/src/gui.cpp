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
    fontTitle_ = CreateFontW(20, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    fontNormal_ = CreateFontW(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                               CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    fontSmall_ = CreateFontW(12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    fontMono_ = CreateFontW(12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");
    fontBold_ = CreateFontW(13, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
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
        CW_USEDEFAULT, CW_USEDEFAULT, 580, 780,
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
    // Jitter slider
    jitterSlider_ = CreateWindowExW(
        0, TRACKBAR_CLASSW, nullptr,
        WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS,
        0, 0, 200, 25,
        hwnd, (HMENU)IDC_JITTER_SLIDER, GetModuleHandle(nullptr), nullptr
    );
    SendMessage(jitterSlider_, TBM_SETRANGE, TRUE, MAKELPARAM(0, 50));
    SendMessage(jitterSlider_, TBM_SETPOS, TRUE, jitterBufferMs_);

    // Audio device dropdowns
    deviceCombo_ = CreateWindowExW(
        0, L"COMBOBOX", nullptr,
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
        0, 0, 200, 300,
        hwnd, (HMENU)IDC_DEVICE_COMBO, GetModuleHandle(nullptr), nullptr
    );
    SendMessageW(deviceCombo_, CB_ADDSTRING, 0, (LPARAM)L"Default Device");
    SendMessageW(deviceCombo_, CB_SETCURSEL, 0, 0);

    outDeviceCombo_ = CreateWindowExW(
        0, L"COMBOBOX", nullptr,
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
        0, 0, 200, 300,
        hwnd, (HMENU)IDC_OUT_DEVICE_COMBO, GetModuleHandle(nullptr), nullptr
    );
    SendMessageW(outDeviceCombo_, CB_ADDSTRING, 0, (LPARAM)L"Default Device");
    SendMessageW(outDeviceCombo_, CB_SETCURSEL, 0, 0);

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
    ShowWindow(approveBtn_, SW_SHOW);
    ShowWindow(denyBtn_, SW_SHOW);

    FLASHWINFO fi = { sizeof(fi), hwnd_, FLASHW_ALL | FLASHW_TIMERNOFG, 5, 0 };
    FlashWindowEx(&fi);
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
    updatingDevices_ = true;
    {
        std::lock_guard<std::mutex> lock(dataMutex_);
        audioDevices_ = devices;
        currentDeviceId_ = currentId;
    }

    if (deviceCombo_) {
        SendMessageW(deviceCombo_, CB_RESETCONTENT, 0, 0);
        SendMessageW(deviceCombo_, CB_ADDSTRING, 0, (LPARAM)L"Default Device");

        int selectIndex = 0;
        for (size_t i = 0; i < devices.size(); i++) {
            SendMessageW(deviceCombo_, CB_ADDSTRING, 0, (LPARAM)devices[i].second.c_str());
            if (devices[i].first == currentId) {
                selectIndex = (int)(i + 1);
            }
        }
        SendMessageW(deviceCombo_, CB_SETCURSEL, selectIndex, 0);
    }
    updatingDevices_ = false;
}

void ServerGui::updateOutDevices(const std::vector<std::pair<std::wstring, std::wstring>>& devices,
                               const std::wstring& currentId) {
    updatingOutDevices_ = true;
    {
        std::lock_guard<std::mutex> lock(dataMutex_);
        outAudioDevices_ = devices;
        currentOutDeviceId_ = currentId;
    }

    if (outDeviceCombo_) {
        SendMessageW(outDeviceCombo_, CB_RESETCONTENT, 0, 0);
        SendMessageW(outDeviceCombo_, CB_ADDSTRING, 0, (LPARAM)L"Default Device");

        int selectIndex = 0;
        for (size_t i = 0; i < devices.size(); i++) {
            SendMessageW(outDeviceCombo_, CB_ADDSTRING, 0, (LPARAM)devices[i].second.c_str());
            if (devices[i].first == currentId) {
                selectIndex = (int)(i + 1);
            }
        }
        SendMessageW(outDeviceCombo_, CB_SETCURSEL, selectIndex, 0);
    }
    updatingOutDevices_ = false;
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
        return 1;

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
            if (updatingDevices_) return 0; // Ignore re-entrant notifications
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
            if (updatingOutDevices_) return 0; // Ignore re-entrant notifications
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
            // Reposition controls (connection state may have changed)
            RECT rc;
            GetClientRect(hwnd, &rc);
            onResize(hwnd, rc.right, rc.bottom);
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
    // Reposition child controls based on current layout
    // This must be done here, NOT in onPaint, to avoid control shrinking bugs
    if (width > 0 && height > 0) {
        int margin = 12;
        int pad = 12;
        int cardW = width - margin * 2;
        int y = 48; // after title + separator

        // Status card height
        bool connected;
        {
            std::lock_guard<std::mutex> lock(dataMutex_);
            connected = stats_.connected;
        }
        int statusCardH = connected ? 108 : 64;
        y += statusCardH + 8;

        // Audio devices card
        int devCardY = y;
        int comboX = margin + pad + 56;
        int comboW = width - margin - pad - comboX;
        if (deviceCombo_) MoveWindow(deviceCombo_, comboX, devCardY + 26, comboW, 300, TRUE);
        if (outDeviceCombo_) MoveWindow(outDeviceCombo_, comboX, devCardY + 54, comboW, 300, TRUE);
        y += 90 + 8;

        // Jitter buffer card
        int jitterCardY = y;
        if (jitterSlider_) {
            MoveWindow(jitterSlider_, margin + pad, jitterCardY + 28,
                       cardW - pad * 2, 22, TRUE);
        }
        y += 58 + 8;

        // Pair request card (conditional)
        bool hasPairReq;
        {
            std::lock_guard<std::mutex> lock(dataMutex_);
            hasPairReq = pairRequest_.pending;
        }
        if (hasPairReq) {
            if (approveBtn_) MoveWindow(approveBtn_, margin + pad, y + 58, 100, 26, TRUE);
            if (denyBtn_) MoveWindow(denyBtn_, margin + pad + 108, y + 58, 90, 26, TRUE);
        }
    }
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

    HFONT oldFont = (HFONT)SelectObject(memDC, fontTitle_);

    // Title
    RECT titleRect = {20, 10, w - 20, 36};
    drawColorText(memDC, L"AudioBridge", titleRect, CLR_ACCENT_LIGHT);

    SelectObject(memDC, fontSmall_);
    RECT verRect = {w - 60, 14, w - 16, 30};
    drawColorText(memDC, L"v1.0", verRect, CLR_TEXT_DIM, DT_RIGHT | DT_SINGLELINE);

    // Thin separator
    HBRUSH sepBrush = CreateSolidBrush(CLR_SEPARATOR);
    RECT sepRect = {16, 40, w - 16, 41};
    FillRect(memDC, &sepRect, sepBrush);
    DeleteObject(sepBrush);

    int y = 48;
    int margin = 12;
    int pad = 12;
    int cardW = w - margin * 2;

    // === STATUS CARD ===
    {
        std::lock_guard<std::mutex> lock(dataMutex_);
        bool connected = stats_.connected;
        bool paused = stats_.paused;

        int cardH = connected ? 108 : 64;
        RECT cardRc = {margin, y, w - margin, y + cardH};
        drawRoundRect(memDC, cardRc, 8, cardBrush_, CLR_CARD_BORDER);

        // Status dot + text
        COLORREF statusColor = connected ? (paused ? CLR_YELLOW : CLR_GREEN) : CLR_TEXT_DIM;
        std::wstring statusText = connected
            ? (paused ? L"Paused" : L"Streaming")
            : L"Waiting for connection";

        // Dot
        HBRUSH dotBr = CreateSolidBrush(statusColor);
        HPEN noPen = CreatePen(PS_NULL, 0, 0);
        HPEN prevPen = (HPEN)SelectObject(memDC, noPen);
        HBRUSH prevBr = (HBRUSH)SelectObject(memDC, dotBr);
        Ellipse(memDC, cardRc.left + pad, cardRc.top + 14, cardRc.left + pad + 8, cardRc.top + 22);
        SelectObject(memDC, prevPen);
        SelectObject(memDC, prevBr);
        DeleteObject(dotBr);
        DeleteObject(noPen);

        SelectObject(memDC, fontBold_);
        RECT stRc = {cardRc.left + pad + 14, cardRc.top + 10, cardRc.right - pad, cardRc.top + 26};
        drawColorText(memDC, statusText.c_str(), stRc, statusColor);

        if (connected) {
            // Client info
            SelectObject(memDC, fontMono_);
            std::wstring clientInfo = toWide(stats_.clientName) + L"  \u2022  " + toWide(stats_.clientAddress);
            RECT cRc = {cardRc.left + pad, cardRc.top + 30, cardRc.right - pad, cardRc.top + 44};
            drawColorText(memDC, clientInfo.c_str(), cRc, CLR_TEXT);

            // Audio level bar
            int barY = cardRc.top + 50;
            HBRUSH barBgBr = CreateSolidBrush(CLR_SEPARATOR);
            RECT barBg = {cardRc.left + pad, barY, cardRc.right - pad, barY + 4};
            FillRect(memDC, &barBg, barBgBr);
            DeleteObject(barBgBr);

            float level = stats_.peakLevel;
            if (level > 1.0f) level = 1.0f;
            int barWidth = (int)((cardW - pad * 2) * level);
            if (barWidth > 0) {
                COLORREF barColor = level > 0.9f ? CLR_RED : (level > 0.7f ? CLR_ORANGE : CLR_GREEN);
                HBRUSH barFgBr = CreateSolidBrush(barColor);
                RECT barFg = {cardRc.left + pad, barY, cardRc.left + pad + barWidth, barY + 4};
                FillRect(memDC, &barFg, barFgBr);
                DeleteObject(barFgBr);
            }

            // Stats row 1
            SelectObject(memDC, fontSmall_);
            char s1[256];
            snprintf(s1, sizeof(s1), "%llu pkts   %s   %.0f kbps   seq %u",
                     (unsigned long long)stats_.packetsSent, formatBytes(stats_.bytesSent).c_str(),
                     stats_.kbps, stats_.sequenceNum);
            std::wstring s1w = toWide(s1);
            RECT r1 = {cardRc.left + pad, barY + 10, cardRc.right - pad, barY + 24};
            drawColorText(memDC, s1w.c_str(), r1, CLR_TEXT_DIM);

            // Stats row 2
            char s2[128];
            snprintf(s2, sizeof(s2), "Uptime: %s   Peak: %.3f%s",
                     formatUptime(stats_.uptimeSeconds).c_str(),
                     stats_.peakLevel,
                     stats_.peakLevel > 0.95f ? " HOT" : "");
            std::wstring s2w = toWide(s2);
            RECT r2 = {cardRc.left + pad, barY + 24, cardRc.right - pad, barY + 38};
            drawColorText(memDC, s2w.c_str(), r2, CLR_TEXT_DIM);
        } else {
            // Server info
            SelectObject(memDC, fontSmall_);
            std::wstring info = toWide(stats_.serverName) + L"  \u2022  " + toWide(stats_.serverMac);
            RECT iRc = {cardRc.left + pad, cardRc.top + 32, cardRc.right - pad, cardRc.top + 46};
            drawColorText(memDC, info.c_str(), iRc, CLR_TEXT_DIM);
        }

        y += cardH + 8;
    }

    // === AUDIO DEVICES CARD ===
    {
        int cardH = 90;
        RECT cardRc = {margin, y, w - margin, y + cardH};
        drawRoundRect(memDC, cardRc, 8, cardBrush_, CLR_CARD_BORDER);

        SelectObject(memDC, fontBold_);
        RECT hdrRc = {cardRc.left + pad, cardRc.top + 8, cardRc.right - pad, cardRc.top + 22};
        drawColorText(memDC, L"AUDIO DEVICES", hdrRc, CLR_TEXT_DIM);

        SelectObject(memDC, fontSmall_);
        RECT lbl1 = {cardRc.left + pad, cardRc.top + 30, cardRc.left + pad + 80, cardRc.top + 44};
        drawColorText(memDC, L"Casting:", lbl1, CLR_TEXT_DIM);
        RECT lbl2 = {cardRc.left + pad, cardRc.top + 58, cardRc.left + pad + 80, cardRc.top + 72};
        drawColorText(memDC, L"Receiving:", lbl2, CLR_TEXT_DIM);

        y += cardH + 8;
    }

    // === JITTER BUFFER CARD ===
    {
        int cardH = 58;
        RECT cardRc = {margin, y, w - margin, y + cardH};
        drawRoundRect(memDC, cardRc, 8, cardBrush_, CLR_CARD_BORDER);

        SelectObject(memDC, fontBold_);
        RECT hdrRc = {cardRc.left + pad, cardRc.top + 8, cardRc.right - 60, cardRc.top + 22};
        drawColorText(memDC, L"JITTER BUFFER", hdrRc, CLR_TEXT_DIM);

        // Value
        wchar_t valBuf[16];
        swprintf(valBuf, 16, L"%d ms", jitterBufferMs_);
        RECT valRc = {cardRc.right - 60, cardRc.top + 8, cardRc.right - pad, cardRc.top + 22};
        drawColorText(memDC, valBuf, valRc, CLR_ACCENT_LIGHT, DT_RIGHT | DT_SINGLELINE);

        y += cardH + 8;
    }

    // === PAIR REQUEST CARD (conditional) ===
    bool hasPairReq = false;
    {
        std::lock_guard<std::mutex> lock(dataMutex_);
        hasPairReq = pairRequest_.pending;
    }
    if (hasPairReq) {
        int cardH = 88;
        RECT cardRc = {margin, y, w - margin, y + cardH};
        drawRoundRect(memDC, cardRc, 8, cardBrush_, CLR_YELLOW);

        SelectObject(memDC, fontBold_);
        RECT hdrRc = {cardRc.left + pad, cardRc.top + 8, cardRc.right - pad, cardRc.top + 22};
        drawColorText(memDC, L"\u26A0  PAIRING REQUEST", hdrRc, CLR_YELLOW);

        std::lock_guard<std::mutex> lock(dataMutex_);
        SelectObject(memDC, fontNormal_);
        std::wstring reqText = L"\"" + toWide(pairRequest_.clientName) + L"\" wants to connect";
        RECT reqRc = {cardRc.left + pad, cardRc.top + 26, cardRc.right - pad, cardRc.top + 42};
        drawColorText(memDC, reqText.c_str(), reqRc, CLR_TEXT);

        SelectObject(memDC, fontMono_);
        std::wstring idText = L"ID: " + toWide(pairRequest_.clientId.substr(0, 20));
        RECT idRc = {cardRc.left + pad, cardRc.top + 42, cardRc.right - pad, cardRc.top + 56};
        drawColorText(memDC, idText.c_str(), idRc, CLR_TEXT_DIM);

        y += cardH + 8;
    }

    // === PAIRED DEVICES CARD ===
    {
        std::lock_guard<std::mutex> lock(dataMutex_);
        int peerCount = (int)approvedPeers_.size();
        int contentH = (std::max)(1, peerCount) * 20;
        int cardH = 32 + contentH;
        RECT cardRc = {margin, y, w - margin, y + cardH};
        drawRoundRect(memDC, cardRc, 8, cardBrush_, CLR_CARD_BORDER);

        SelectObject(memDC, fontBold_);
        wchar_t hdr[48];
        swprintf(hdr, 48, L"PAIRED DEVICES  (%d)", peerCount);
        RECT hdrRc = {cardRc.left + pad, cardRc.top + 8, cardRc.right - pad, cardRc.top + 22};
        drawColorText(memDC, hdr, hdrRc, CLR_TEXT_DIM);

        if (approvedPeers_.empty()) {
            SelectObject(memDC, fontSmall_);
            RECT eRc = {cardRc.left + pad, cardRc.top + 26, cardRc.right - pad, cardRc.top + 40};
            drawColorText(memDC, L"No paired devices", eRc, CLR_TEXT_DIM);
        } else {
            int py = cardRc.top + 28;
            for (size_t i = 0; i < approvedPeers_.size(); i++) {
                SelectObject(memDC, fontSmall_);
                std::wstring name = L"\u2022 " + toWide(approvedPeers_[i].second);
                RECT nRc = {cardRc.left + pad, py, cardRc.left + 200, py + 16};
                drawColorText(memDC, name.c_str(), nRc, CLR_TEXT);

                // Truncated ID
                SelectObject(memDC, fontMono_);
                std::string shortId = approvedPeers_[i].first;
                if (shortId.length() > 12) shortId = shortId.substr(0, 12) + "...";
                RECT idRc = {cardRc.left + 200, py + 1, cardRc.right - 100, py + 15};
                drawColorText(memDC, toWide(shortId).c_str(), idRc, CLR_TEXT_DIM);

                // Time ago
                if (i < peerTimestamps_.size()) {
                    int64_t age = std::time(nullptr) - peerTimestamps_[i];
                    int days = (int)(age / 86400);
                    int hours = (int)((age % 86400) / 3600);
                    char ageBuf[32];
                    if (days > 0)
                        snprintf(ageBuf, sizeof(ageBuf), "%dd ago", days);
                    else
                        snprintf(ageBuf, sizeof(ageBuf), "%dh ago", hours);
                    RECT aRc = {cardRc.right - 80, py + 1, cardRc.right - pad, py + 15};
                    drawColorText(memDC, toWide(ageBuf).c_str(), aRc, CLR_TEXT_DIM, DT_RIGHT | DT_SINGLELINE);
                }
                py += 20;
            }
        }

        y += cardH + 8;
    }

    // === LOG CARD (fills remaining space) ===
    {
        int logH = h - y - 8;
        if (logH < 50) logH = 50;
        RECT cardRc = {margin, y, w - margin, y + logH};
        drawRoundRect(memDC, cardRc, 8, cardBrush_, CLR_CARD_BORDER);

        SelectObject(memDC, fontBold_);
        RECT hdrRc = {cardRc.left + pad, cardRc.top + 8, cardRc.right - pad, cardRc.top + 22};
        drawColorText(memDC, L"LOG", hdrRc, CLR_TEXT_DIM);

        std::lock_guard<std::mutex> lock(dataMutex_);
        SelectObject(memDC, fontMono_);
        int ly = cardRc.top + 26;
        int maxLines = (logH - 34) / 14;
        int startIdx = (int)logMessages_.size() - maxLines;
        if (startIdx < 0) startIdx = 0;

        for (int i = startIdx; i < (int)logMessages_.size() && ly < cardRc.bottom - 6; i++) {
            std::wstring logW = toWide(logMessages_[i]);
            RECT lRc = {cardRc.left + pad, ly, cardRc.right - pad, ly + 14};
            drawColorText(memDC, logW.c_str(), lRc, CLR_TEXT_DIM);
            ly += 14;
        }

        if (logMessages_.empty()) {
            SelectObject(memDC, fontSmall_);
            RECT eRc = {cardRc.left + pad, cardRc.top + 28, cardRc.right - pad, cardRc.top + 42};
            drawColorText(memDC, L"No log messages", eRc, CLR_TEXT_DIM);
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
