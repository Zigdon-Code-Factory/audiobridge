#pragma once

namespace gui_html {

inline const char* HTML_CONTENT = R"html(
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
    margin-left: 8px;
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
.btn.frame-btn { background: #2a2a3a; color: var(--text); border: 1px solid #3a3a4a; padding: 6px 16px; }
.btn.frame-btn:hover { background: #3a3a4a; }
.btn.frame-btn.active { background: var(--accent); color: #0f0f14; border-color: var(--accent); }
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
    <span class="version">v1.0</span>
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
        <label>Sending:</label>
        <select id="deviceSelect"></select>
    </div>
    <div class="device-row">
        <label>Receiving:</label>
        <select id="outDeviceSelect"></select>
    </div>
</div>

<!-- Jitter Buffer Card -->
<div class="card">
    <div class="card-header">
        <span>JITTER BUFFER</span>
        <span class="accent" id="jitterValue">10 ms</span>
    </div>
    <input type="range" id="jitterSlider" min="0" max="150" step="5" value="10">
</div>

<!-- Frame Size Card -->
<div class="card">
    <div class="card-header">
        <span>FRAME SIZE</span>
        <span class="accent" id="frameSizeValue">10 ms</span>
    </div>
    <div class="btn-row" style="justify-content:center;gap:8px;margin-top:4px">
        <button class="btn frame-btn" id="frameBtn5" data-ms="5">5 ms</button>
        <button class="btn frame-btn active" id="frameBtn10" data-ms="10">10 ms</button>
        <button class="btn frame-btn" id="frameBtn20" data-ms="20">20 ms</button>
    </div>
    <div class="dim small" style="margin-top:6px;text-align:center">Lower = less latency &nbsp;|&nbsp; Higher = better compression</div>
</div>

<!-- Pair Request Card -->
<div class="card pair-request" id="pairRequestCard" style="display:none">
    <div class="card-header">PAIRING REQUEST</div>
    <div class="pair-name" id="pairRequestName"></div>
    <div class="pair-id" id="pairRequestId"></div>
    <div class="btn-row">
        <button class="btn approve" id="btnApprove">Approve</button>
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
        document.getElementById('frameSizeValue').textContent = ms + ' ms';
        window._setFrameSize(String(ms));
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
        text.textContent = s.paused ? 'Paused' : 'Streaming';
        text.style.color = s.paused ? 'var(--yellow)' : 'var(--green)';
        conn.style.display = '';
        disc.style.display = 'none';

        var clientInfoEl = document.getElementById('clientInfo');
        clientInfoEl.innerHTML = s.clientName + '  \u2022  ' + s.clientAddress +
            '<span class="encryption-badge ' + (s.encrypted ? 'encrypted' : 'unencrypted') + '">' +
            (s.encrypted ? '\u{1F512} DTLS' : '\u{1F513} Open') + '</span>';

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
            s.serverName + '  \u2022  ' + s.serverMac;
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
function showPairRequest(clientId, name) {
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

async function tick() {
    try {
        var result = await window._tick();
        var s = (typeof result === 'string') ? JSON.parse(result) : result;
        updateStatusCard(s);
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
            lastFrameSizeMs = s.frameSizeMs;
            document.querySelectorAll('.frame-btn').forEach(function(b) {
                b.classList.toggle('active', parseInt(b.getAttribute('data-ms')) === s.frameSizeMs);
            });
            document.getElementById('frameSizeValue').textContent = s.frameSizeMs + ' ms';
        }
    } catch(e) {
        console.error('tick error:', e);
    }
}

setInterval(tick, 50);
</script>

</body>
</html>
)html";

} // namespace gui_html
