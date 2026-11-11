#pragma once
#include <string>

namespace gui_html {

inline const std::string HTML_CONTENT = std::string(R"html(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<style>
:root {
    --bg: #0f0f14;
    --card: #181822;
    --card-border: #262637;
    --text: #e1e1f0;
    --text-dim: #787896;
    --accent: #6366f1;
    --accent-light: #818cf8;
    --green: #34d399;
    --red: #f87171;
    --yellow: #fbbf24;
    --orange: #fb923c;
    --separator: #232332;
}
* { margin: 0; padding: 0; box-sizing: border-box; }
body {
    background: var(--bg);
    color: var(--text);
    font-family: 'Segoe UI', system-ui, -apple-system, sans-serif;
    font-size: 15px;
    overflow-y: auto;
    user-select: none;
    -webkit-user-select: none;
}
.header {
    display: flex;
    align-items: baseline;
    justify-content: space-between;
    padding: 20px 28px 12px;
}
.title { font-size: 24px; font-weight: 700; color: var(--accent-light); }
.version { font-size: 13px; color: var(--text-dim); }
.sep { height: 1px; background: var(--separator); margin: 0 24px 12px; }
.card {
    background: var(--card);
    border: 1px solid var(--card-border);
    border-radius: 10px;
    padding: 16px 20px;
    margin: 0 20px 12px;
}
.card-header {
    font-size: 12px;
    font-weight: 600;
    color: var(--text-dim);
    letter-spacing: 0.5px;
    margin-bottom: 10px;
    display: flex;
    justify-content: space-between;
    align-items: center;
}
.card-header .accent { color: var(--accent-light); font-size: 16px; }
.status-row {
    display: flex;
    align-items: center;
    gap: 10px;
}
.status-dot {
    width: 10px; height: 10px;
    border-radius: 50%;
    background: var(--text-dim);
    flex-shrink: 0;
}
.status-dot.green { background: var(--green); }
.status-dot.yellow { background: var(--yellow); }
.status-dot.red { background: var(--red); }
.status-text { font-weight: 600; font-size: 16px; }
.mono { font-family: 'Consolas', 'Courier New', monospace; font-size: 14px; }
.dim { color: var(--text-dim); }
.small { font-size: 13px; }
.level-bar {
    height: 6px;
    background: var(--separator);
    border-radius: 3px;
    overflow: hidden;
}
.level-row {
    display: flex;
    align-items: center;
    gap: 8px;
    margin: 4px 0;
}
.level-label {
    font-size: 11px;
    color: var(--text-dim);
    width: 68px;
    flex-shrink: 0;
}
.level-row .level-bar {
    flex: 1;
}
.levels-container {
    margin: 10px 0;
}
.encryption-badge {
    display: inline-flex;
    align-items: center;
    gap: 4px;
    font-size: 11px;
    font-weight: 600;
    padding: 2px 8px;
    border-radius: 6px;
    margin-left: 6px;
    vertical-align: middle;
}
.encryption-badge.encrypted {
    background: rgba(76, 175, 80, 0.15);
    color: #4caf50;
    border: 1px solid rgba(76, 175, 80, 0.3);
}
.encryption-badge.unencrypted {
    background: rgba(255, 152, 0, 0.15);
    color: #ff9800;
    border: 1px solid rgba(255, 152, 0, 0.3);
}
.encryption-badge.fast {
    background: rgba(99, 102, 241, 0.18);
    color: #a5b4fc;
    border: 1px solid rgba(99, 102, 241, 0.4);
}
.level-fill {
    height: 100%;
    width: 0%;
    border-radius: 3px;
    background: var(--green);
    transition: width 0.15s ease, background-color 0.15s ease;
}
.stats-row {
    font-size: 13px;
    color: var(--text-dim);
    margin-top: 4px;
    font-family: 'Consolas', 'Courier New', monospace;
}
.client-info {
    font-family: 'Consolas', 'Courier New', monospace;
    font-size: 14px;
    margin-top: 8px;
}
.server-info {
    font-size: 14px;
    color: var(--text-dim);
    margin-top: 8px;
}
.device-row {
    display: flex;
    align-items: center;
    gap: 12px;
    margin-bottom: 10px;
}
.device-row:last-child { margin-bottom: 0; }
.device-row label {
    font-size: 14px;
    color: var(--text-dim);
    width: 85px;
    flex-shrink: 0;
}
.device-row select {
    flex: 1;
    background: var(--bg);
    color: var(--text);
    border: 1px solid var(--card-border);
    border-radius: 6px;
    padding: 8px 12px;
    font-size: 14px;
    font-family: inherit;
    outline: none;
    cursor: pointer;
}
.device-row select:focus { border-color: var(--accent); }
.device-row select option { background: var(--bg); color: var(--text); }
input[type=range] {
    -webkit-appearance: none;
    width: 100%;
    height: 8px;
    border-radius: 4px;
    background: var(--separator);
    outline: none;
    margin-top: 6px;
}
input[type=range]::-webkit-slider-thumb {
    -webkit-appearance: none;
    width: 20px; height: 20px;
    border-radius: 50%;
    background: var(--accent);
    cursor: pointer;
    border: none;
}
.pair-request { border-color: var(--yellow); }
.pair-request .card-header { color: var(--yellow); }
.pair-name { margin: 6px 0 4px; font-size: 15px; }
.pair-id { font-family: 'Consolas', monospace; font-size: 13px; color: var(--text-dim); }
.btn-row { display: flex; gap: 12px; margin-top: 12px; }
.btn {
    padding: 8px 24px;
    border: none;
    border-radius: 6px;
    font-size: 14px;
    font-weight: 600;
    cursor: pointer;
    font-family: inherit;
}
.btn.approve { background: var(--green); color: #0f0f14; }
.btn.approve:hover { filter: brightness(1.1); }
.btn.deny { background: var(--red); color: #0f0f14; }
.btn.deny:hover { filter: brightness(1.1); }
.btn.frame-btn, .btn.codec-btn, .btn.voice-btn { background: #2a2a3a; color: var(--text); border: 1px solid #3a3a4a; padding: 6px 14px; }
.btn.frame-btn:hover, .btn.codec-btn:hover, .btn.voice-btn:hover { background: #3a3a4a; }
.btn.frame-btn.active, .btn.codec-btn.active { background: var(--accent); color: #0f0f14; border-color: var(--accent); }
.peer-item {
    display: flex;
    align-items: center;
    padding: 6px 0;
    font-size: 14px;
}
.peer-name { flex: 1; }
.peer-id { font-family: 'Consolas', monospace; font-size: 13px; color: var(--text-dim); margin-right: 12px; }
.peer-age { font-size: 13px; color: var(--text-dim); margin-right: 12px; white-space: nowrap; }
.peer-revoke {
    background: none;
    border: 1px solid var(--card-border);
    color: var(--text-dim);
    border-radius: 4px;
    padding: 4px 10px;
    font-size: 13px;
    cursor: pointer;
    font-family: inherit;
}
.peer-revoke:hover { border-color: var(--red); color: var(--red); }
.log-container {
    font-family: 'Consolas', 'Courier New', monospace;
    font-size: 13px;
    color: var(--text-dim);
    max-height: 300px;
    overflow-y: auto;
    line-height: 1.5;
}
.log-container::-webkit-scrollbar { width: 8px; }
.log-container::-webkit-scrollbar-track { background: transparent; }
.log-container::-webkit-scrollbar-thumb { background: var(--card-border); border-radius: 4px; }
.log-line { white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.hot { color: var(--red); font-weight: 600; }
</style>
</head>
<body>

<div class="header">
    <span class="title">AudioBridge</span>
    <span class="version">v1.1 Low-Latency</span>
</div>
<div class="sep"></div>

<!-- Status Card -->
<div class="card">
    <div class="status-row">
        <span class="status-dot" id="statusDot"></span>
        <span class="status-text" id="statusText">Waiting for connection</span>
    </div>
    <div id="connectedInfo" style="display:none">
        <div class="client-info" id="clientInfo"></div>
        <div class="levels-container">
            <div class="level-row">
                <span class="level-label">Sending</span>
                <div class="level-bar"><div class="level-fill" id="sendLevelFill"></div></div>
            </div>
            <div class="level-row">
                <span class="level-label">Receiving</span>
                <div class="level-bar"><div class="level-fill" id="recvLevelFill"></div></div>
            </div>
        </div>
        <div class="stats-row" id="statsRow1"></div>
        <div class="stats-row" id="statsRow2"></div>
    </div>
    <div id="disconnectedInfo">
        <div class="server-info" id="serverInfo"></div>
    </div>
</div>

<!-- Audio Devices Card -->
<div class="card">
    <div class="card-header">AUDIO DEVICES</div>
    <div class="device-row">
        <label>Mode:</label>
        <select id="captureModeSelect">
            <option value="loopback">Loopback (system audio)</option>
            <option value="recording">Recording device</option>
        </select>
    </div>
    <div class="device-row">
        <label>PC to phone:</label>
        <select id="deviceSelect"></select>
    </div>
    <div class="device-row">
        <label>Phone mic to PC:</label>
        <select id="outDeviceSelect"></select>
    </div>
    <details class="dim small" style="margin-top:10px">
        <summary>Use your phone as a wireless headset</summary>
        <p>Use Loopback and choose the PC playback device your games and calls use.
        Enable the microphone in the phone app. Set Phone mic to PC to
        <b>CABLE Input</b>, then select <b>CABLE Output</b> as the microphone in Discord/Zoom.
        One VB-CABLE is enough: PC playback travels over Wi-Fi.</p>
        <p>Keep CABLE Input dedicated to the phone microphone. Do not select it as the
        sending device or route it back into itself in Voicemeeter. If using Voicemeeter,
        send PC/game/call playback to a separate bus from the phone microphone.</p>
        <p>AudioBridge Microphone is an alternative only when its driver and service are
        installed and working. Extra cables are needed only for additional independent mixes.</p>
    </details>
</div>

<div class="card" id="voiceCard">
    <div class="card-header">PHONE MICROPHONE EFFECTS</div>
    <div class="device-row">
        <label><input id="voiceNoise" type="checkbox" checked> Noise removal</label>
        <select id="voiceStrength" aria-label="Noise removal strength">
            <option value="6">6 dB</option><option value="10">10 dB</option>
            <option value="15">15 dB</option><option value="20">20 dB</option>
            <option value="30">30 dB</option><option value="100" selected>Max</option>
        </select>
    </div>
    <div class="device-row"><label><input id="voiceGate" type="checkbox" checked> Voice gate</label></div>
    <details>
)html") + std::string(R"html(        <summary class="dim small">Tune voice gate and rumble</summary>
        <div class="device-row"><label for="voiceThreshold">Threshold</label>
            <input id="voiceThreshold" type="range" min="10" max="90" value="50">
            <span id="voiceThresholdValue">50%</span></div>
        <div class="device-row"><label for="voiceHold">Hold</label>
            <input id="voiceHold" type="range" min="100" max="1500" step="100" value="300">
            <span id="voiceHoldValue">0.3 s</span></div>
        <label class="dim small"><input id="voiceLowCut" type="checkbox"> Reduce low rumble (80 Hz)</label>
    </details>
    <div class="dim small" id="voiceStatus" role="status" style="margin-top:8px">Loading voice models...</div>
    <div class="sep"></div>
    <div class="device-row">
        <button class="btn voice-btn" id="voiceRecord">Record</button>
        <span class="dim small" id="voiceTime">Up to 10 seconds from the phone mic</span>
        <select id="voiceSource" aria-label="Recording version">
            <option value="1">Processed</option><option value="0">Original</option>
        </select>
    </div>
    <audio id="voiceAudio" controls preload="metadata" style="width:100%;margin-top:8px"
        aria-label="Phone microphone test playback"></audio>
    <div class="dim small" id="voicePreviewStatus" role="status">Connect your phone, enable its mic, then record and compare.</div>
</div>

<!-- Streaming Codec Card -->
<div class="card">
    <div class="card-header">
        <span>STREAMING CODEC</span>
        <span class="accent" id="codecValue">Raw PCM (0ms)</span>
    </div>
    <div class="btn-row" style="justify-content:center;gap:8px;margin-top:4px">
        <button class="btn codec-btn active" id="codecBtnPcm" data-codec="pcm">Raw PCM (0ms delay)</button>
        <button class="btn codec-btn" id="codecBtnOpus" data-codec="opus">Opus (128 kbps)</button>
    </div>
    <div class="dim small" style="margin-top:6px;text-align:center">Raw PCM bypasses Opus's 2.5ms lookahead (~1.5 Mbps over LAN Wi-Fi)</div>
</div>

<!-- Jitter Buffer Card -->
<div class="card">
    <div class="card-header">
        <span>JITTER BUFFER</span>
        <span class="accent" id="jitterValue">10 ms</span>
    </div>
    <input type="range" id="jitterSlider" min="0" max="100" step="1" value="10">
</div>

<!-- Frame Size Card -->
<div class="card">
    <div class="card-header">
        <span>FRAME SIZE</span>
        <span class="accent" id="frameSizeValue">2.5 ms</span>
    </div>
    <div class="btn-row" style="justify-content:center;gap:8px;margin-top:4px">
        <button class="btn frame-btn active" id="frameBtn2" data-ms="2">2.5 ms</button>
        <button class="btn frame-btn" id="frameBtn5" data-ms="5">5 ms</button>
        <button class="btn frame-btn" id="frameBtn10" data-ms="10">10 ms</button>
        <button class="btn frame-btn" id="frameBtn20" data-ms="20">20 ms</button>
    </div>
    <div class="dim small" style="margin-top:6px;text-align:center">2.5 ms (120 samples) = fastest streaming response</div>
</div>

<!-- Pair Request Card -->
<div class="card pair-request" id="pairRequestCard" style="display:none">
    <div class="card-header">PAIRING REQUEST</div>
    <div class="pair-name" id="pairRequestName"></div>
    <div class="pair-id" id="pairRequestId"></div>
    <div id="pairCode" style="font-size:28px;letter-spacing:6px;margin:12px 0"></div>
    <p>Compare this code with your phone. Approve only if both codes match.</p>
    <div class="btn-row">
        <button class="btn approve" id="btnApprove">Codes match — approve</button>
        <button class="btn deny" id="btnDeny">Deny</button>
    </div>
</div>

<!-- Paired Devices Card -->
<div class="card">
    <div class="card-header">PAIRED DEVICES <span class="dim" id="peerCount">(0)</span></div>
    <div id="peerList"><div class="dim small">No paired devices</div></div>
</div>

<!-- Log Card -->
<div class="card">
    <div class="card-header">LOG</div>
    <div class="log-container" id="logContainer">
        <div class="dim">Waiting for events...</div>
    </div>
</div>
)html"
R"html(<script>
let lastLogCount = 0;
let lastPeerJson = '';
let currentPairId = '';
let currentDeviceIndex = -1;
let currentOutDeviceIndex = -1;

function frameLabel(ms) {
    return (ms === 2) ? '2.5 ms' : (ms + ' ms');
}

// Event listeners
document.getElementById('jitterSlider').addEventListener('input', function() {
    document.getElementById('jitterValue').textContent = this.value + ' ms';
    window._setJitter(this.value);
});

document.querySelectorAll('.frame-btn').forEach(function(btn) {
    btn.addEventListener('click', function() {
        var ms = parseInt(this.getAttribute('data-ms'));
        document.querySelectorAll('.frame-btn').forEach(function(b) { b.classList.remove('active'); });
        this.classList.add('active');
        document.getElementById('frameSizeValue').textContent = frameLabel(ms);
        window._setFrameSize(String(ms));
    });
});

document.querySelectorAll('.codec-btn').forEach(function(btn) {
    btn.addEventListener('click', function() {
        var codec = this.getAttribute('data-codec');
        document.querySelectorAll('.codec-btn').forEach(function(b) { b.classList.remove('active'); });
        this.classList.add('active');
        document.getElementById('codecValue').textContent = (codec === 'pcm') ? 'Raw PCM (0ms)' : 'Opus';
        window._setCodec(codec);
    });
});

document.getElementById('captureModeSelect').addEventListener('change', function() {
    window._setCaptureMode(this.value);
});

document.getElementById('deviceSelect').addEventListener('change', function() {
    window._selectDevice(String(this.selectedIndex));
});

document.getElementById('outDeviceSelect').addEventListener('change', function() {
    window._selectOutDevice(String(this.selectedIndex));
});

document.getElementById('btnApprove').addEventListener('click', function() {
    if (currentPairId) window._approvePair(currentPairId);
});

document.getElementById('btnDeny').addEventListener('click', function() {
    if (currentPairId) window._denyPair(currentPairId);
});

function formatBytes(bytes) {
    if (bytes < 1024) return bytes + ' B';
    if (bytes < 1024 * 1024) return (bytes / 1024).toFixed(1) + ' KB';
    return (bytes / (1024 * 1024)).toFixed(2) + ' MB';
}

function formatUptime(seconds) {
    var h = Math.floor(seconds / 3600);
    var m = Math.floor((seconds % 3600) / 60);
    var s = Math.floor(seconds % 60);
    if (h > 0) return h + 'h ' + String(m).padStart(2,'0') + 'm ' + String(s).padStart(2,'0') + 's';
    if (m > 0) return m + 'm ' + String(s).padStart(2,'0') + 's';
    return s + 's';
}

function formatAge(timestamp) {
    if (!timestamp) return '';
    var age = Math.floor(Date.now() / 1000) - timestamp;
    var days = Math.floor(age / 86400);
    var hours = Math.floor((age % 86400) / 3600);
    if (days > 0) return days + 'd ago';
    return hours + 'h ago';
}

function updateStatusCard(s) {
    var dot = document.getElementById('statusDot');
    var text = document.getElementById('statusText');
    var conn = document.getElementById('connectedInfo');
    var disc = document.getElementById('disconnectedInfo');

    if (s.connected) {
        dot.className = 'status-dot ' + (s.paused ? 'yellow' : 'green');
        text.textContent = s.paused ? 'Paused' : ('Streaming to ' + (s.clientCount || 1) + ((s.clientCount || 1) === 1 ? ' phone' : ' phones'));
        text.style.color = s.paused ? 'var(--yellow)' : 'var(--green)';
        conn.style.display = '';
        disc.style.display = 'none';

        var badges = '<span class="encryption-badge ' + (s.encrypted ? 'encrypted' : 'unencrypted') + '">' +
            (s.encrypted ? '\u{1F512} DTLS' : '\u{1F513} Open') + '</span>' +
            '<span class="encryption-badge fast">' + (s.rawPcm ? 'PCM 0ms' : 'Opus') + '</span>';
        if (s.voiceQos) {
            badges += '<span class="encryption-badge encrypted">Wi-Fi AC_VO</span>';
        }
        if (s.kernelTap) {
            badges += '<span class="encryption-badge encrypted">Kernel Tap</span>';
        }

        var clientInfoEl = document.getElementById('clientInfo');
        clientInfoEl.innerHTML = escapeHtml(s.clientName) + '  \u2022  ' + escapeHtml(s.clientAddress) + badges + '<br><small>Microphone return: ' + escapeHtml(s.micOwner || 'None') + '</small>';

        var sendLevel = Math.min(s.peakLevel, 1.0);
        var sendFill = document.getElementById('sendLevelFill');
        sendFill.style.width = (sendLevel * 100) + '%';
        sendFill.style.background = sendLevel > 0.9 ? 'var(--red)' : (sendLevel > 0.7 ? 'var(--orange)' : 'var(--green)');

        var recvLevel = Math.min(s.micPeakLevel, 1.0);
        var recvFill = document.getElementById('recvLevelFill');
        recvFill.style.width = (recvLevel * 100) + '%';
        recvFill.style.background = recvLevel > 0.9 ? 'var(--red)' : (recvLevel > 0.7 ? 'var(--orange)' : 'var(--green)');

        document.getElementById('statsRow1').textContent =
            s.packetsSent + ' pkts   ' + formatBytes(s.bytesSent) + '   ' +
            s.kbps.toFixed(0) + ' kbps   seq ' + s.sequenceNum;

        var uptimeStr = 'Uptime: ' + formatUptime(s.uptimeSeconds) +
            '   Peak: ' + s.peakLevel.toFixed(3);
        if (s.peakLevel > 0.95) uptimeStr += ' HOT';
        if (s.clientRttMs > 0) uptimeStr += '   RTT: ' + s.clientRttMs.toFixed(1) + ' ms';
        var row2 = document.getElementById('statsRow2');
        row2.textContent = uptimeStr;
        row2.className = 'stats-row' + (s.peakLevel > 0.95 ? ' hot' : '');
    } else {
        dot.className = 'status-dot';
        text.textContent = 'Waiting for connection';
        text.style.color = '';
        conn.style.display = 'none';
        disc.style.display = '';
        document.getElementById('serverInfo').textContent =
)html") + std::string(R"html(            s.serverName + '  \u2022  ' + s.serverMac + (s.kernelTap ? '  \u2022  Kernel Tap Active' : '');
    }
}

function updatePeers(peers) {
    // Only rebuild DOM when peer data actually changes
    var peerJson = JSON.stringify(peers);
    if (peerJson === lastPeerJson) return;
    lastPeerJson = peerJson;

    var container = document.getElementById('peerList');
    document.getElementById('peerCount').textContent = '(' + peers.length + ')';

    if (!peers.length) {
        container.innerHTML = '<div class="dim small">No paired devices</div>';
        return;
    }

    container.innerHTML = '';
    for (var i = 0; i < peers.length; i++) {
        (function(peerId) {
            var p = peers[i];
            var shortId = p.id.length > 12 ? p.id.substring(0, 12) + '...' : p.id;
            var row = document.createElement('div');
            row.className = 'peer-item';
            row.innerHTML =
                '<span class="peer-name">\u2022 ' + escapeHtml(p.name) + '</span>' +
                '<span class="peer-id">' + escapeHtml(shortId) + '</span>' +
                '<span class="peer-age">' + formatAge(p.lastConnected) + '</span>';
            var btn = document.createElement('button');
            btn.className = 'peer-revoke';
            btn.textContent = '\u2717';
            btn.addEventListener('click', function() {
                window._revokePeer(peerId);
            });
            row.appendChild(btn);
            container.appendChild(row);
        })(peers[i].id);
    }
}

function updateLogs(logs) {
    if (logs.length === lastLogCount) return;
    lastLogCount = logs.length;

    var container = document.getElementById('logContainer');
    var html = '';
    for (var i = 0; i < logs.length; i++) {
        html += '<div class="log-line">' + escapeHtml(logs[i]) + '</div>';
    }
    container.innerHTML = html || '<div class="dim">Waiting for events...</div>';
    container.scrollTop = container.scrollHeight;
}

function updateDeviceList(devices, currentIndex, changed) {
    var sel = document.getElementById('deviceSelect');
    if (!changed && currentDeviceIndex === currentIndex && sel.options.length === devices.length + 1) return;
    currentDeviceIndex = currentIndex;
    sel.innerHTML = '<option>Default Device</option>';
    for (var i = 0; i < devices.length; i++) {
        var opt = document.createElement('option');
        opt.textContent = devices[i];
        sel.appendChild(opt);
    }
    sel.selectedIndex = currentIndex;
}

function updateOutDeviceList(outDevices, currentIndex, changed) {
    var sel = document.getElementById('outDeviceSelect');
    if (!changed && currentOutDeviceIndex === currentIndex && sel.options.length === outDevices.length + 1) return;
    currentOutDeviceIndex = currentIndex;
    sel.innerHTML = '<option>Default Device</option>';
    for (var i = 0; i < outDevices.length; i++) {
        var opt = document.createElement('option');
        opt.textContent = outDevices[i];
        sel.appendChild(opt);
    }
    sel.selectedIndex = currentIndex;
}

function updatePairRequest(pr) {
    var card = document.getElementById('pairRequestCard');
    if (pr.pending) {
        currentPairId = pr.clientId;
        document.getElementById('pairCode').textContent = pr.code;
        document.getElementById('pairRequestName').textContent =
            '"' + pr.clientName + '" wants to connect';
        document.getElementById('pairRequestId').textContent =
            'ID: ' + (pr.clientId.length > 20 ? pr.clientId.substring(0, 20) + '...' : pr.clientId);
        card.style.display = '';
    } else {
        card.style.display = 'none';
        currentPairId = '';
    }
}

// Called from C++ via eval for immediate pair request notification
function showPairRequest(clientId, name, code) {
    document.getElementById('pairCode').textContent = code;
    currentPairId = clientId;
    document.getElementById('pairRequestName').textContent =
        '"' + name + '" wants to connect';
    document.getElementById('pairRequestId').textContent =
        'ID: ' + (clientId.length > 20 ? clientId.substring(0, 20) + '...' : clientId);
    document.getElementById('pairRequestCard').style.display = '';
}

function revokePeer(clientId) {
    window._revokePeer(clientId);
}

function escapeHtml(s) {
    var d = document.createElement('div');
    d.textContent = s;
    return d.innerHTML;
}

// Periodic tick
var lastJitterMs = -1;
var lastFrameSizeMs = -1;
var lastRawPcm = null;

var voiceInitialized = false, voiceRecording = false, voiceRevision = -1;
var voiceRequest = 0, voiceRenderTimer;
function voiceLabels() {
    document.getElementById('voiceThresholdValue').textContent = document.getElementById('voiceThreshold').value + '%';
    document.getElementById('voiceHoldValue').textContent = (document.getElementById('voiceHold').value / 1000).toFixed(1) + ' s';
    document.getElementById('voiceStrength').disabled = !document.getElementById('voiceNoise').checked;
    document.getElementById('voiceThreshold').disabled = !document.getElementById('voiceGate').checked;
    document.getElementById('voiceHold').disabled = !document.getElementById('voiceGate').checked;
}
function voiceChanged() {
    voiceLabels();
    clearTimeout(voiceRenderTimer);
    voiceRenderTimer = setTimeout(function() {
        var val = [document.getElementById('voiceNoise').checked ? 1 : 0,
            document.getElementById('voiceStrength').value, document.getElementById('voiceGate').checked ? 1 : 0,
            document.getElementById('voiceThreshold').value, document.getElementById('voiceHold').value,
            document.getElementById('voiceLowCut').checked ? 1 : 0].join(' ');
        window._voiceSettings(val).catch(console.error);
    }, 250);
}
['voiceNoise','voiceStrength','voiceGate','voiceThreshold','voiceHold','voiceLowCut'].forEach(function(id) {
    document.getElementById(id).addEventListener('input', voiceChanged);
});
async function voiceLoadClip() {
    var request = ++voiceRequest;
    var audio = document.getElementById('voiceAudio');
    var position = audio.currentTime || 0, playing = !audio.paused;
    var result = await window._voiceClip(Number(document.getElementById('voiceSource').value));
    if (request !== voiceRequest) return;
    var url = typeof result === 'string' && result.startsWith('data:') ? result :
        (typeof result === 'string' ? JSON.parse(result) : result);
    if (!url) { audio.pause(); audio.removeAttribute('src'); audio.load(); return; }
    audio.src = url;
    audio.onloadedmetadata = function() {
        audio.currentTime = Math.min(position, audio.duration || 0);
        if (playing) audio.play().catch(console.error);
    };
    audio.load();
}
document.getElementById('voiceSource').addEventListener('change', function() { voiceLoadClip().catch(console.error); });
document.getElementById('voiceRecord').addEventListener('click', async function() {
    var button = this;
    button.disabled = true;
    try {
        document.getElementById('voiceAudio').pause();
        await window._voiceRecord(voiceRecording ? 0 : 1);
    } finally { button.disabled = false; }
});
function updateVoice(s) {
    var v = s.voice;
    if (!v || !v.settings) return;
    if (!voiceInitialized) {
        var fields = v.settings.split(' ').map(Number);
        document.getElementById('voiceNoise').checked = !!fields[0];
        document.getElementById('voiceStrength').value = fields[1];
        document.getElementById('voiceGate').checked = !!fields[2];
        document.getElementById('voiceThreshold').value = fields[3];
        document.getElementById('voiceHold').value = fields[4];
        document.getElementById('voiceLowCut').checked = !!fields[5];
        voiceInitialized = true; voiceLabels();
    }
    voiceRecording = v.recording;
    document.getElementById('voiceAudio').controls = !v.recording;
    if (v.recording) document.getElementById('voiceAudio').pause();
    document.getElementById('voiceRecord').textContent = v.recording ? 'Stop' : 'Record';
    document.getElementById('voiceRecord').disabled = !v.recording && !s.connected;
    document.getElementById('voiceSource').disabled = v.recording;
    document.getElementById('voiceTime').textContent = v.duration.toFixed(1) + ' / 10 s';
    document.getElementById('voiceStatus').textContent = v.error ? 'Effects unavailable; phone mic passes through. ' + v.error :
        (v.ready ? 'DeepFilterNet + Silero. Noise removal adds 32 ms model delay plus buffering.' : 'Loading voice models...');
    document.getElementById('voicePreviewStatus').textContent = v.previewError ? 'Could not process recording: ' + v.previewError :
        (v.recording ? 'Recording phone mic. Speak normally with your usual background noise.' :
        (v.rendering ? 'Applying effects...' : (v.duration > 0 ? 'Compare Original / Processed. Plays on your PC default output.' :
        'Connect your phone, enable its mic, then record and compare.')));
    if (voiceRevision !== v.revision && !v.recording) {
        voiceRevision = v.revision;
        voiceLoadClip().catch(console.error);
    }
}
async function tick() {
    try {
        var result = await window._tick();
        var s = (typeof result === 'string') ? JSON.parse(result) : result;
        updateStatusCard(s);
        updateVoice(s);
        updatePairRequest(s.pairRequest);
        updatePeers(s.peers);
        updateLogs(s.logs);
        updateDeviceList(s.devices || [], s.currentDeviceIndex || 0, s.devicesChanged);
        updateOutDeviceList(s.outDevices || [], s.currentOutDeviceIndex || 0, s.outDevicesChanged);
        if (s.jitterBufferMs !== undefined && s.jitterBufferMs !== lastJitterMs) {
            lastJitterMs = s.jitterBufferMs;
            var slider = document.getElementById('jitterSlider');
            if (slider && document.activeElement !== slider) {
                slider.value = s.jitterBufferMs;
                document.getElementById('jitterValue').textContent = s.jitterBufferMs + ' ms';
            }
        }
        if (s.frameSizeMs !== undefined && s.frameSizeMs !== lastFrameSizeMs) {
)html") + std::string(R"html(            lastFrameSizeMs = s.frameSizeMs;
            document.querySelectorAll('.frame-btn').forEach(function(b) {
                b.classList.toggle('active', parseInt(b.getAttribute('data-ms')) === s.frameSizeMs);
            });
            document.getElementById('frameSizeValue').textContent = frameLabel(s.frameSizeMs);
        }
        if (s.rawPcm !== undefined && s.rawPcm !== lastRawPcm) {
            lastRawPcm = s.rawPcm;
            document.querySelectorAll('.codec-btn').forEach(function(b) {
                var isPcmBtn = b.getAttribute('data-codec') === 'pcm';
                b.classList.toggle('active', isPcmBtn === s.rawPcm);
            });
            document.getElementById('codecValue').textContent = s.rawPcm ? 'Raw PCM (0ms)' : 'Opus';
        }
    } catch(e) {
        console.error('tick error:', e);
    }
}

setInterval(tick, 50);
</script>

</body>
</html>
)html");

} // namespace gui_html
