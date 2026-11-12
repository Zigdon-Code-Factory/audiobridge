#pragma once
#include <string>

namespace gui_html {

inline const std::string HTML_CONTENT =
std::string(R"html(<!doctype html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>AudioBridge — Audio workspace</title>
<style>
:root { color-scheme: dark; --bg:#111615; --card:#1a211f; --raised:#222b28; --border:#34413b; --text:#eef2eb; --dim:#a6b4ac; --accent:#b9e7cb; --green:#b9e7cb; --red:#ffaca4; --yellow:#f0d28e; --orange:#f0b48b; --mono:'Cascadia Code','Consolas',monospace; }
* { box-sizing:border-box; }
body { margin:0; background:var(--bg); color:var(--text); font:14px/1.5 'Segoe UI',system-ui,sans-serif; }
button,input,select { font:inherit; }
button,select,summary { cursor:pointer; }
button,select,input { accent-color:var(--accent); }
button { color:inherit; }
button:disabled,select:disabled,input:disabled { cursor:not-allowed; opacity:.48; }
:focus-visible { outline:2px solid var(--accent); outline-offset:4px; }
[hidden] { display:none !important; }
h1,h2,h3,p { margin:0; }
h1 { font-size:30px; font-weight:600; letter-spacing:-1px; line-height:1.2; }
h2 { font-size:20px; letter-spacing:-.5px; font-weight:600; }
h3 { font-size:16px; font-weight:600; }
svg.icon { width:22px; height:22px; fill:none; stroke:currentColor; stroke-width:1.65; stroke-linecap:round; stroke-linejoin:round; flex-shrink:0; }
.symbols { position:absolute; width:0; height:0; overflow:hidden; }
.app-header { height:62px; padding:0 32px; display:flex; align-items:center; justify-content:space-between; border-bottom:1px solid var(--border); gap:16px; }
.brand { display:flex; align-items:center; gap:12px; font-size:18px; font-weight:650; letter-spacing:-.5px; }
.brand-mark { display:grid; place-items:center; width:34px; height:34px; color:var(--accent); background:#293e33; border-radius:12px; }
.app-label,.eyebrow { text-transform:uppercase; letter-spacing:1.6px; font-size:12px; font-weight:650; color:var(--dim); }
.header-note { display:flex; align-items:center; gap:8px; color:var(--dim); font-size:12px; }
.workspace { max-width:1240px; margin:auto; padding:26px 32px 24px; }
.workspace-heading { display:flex; align-items:center; justify-content:space-between; gap:16px; margin-bottom:18px; }
.subtitle { color:var(--dim); margin-top:5px; }
.status-pill { display:flex; align-items:center; gap:9px; border:1px solid var(--border); background:var(--card); padding:9px 13px; border-radius:24px; white-space:nowrap; font-size:12px; }
.status-dot { width:7px; height:7px; border-radius:50%; background:var(--dim); flex-shrink:0; }
.status-dot.green { background:var(--green); box-shadow:0 0 0 4px #b9e7cb12; }
.status-dot.yellow { background:var(--yellow); }
.session { border:1px solid var(--border); border-radius:14px; background:#18201c; margin-bottom:22px; }
.session-connected { display:grid; grid-template-columns:1.35fr 1fr .7fr; gap:22px; padding:16px 22px; align-items:center; }
.session-identity { min-width:0; }
.session-name { display:flex; align-items:center; gap:12px; }
.session-name .icon { color:var(--accent); }
#clientInfo { font-size:16px; font-weight:600; overflow-wrap:anywhere; }
.session-meta { display:flex; flex-wrap:wrap; gap:5px 12px; margin-top:5px; font-size:12px; color:var(--dim); }
.encryption-badge { font-size:12px; letter-spacing:.4px; font-weight:600; color:var(--accent); }
.encryption-badge.unencrypted { color:var(--yellow); }
.level-row { display:grid; grid-template-columns:58px 1fr; align-items:center; gap:12px; margin:5px 0; }
.level-label { font-size:12px; color:var(--dim); }
.level-bar { height:5px; border-radius:4px; background:#34433a; overflow:hidden; }
.level-fill { height:100%; width:0; background:var(--accent); border-radius:4px; transition:width .1s; }
.session-metrics { display:flex; justify-content:flex-end; gap:26px; }
.metric-value { font:16px/1.3 var(--mono); white-space:nowrap; }
.metric-label { display:block; font-size:12px; color:var(--dim); margin-top:4px; }
.session-empty { display:flex; gap:16px; align-items:center; padding:19px 22px; }
.empty-icon { display:grid; place-items:center; width:44px; height:44px; flex-shrink:0; border:1px dashed #53685b; border-radius:12px; color:var(--accent); }
.session-empty h2 { font-size:15px; letter-spacing:0; }
.session-empty p { font-size:13px; color:var(--dim); margin-top:3px; }
.server-info { color:var(--accent); font:12px var(--mono); margin-top:6px; overflow-wrap:anywhere; }
.columns { display:grid; grid-template-columns:minmax(280px,.82fr) minmax(0,1.18fr); gap:20px; align-items:start; }
.routing-column { display:grid; gap:14px; }
.panel { border:1px solid var(--border); border-radius:14px; background:var(--card); overflow:hidden; }
.panel-body { padding:22px; }
.panel-title { display:flex; align-items:center; justify-content:space-between; gap:12px; margin-bottom:19px; }
.panel-title .eyebrow { margin-bottom:4px; }
.panel-title > .icon { color:var(--dim); }
.route { position:relative; padding-left:36px; }
.route + .route { margin-top:24px; }
.route-icon { position:absolute; left:0; top:0; color:var(--accent); }
.route-direction { font-size:12px; color:var(--dim); margin-bottom:3px; }
.route label { display:block; font-size:14px; font-weight:600; margin-bottom:9px; }
select { width:100%; min-width:0; min-height:40px; border:1px solid #46564d; border-radius:7px; background:#141b17; color:var(--text); padding:8px 28px 8px 12px; text-overflow:ellipsis; }
.route-mode { margin-top:12px; display:flex; align-items:center; gap:12px; }
.route-mode label { margin:0; white-space:nowrap; color:var(--dim); font-size:12px; font-weight:400; }
.route-mode select { font-size:12px; min-height:34px; padding-top:5px; padding-bottom:5px; }
.route-note { color:var(--dim); font-size:13px; margin-top:7px; }
.details { border-top:1px solid var(--border); }
summary { min-height:48px; padding:13px 22px; display:flex; align-items:center; justify-content:space-between; gap:12px; list-style:none; font-size:12px; font-weight:600; }
summary::-webkit-details-marker { display:none; }
summary::after { content:'+'; color:var(--dim); font:18px/1 var(--mono); }
details[open] > summary::after { content:'−'; }
.summary-info { margin-left:auto; color:var(--dim); font-size:12px; font-weight:400; }
.details-content { padding:0 22px 20px; }
.help-copy { color:var(--dim); font-size:13px; }
.help-copy ol { margin:0; padding-left:18px; }
.help-copy li + li { margin-top:8px; }
.help-copy p { margin-top:12px; }
.help-copy strong { color:var(--text); font-weight:500; }
.control-group + .control-group { margin-top:19px; }
.control-heading { display:flex; align-items:center; justify-content:space-between; margin-bottom:9px; gap:12px; font-size:12px; }
.control-heading label { color:var(--text); }
.control-value { color:var(--accent); font:12px var(--mono); }
.segmented { display:flex; border:1px solid #46564d; border-radius:8px; padding:3px; gap:3px; background:#141b17; }
.segmented .btn { flex:1; min-width:0; border:0; background:transparent; padding:7px 5px; min-height:34px; color:var(--dim); font-size:12px; border-radius:5px; }
.segmented .btn.active { background:#3b5144; color:#e0f6e6; }
input[type=range] { width:100%; height:22px; margin:0; background:transparent; }
.range-caption { display:flex; justify-content:space-between; font-size:12px; color:var(--dim); }
.control-note { font-size:13px; color:var(--dim); margin-top:7px; }
.effect { display:flex; align-items:center; justify-content:space-between; gap:20px; padding:17px 0; border-top:1px solid var(--border); }
.panel-title + .effect, .effects-unavailable[hidden] + .effect { border-top:0; padding-top:0; }
.effect-name { display:block; font-size:14px; font-weight:600; cursor:pointer; }
.effect-description { font-size:13px; color:var(--dim); margin-top:3px; }
.switch { appearance:none; -webkit-appearance:none; position:relative; display:block; width:40px; height:23px; min-width:40px; border:1px solid #68796f; border-radius:20px; background:#344039; margin:0; cursor:pointer; transition:background .15s; }
.switch::before { content:''; position:absolute; width:15px; height:15px; border-radius:50%; background:#c3cec7; top:3px; left:3px; transition:transform .15s; }
.switch:checked { background:var(--accent); border-color:var(--accent); }
.switch:checked::before { background:#20392b; transform:translateX(17px); }
.strength-row { display:flex; align-items:center; justify-content:space-between; gap:18px; padding:0 0 17px; }
.strength-row label { color:var(--dim); font-size:12px; }
.strength-row select { width:160px; min-height:34px; font-size:12px; padding-top:5px; padding-bottom:5px; }
.tuning { background:#171e1a; border:1px solid var(--border); border-radius:9px; margin-bottom:15px; }
.tuning summary { padding:12px 14px; min-height:42px; }
.tuning .details-content { padding:4px 14px 15px; }
.tuning-grid { display:grid; grid-template-columns:1fr 1fr; gap:18px; }
.lowcut-row { display:flex; gap:9px; align-items:center; margin-top:17px; font-size:12px; }
.lowcut-row input { width:16px; height:16px; margin:0; }
.processor-status { font-size:13px; color:var(--dim); min-height:17px; overflow-wrap:anywhere; }
.processor-status[data-error=true] { color:var(--red); }
.preview { border-top:1px solid var(--border); background:#171e1a; padding:20px 22px; }
.preview-heading { display:flex; align-items:center; justify-content:space-between; gap:12px; margin-bottom:14px; }
.preview-heading span { color:var(--dim); font:12px var(--mono); }
.preview-actions { display:flex; justify-content:space-between; align-items:center; gap:14px; }
.btn { min-height:40px; border:1px solid #506357; border-radius:7px; padding:8px 14px; font-weight:600; background:#26342b; }
.btn:hover:not(:disabled) { background:#354a3c; border-color:#819f8c; }
.btn.primary { color:#173022; border-color:var(--accent); background:var(--accent); }
.btn.primary:hover:not(:disabled) { background:#d0f0dc; }
.btn.voice-btn { display:flex; align-items:center; gap:9px; }
.btn.voice-btn::before { content:''; width:8px; height:8px; border-radius:50%; background:currentColor; flex-shrink:0; }
.btn.voice-btn[aria-pressed=true], .btn.voice-btn[aria-pressed=true]:hover:not(:disabled) { color:#2f1915; background:#ffb9ad; border-color:#ffb9ad; }
.btn.voice-btn[aria-pressed=true]::before { border-radius:2px; }
.sample-version { display:flex; align-items:center; gap:8px; color:var(--dim); font-size:12px; }
.sample-version select { width:120px; font-size:12px; }
audio { width:100%; height:38px; margin-top:14px; }
.preview-status { color:var(--dim); font-size:13px; margin-top:12px; min-height:36px; }
.preview-status[data-error=true] { color:var(--red); }
.peer-count { font:12px var(--mono); margin-left:7px; color:var(--dim); }
.peer-item { display:flex; gap:12px; align-items:center; padding:12px 0; border-top:1px solid var(--border); flex-wrap:wrap; }
.peer-item:first-child { border:0; }
.peer-name { flex:1; min-width:80px; font-size:12px; overflow-wrap:anywhere; }
.peer-id,.peer-age { font-size:12px; color:var(--dim); }
.peer-role)html") +
std::string(R"html( { width:125px; font-size:12px; }
.peer-role-status { width:100%; font-size:12px; color:var(--dim); }
.peer-revoke { min-height)html") +
std::string(R"html(:34px; border:1px solid #506357; border-radius:6px; background:transparent; color:var(--dim); font-size:12px; padding:5px 9px; }
.peer-revoke:hover { color:var(--red); border-color:var(--red); }
.diagnostics { margin-top:20px; border-top:1px solid var(--border); }
.diagnostics > summary { padding:15px 0; }
.diagnostics .details-content { padding:0 0 12px; }
.stats-row { font:12px/1.8 var(--mono); color:var(--dim); overflow-wrap:anywhere; }
.log-container { margin-top:12px; max-height:160px; overflow:auto; padding:12px; border:1px solid var(--border); border-radius:8px; background:#0e1310; font:12px/1.8 var(--mono); color:var(--dim); user-select:text; }
.log-line { overflow-wrap:anywhere; }
.hot { color:var(--red); }
.footer { display:flex; justify-content:space-between; gap:12px; color:var(--dim); font-size:12px; padding-top:3px; }
.pair-request { border:1px solid #a28d56; border-radius:12px; background:#2b291e; padding:18px 22px; display:flex; gap:22px; align-items:center; justify-content:space-between; margin-bottom:20px; }
.pair-request .eyebrow { color:var(--yellow); }
.pair-name { font-weight:600; margin:4px 0; overflow-wrap:anywhere; }
.pair-id { font:12px var(--mono); color:var(--dim); overflow-wrap:anywhere; }
.pair-request p { font-size:12px; color:var(--dim); margin-top:6px; }
.pair-code { font:26px var(--mono); letter-spacing:5px; color:var(--yellow); white-space:nowrap; }
.btn-row { display:flex; gap:8px; margin-top:12px; flex-wrap:wrap; }
dialog { background:var(--card); color:var(--text); border:1px solid #53685b; border-radius:14px; max-width:420px; width:calc(100% - 40px); padding:26px; }
dialog::backdrop { background:#080d0bd9; }
dialog p { color:var(--dim); margin-top:12px; font-size:13px; }
dialog .btn-row { justify-content:flex-end; margin-top:22px; }
.btn.danger { color:var(--red); border-color:#82554e; }
.empty-copy,.dim { color:var(--dim); font-size:12px; }
@media(min-width:1100px) { .workspace { padding-top:24px; } .columns { gap:24px; } .panel-body { padding:24px; } }
.control-fieldset { margin:0; padding:0; border:0; min-width:0; }
.workspace-tabs { display:flex; gap:4px; border:1px solid var(--border); border-radius:9px; background:#141b17; padding:4px; margin-bottom:14px; }
.workspace-tab { flex:1; min-height:40px; border:0; border-radius:6px; color:var(--dim); background:transparent; font-weight:600; }
.workspace-tab[aria-selected=true] { background:#3b5144; color:#e0f6e6; }
.notice { display:flex; justify-content:space-between; align-items:center; gap:16px; padding:16px 18px; margin-bottom:16px; border-radius:12px; border:1px solid #a28d56; background:#2b291e; }
.notice h2 { font-size:15px; letter-spacing:0; }
.notice p { font-size:13px; margin-top:4px; }
.notice .btn { flex-shrink:0; }
.notice.error { background:#34231f; border-color:#96665d; color:#ffd0c8; }
.effects-unavailable { padding:14px; border:1px solid #96665d; border-radius:9px; color:#ffd0c8; background:#34231f; margin-bottom:18px; }
.effects-unavailable strong { font-size:14px; }
.effects-unavailable p,.effects-unavailable ol { font-size:13px; margin-top:7px; }
.effects-unavailable summary { padding:12px 0 0; min-height:34px; font-size:13px; }
.effects-unavailable ol { padding-left:20px; }
.effects-unavailable li + li { margin-top:7px; }
.effects-unavailable code { font-size:12px; overflow-wrap:anywhere; }
.voice-panel[data-unavailable=true] .switch:checked { background:#5c685f; border-color:#8d9a91; }
.settings-feedback { font-size:13px; color:var(--yellow); margin-bottom:8px; }
.endpoint-name { font-size:13px; color:var(--dim); margin-top:6px; overflow-wrap:anywhere; }
.discovery-help summary { padding:7px 0 0; min-height:30px; justify-content:flex-start; color:var(--accent); font-size:13px; }
.discovery-help ol { padding-left:18px; font-size:13px; color:var(--dim); margin-bottom:0; }
.discovery-help li + li { margin-top:5px; }
.setup-status { padding:12px 12px; background:#26342b; border-radius:7px; margin:0 0 12px !important; color:var(--text); }
@media(max-width:1000px) {
 .app-header { height:50px; padding:0 20px; } .workspace { padding:16px 20px; } .workspace-heading { margin-bottom:14px; } h1 { font-size:24px; } .subtitle { display:none; }
 .columns { grid-template-columns:minmax(0,1fr); gap:14px; } .routing-column { display:grid; gap:12px; }
 .session { margin-bottom:14px; } .session-connected { grid-template-columns:1.15fr 1fr; gap:8px 20px; padding:12px 16px; }
 .session-metrics { display:flex; grid-column:1 / -1; justify-content:flex-start; gap:22px; border-top:1px solid var(--border); padding-top:8px; }
 .session-metrics > div { display:flex; align-items:center; gap:8px; } .metric-value { font-size:12px; } .metric-label { margin:0; font-size:12px; }
 .session-meta { font-size:12px; } #clientInfo { font-size:14px; } .session-empty { padding:12px 16px; } .session-empty .empty-icon { display:none; }
 .panel-body { padding:16px; } .panel-title { margin-bottom:14px; } .panel-title .eyebrow { display:none; } h2 { font-size:18px; }
 .route { padding-left:32px; } .route + .route { margin-top:18px; } .route-mode { margin-top:8px; } .route label { margin-bottom:6px; } .route-mode label { margin:0; }
 .route-direction { margin-bottom:1px; } .route-note { margin-top:5px; }
 .effect { padding:12px 0; } .effect-description { margin-top:1px; } .strength-row { padding-bottom:12px; } .tuning { margin-bottom:12px; }
 .preview { padding:14px 16px; } .preview-heading { margin-bottom:12px; } .preview-status { min-height:0; margin-top:12px; }
 .pair-request { flex-wrap:wrap; align-items:flex-start; } .header-note .app-label { display:none; } .diagnostics { margin-top:14px; }
}
@media(max-width:480px) { .app-header { padding:0 16px; height:60px; } .app-label { display:none; } .workspace { padding:20px 16px; } h1 { font-size:25px; } .workspace-heading { flex-wrap:wrap; gap:14px; } .session-connected { grid-template-columns:1fr; gap:12px; } .panel-body { padding:18px; } .preview { padding:18px; } .preview-actions { flex-wrap:wrap; } .sample-version { flex:1; justify-content:space-between; } .tuning-grid { grid-template-columns:1fr; gap:15px; } .footer { flex-wrap:wrap; } .header-note { font-size:12px; } .peer-id { display:none; } }
@media(max-width:1000px) and (max-height:700px) {
 .app-header { height:44px; } .workspace { padding-top:8px; } .workspace-heading { margin-bottom:6px; }
 .session-connected { padding:8px 14px; gap:2px 16px; } .session-meta { margin-top:2px; } .session-metrics { padding-top:4px; }
 .session { margin-bottom:6px; } .workspace-tabs { margin-bottom:6px; } .panel-title { margin-bottom:12px; }
 .route + .route { margin-top:12px; } .route-mode { margin-top:6px; } .effect { padding:9px 0; } .preview { padding:12px 16px; }
 .effect-description { display:none; } .preview-heading { margin-bottom:8px; } .preview-heading h3 { display:none; }
 .preview-heading { justify-content:flex-end; } .tuning summary { min-height:34px; padding:8px 14px; }
 .voice-panel .panel-body { padding-bottom:12px; } .voice-panel .panel-title { margin-bottom:8px; }
 .effect { padding:8px 0; } .strength-row { padding-bottom:8px; }
}
@media(prefers-reduced-motion:reduce) { *,*::before { transition:none !important; } }
@media(forced-colors:active) { .switch:checked { background:Highlight; } .switch:checked::before { background:HighlightText; } .level-fill { background:Highlight; } .btn.active { outline:2px solid Highlight; } }
.local-feedback { color:var(--red); font-size:13px; margin:10px 0; line-height:1.5; }
.local-feedback[data-pending=true] { color:var(--accent); }
.mic-readiness { color:var(--dim); font-size:13px; margin-bottom:12px; }
.recording-strip { position:fixed; bottom:12px; left:50%; transform:translateX(-50%); z-index:10; width:calc(100% - 32px); max-width:600px; display:flex; align-items:center; justify-content:space-between; gap:12px; padding:12px 16px; border:1px solid #aa7b70; border-radius:12px; background:#34231f; box-shadow:0 8px 30px #0008; }
.recording-strip p { font-size:13px; }
body[data-recording=true] { padding-bottom:100px; }
.voice-heading { padding-bottom:0; } .voice-heading .panel-title { margin-bottom:16px; } .voice-panel .preview { border-bottom:1px solid var(--border); }
html { height:100%; overflow:hidden; }
body { height:100vh; overflow-y:auto; scroll-padding-top:76px; }
.app-header { position:sticky; top:0; z-index:12; background:var(--bg); }
.app-header:has(.pairing-shortcut:not([hidden])) .header-note { display:none; }
.pairing-shortcut { color:var(--yellow); border-color:#a28d56; font-size:13px; }
body[data-recording=true] { height:calc(100vh - 96px); padding-bottom:0; }
.recording-strip { height:96px; bottom:0; left:0; transform:none; width:100%; max-width:none; border-radius:0; padding:12px 24px; }
.setup-warning { display:flex; align-items:center; justify-content:space-between; gap:12px; background:#2b291e; color:#f3dfab; border:1px solid #a28d56; border-radius:8px; padding:12px; margin-bottom:16px; }
.setup-warning p { margin-top:4px; font-size:13px; }
.setup-warning .btn { flex-shrink:0; }
#micSetupDialog ol { margin:16px 0; padding-left:20px; font-size:14px; }
#micSetupDialog li + li { margin-top:12px; }
#micSetupDialog { max-width:540px; max-height:calc(100vh - 32px); overflow:auto; }
.sample-availability { color:var(--yellow); font-size:13px; margin-top:12px; }
[aria-disabled=true] { opacity:.65; cursor:wait; }
@media(max-width:1000px) and (max-height:700px) {
 .workspace-heading > div:first-child { display:none; }
 .workspace-heading { justify-content:flex-end; }
 .session-connected { padding:8px 14px; }
 .session-metrics { display:none; }
 .setup-warning { margin-bottom:12px; padding:10px; }
 .setup-warning p { display:none; }
 .route-panel:has(#missingMicWarning:not([hidden])) .panel-title { display:none; }
 .route-panel:has(#missingMicWarning:not([hidden])) .route-note { display:none; }
 .effect-description { display:block; }
}
.peer-role-label { display:flex; align-items:center; gap:10px; color:var(--dim); font-size:14px; }
.peer-role-label select { font-size:14px; width:140px; }
.peer-role-status { font-size:14px; line-height:1.5; }
.peer-role-status[data-state=error] { color:var(--red); border-left:3px solid var(--red); padding-left:10px; }
)html") +
std::string(R"html(.peer-role-status[data-state=pending] { color:var(--yellow); }
.peer-name { font-size:14px; flex-basis:100%; }
.peer-role-label { margin-left:auto; }
.pairing-outcome:empty { display:none; }
.pairing-outcome { color:var(--accent); margin:0; font-size:14px; }
.status-pill,.session-meta,.level-label,.metric-label,.route-direction,.control-value,.strength-row label,.strength-row select,.sample-version,.sample-version select,.summary-info,.lowcut-row { font-size:14px; }
.level-row { grid-template-columns:92px 1fr; }
.control-note,.route-note,.route-mode label,.effect-description,.processor-status,.preview-status,.mic-readiness,.sample-availability,.local-feedback { font-size:14px; }
.sample-version select { width:145px; }
.workspace { padding-top:18px; }
.workspace-heading { margin-bottom:14px; }
.session { margin-bottom:16px; }
.panel-body { padding:18px; }
.voice-heading { padding:16px 18px 0; }
.voice-heading .panel-title { margin-bottom:8px; }
.voice-controls { padding-top:8px; padding-bottom:14px; }
.quick-effects { display:grid; grid-template-columns:1.15fr 1fr; gap:20px; }
.quick-effects .effect { border:0; padding:0 0 8px; gap:12px; }
.quick-effects .strength-row { padding:0; gap:10px; }
.quick-effects .strength-row select { width:145px; max-width:100%; min-width:0; }
.quick-effects .strength-row label { max-width:90px; }
.preview { padding:14px 18px; }
.preview-heading { margin-bottom:8px; }
.preview-status { min-height:0; margin-top:8px; }
.mic-readiness { margin-bottom:8px; }
.voice-detail { padding-top:14px; }
.voice-detail .tuning { margin-bottom:8px; }
body[data-recording=true] { height:calc(100vh - 76px); }
.recording-strip { height:76px; padding:8px 20px; }
@media(min-width:1001px) { .columns { grid-template-columns:minmax(300px,.9fr) minmax(0,1.1fr); } }
@media(max-width:1000px) and (max-height:700px) {
 .workspace-heading { margin-bottom:4px; }
 .session-connected { padding:6px 14px; }
 .voice-heading { padding:10px 16px 0; }
 .voice-heading .panel-title { margin-bottom:0; }
 .voice-controls { padding:10px 16px; }
 .quick-effects .effect-description { font-size:14px; line-height:1.35; }
 .quick-effects .effect { padding-bottom:6px; }
 .preview { padding:10px 16px; }
 .preview-heading { justify-content:space-between; margin-bottom:4px; }
 .preview-heading h3 { display:block; font-size:14px; }
 .preview-heading span { font-size:13px; }
 .mic-readiness { margin-bottom:6px; }
 .voice-detail { padding:10px 16px; }
}
@media(max-width:480px) {
 .quick-effects { grid-template-columns:1fr; gap:12px; }
 .sample-version select { width:145px; }
 .status-pill { white-space:normal; }
 .recording-strip { height:96px; }
 body[data-recording=true] { height:calc(100vh - 96px); }
}
.app-header .header-note { margin-left:auto; }
#manageDevices { font-size:14px; white-space:nowrap; }
.processing-availability { color:var(--yellow); font-size:14px; border-left:3px solid var(--yellow); padding:6px 10px; margin-top:8px; }
.device-outcome { color:var(--accent); font-size:14px; }
.device-outcome:empty { display:none; }
#peerRoleHelp,.details > summary,.stream-panel > summary,.peers-panel > summary,.tuning > summary { font-size:14px; }
.operation-links { display:flex; gap:8px; flex-wrap:wrap; margin-top:6px; }
.task-link { color:var(--text); background:transparent; border:1px solid #96665d; border-radius:6px; padding:5px 10px; font-size:14px; min-height:34px; }
.peer-item,.local-feedback,#pairedDevicesSummary { scroll-margin-top:76px; }
@media(max-width:1000px) { .app-header .header-note { display:none; } #manageDevices { margin-left:auto; } }
@media(max-width:1000px) and (max-height:700px) {
 .workspace { padding-top:8px; }
 .status-pill { padding:5px 10px; }
 .session { margin-bottom:8px; }
 #micOwnerLabel { display:none; }
 .route-panel .panel-body { padding:12px 16px; }
 .route-panel .panel-title { margin-bottom:8px; }
 .setup-status { padding:8px 10px; margin-bottom:4px!important; font-size:14px; }
}
@media(max-width:480px) {
 .app-header:has(.pairing-shortcut:not([hidden])) .brand { font-size:0; gap:0; }
 #manageDevices { font-size:13px; padding:6px 8px; }
 .pairing-shortcut { font-size:13px; padding:6px 8px; }
 .notice { flex-wrap:wrap; }
 .peer-role-label { width:100%; margin-left:0; justify-content:space-between; }
}
.pair-request { display:block; padding:12px 18px; }
#pairDetails > summary { min-height:34px; padding:0; font-size:14px; }
.pair-content { display:flex; align-items:center; justify-content:space-between; gap:24px; padding-top:12px; }
.pair-content > div:first-child { flex:1; }
.pair-content p { font-size:14px; }
.pair-consent { margin-top:10px; color:#e1d6b8; }
@media(max-width:1000px) and (max-height:700px) {
 body[data-task-errors=true] .session-connected { padding:6px 12px; gap:10px; }
 body[data-task-errors=true] .session-name { font-size:14px; }
 body[data-task-errors=true] .session-meta { margin-top:2px; }
 body[data-task-errors=true] #clientAddress { display:none; }
 .pair-request { margin-bottom:8px; padding:8px 14px; }
 .notice { padding:10px 12px; margin-bottom:8px; }
 .operation-links { margin-top:4px; }
}
@media(max-width:1000px) { .pair-content { flex-wrap:wrap; gap:14px; } .pair-content > div:first-child { flex-basis:100%; } }
</style>
</head>
<body>
<svg class="symbols" aria-hidden="true" xmlns="http://www.w3.org/2000/svg"><symbol id="icon-wave" viewBox="0 0 24 24"><path d="M3 10v4m5-8v12m4-15v18m4-15v12m5-8v4"/></symbol><symbol id="icon-pc" viewBox="0 0 24 24"><rect x="3" y="4" width="18" height="13" rx="2"/><path d="M8 21h8m-4-4v4"/></symbol><symbol id="icon-mic" viewBox="0 0 24 24"><rect x="9" y="2" width="6" height="12" rx="3"/><path d="M5 10v2a7 7 0 0 0 14 0v-2m-7 9v3m-4 0h8"/></symbol><symbol id="icon-phone" viewBox="0 0 24 24"><rect x="6" y="2" width="12" height="20" rx="3"/><path d="M10 5h4m-3 14h2"/></symbol><symbol id="icon-route" viewBox="0 0 24 24"><path d="M4 7h16m-4-4 4 4-4 4M20 17H4m4-4-4 4 4 4"/></symbol><symbol id="icon-lock" viewBox="0 0 24 24"><rect x="5" y="10" width="14" height="11" rx="2"/><path d="M8 10V7a4 4 0 0 1 8 0v3m-4 5v2"/></symbol></svg>
<header class="app-header"><div class="brand"><span class="brand-mark"><svg class="icon" aria-hidden="true"><use href="#icon-wave"/></svg></span>AudioBridge</div><div class="header-note"><span class="app-)html") +
std::string(R"html(label">Windows desktop</span><svg class="icon" aria-hidden="true" style="width:14px;height:14px"><use href="#icon-lock"/></svg> Local audio. Private by design.</div><button class="btn" id="manageDevices">Devices &amp; access</button><button class="btn pairing-shortcut" id="reviewPairing" aria-label="Review pending pairing request" hidden>Review pairing</button></header>
<main class="workspace">
<div class="workspace-heading"><div><h1>Audio workspace</h1><p class="subtitle">Your PC's sound. Your phone's freedom.</p></div><p class="pairing-outcome" id="pairingOutcome" role="status"></p><div class="status-pill" role="status"><span class="status-dot" id="statusDot"></span><span id="statusText" tabindex="-1">Ready to connect</span></div></div>
<section class="pair-request" id="pairRequestCard" style="display:none" aria-label="Pairing request" role="alert"><details id="pairDetails" open><summary><span id="pairingSummaryText">Pairing request · review codes</span></summary><div class="pair-content"><div><div class="eyebrow">New connection request</div><div class="pair-name" id="pairRequestName"></div><div class="pair-id" id="pairRequestId"></div><p>Compare with your phone. Only approve if both codes match.</p><p class="pair-consent">New devices start <strong>Read only</strong>: listen and use their own phone controls and mic. Grant PC settings and media control separately in <strong>Devices &amp; access</strong>.</p><p class="local-feedback" id="pairingFeedback" role="status" hidden></p></div><div><div class="pair-code" id="pairCode"></div><div class="btn-row"><button class="btn primary" id="btnApprove">Codes match · approve</button><button class="btn" id="btnDeny">Deny</button></div></div></div></details></section>
<div class="notice warning" id="bridgeNotice" role="alert" hidden><div><h2>Desktop controls are disconnected</h2><p>Current connection status is unavailable. Audio may still be running. Controls will return when the desktop responds.</p></div><button class="btn" id="retryBridge">Retry</button></div><div class="notice error" id="operationNotice" role="alert" hidden><div><p id="operationMessage"></p><div class="operation-links" id="operationLinks"></div></div><button class="btn" id="dismissOperationError">Hide summary</button></div><section class="session" id="sessionOverview" aria-label="Connection overview"><div class="session-connected" id="connectedInfo" style="display:none"><div class="session-identity"><div class="session-name"><svg class="icon" aria-hidden="true"><use href="#icon-phone"/></svg><span id="clientInfo"></span></div><div class="session-meta"><span id="clientAddress"></span><span class="encryption-badge" id="sessionSecurity"></span></div><div class="session-meta" id="micOwnerLabel"></div></div><div><div class="level-row"><span class="level-label">PC audio</span><div class="level-bar" role="meter" aria-label="PC audio level" aria-valuemin="0" aria-valuemax="100" aria-valuenow="0" id="sendMeter"><div class="level-fill" id="sendLevelFill"></div></div></div><div class="level-row"><span class="level-label" title="Incoming phone audio, not proof of delivery to a calling app">Mic received</span><div class="level-bar" role="meter" aria-label="Phone microphone received level" aria-valuemin="0" aria-valuemax="100" aria-valuenow="0" id="recvMeter"><div class="level-fill" id="recvLevelFill"></div></div></div></div><div class="session-metrics"><div><span class="metric-value" id="rttValue">—</span><span class="metric-label">Network RTT</span></div><div><span class="metric-value" id="uptimeValue">—</span><span class="metric-label">Session time</span></div></div></div><div class="session-empty" id="disconnectedInfo"><div class="empty-icon"><svg class="icon" aria-hidden="true"><use href="#icon-phone"/></svg></div><div><h2>Let's connect your phone</h2><p>Use the same Wi-Fi, open AudioBridge on your phone, and select this PC. Then compare the pairing codes.</p><div class="server-info" id="serverInfo">Finding your server…</div><details class="discovery-help"><summary>Can’t find this PC?</summary><ol><li>Connect both devices to the same local network; guest Wi-Fi may isolate them.</li><li>Rescan in the phone app and allow AudioBridge through Windows Firewall on your private network.</li><li>If a VPN blocks local devices, allow local network access. Keep pairing-code verification enabled.</li></ol></details></div></div></section>
<nav class="workspace-tabs" id="workspaceTabs" role="tablist" aria-label="Audio workspace" hidden><button id="routingTab" class="workspace-tab" role="tab" aria-selected="true" aria-controls="routeWorkspace">Audio routing</button><button id="microphoneTab" class="workspace-tab" role="tab" aria-selected="false" aria-controls="voiceCard" tabindex="-1">Microphone &amp; test</button></nav><fieldset class="control-fieldset" id="workspaceControls" aria-label="Audio controls"><div class="columns"><div class="routing-column" id="routeWorkspace" aria-labelledby="routingTitle">
<section class="panel route-panel" aria-labelledby="routingTitle"><div class="panel-body"><div class="panel-title"><div><div class="eyebrow">Signal path</div><h2 id="routingTitle">Audio routing</h2></div><svg class="icon" aria-hidden="true"><use href="#icon-route"/></svg></div><div class="setup-warning" id="missingMicWarning" role="status" hidden><div><strong>No virtual microphone for calls</strong><p>Install a virtual input to send your phone mic into a calling app.</p></div><button class="btn" id="openMicSetup">Setup guide</button></div><div class="route"><svg class="icon route-icon" aria-hidden="true"><use href="#icon-pc"/></svg><div class="route-direction">01 / PC → PHONE</div><label for="deviceSelect">Send PC audio</label><select id="deviceSelect" aria-describedby="selectedDeviceName"><option>Default device</option></select><p class="endpoint-name" id="selectedDeviceName" hidden></p><p class="local-feedback" id="routingFeedback" role="status" hidden></p><p class="local-feedback" id="captureStatus" role="status" hidden></p><div class="route-mode"><label for="captureModeSelect">Capture</label><select id="captureModeSelect"><option value="loopback">System audio</option><option value="recording">Recording device</option></select></div></div><div class="route"><svg class="icon route-icon" aria-hidden="true"><use href="#icon-mic"/></svg><div class="route-direction">02 / PHONE → PC</div><label for="outDeviceSelect">Return phone microphone</label><select id="outDeviceSelect" aria-describedby="selectedOutputName"><option>Default device</option></select><p class="endpoint-name" id="selectedOutputName" hidden></p><p class="local-feedback" id="outputReadiness" role="status" hidden></p><p class="setup-status" id="driverSetupStatus">Checking microphone destinations…</p><p class="route-note" id="routeMicStatus"></p><p class="local-feedback" id="outputFeedback" role="status" hidden></p></div></div><details class="details"><summary>Headset setup checklist</summary><div class="details-content help-copy"><ol><li><strong>Check the virtual microphone.</strong> If CABLE Input is missing, install VB-CABLE from its official vendor and restart AudioBridge. You can instead use AudioBridge Microphone when its driver and service are available. Do not select an unavailable endpoint.</li><li>Use <strong>System audio</strong> and select the PC playback device used by your games and calls.</li><li>Return the phone microphone to <strong>CABLE Input</strong>. Choose <strong>CABLE Output</strong> as your microphone in Discord, Zoom, or your game.</li><li>Listen through headphones connected to your phone. One VB-CABLE is enough.</li><li><strong>Verify the route.</strong> Enable your phone mic and speak. Watch the Phone mic meter here, then the input meter in your calling app. The calling app must use CABLE Output (or AudioBridge Microphone), not CABLE Input.</li></ol><p>Keep CABLE Input dedicated to your mic. Do not send it back to the phone or into its own Voicemeeter bus. AudioBridge Microphone is an alternative with its working driver and service installed.</p></div></details></section>
<details class="panel stream-panel"><summary>Stream tuning <span class="summary-info" id="streamSummary">PCM · 2.5 ms</span></summary><div class="details-content"><div class="control-group"><div class="control-heading"><span id="codecLabel">Audio format</span><span class="control-value" id="codecValue">Raw PCM</span></div><div class="segmented" role="group" aria-labelledby="codecLabel"><button class="btn codec-btn active" id="codecBtnPcm" data-codec="pcm" aria-pressed="true">Raw PCM</button><button class="btn codec-btn" id="codecBtnOpus" data-codec="opus" aria-pressed="false">Opus</button></div><p class="control-note">PCM skips codec lookahead. Opus uses less bandwidth.</p></div><p class="local-feedback" id="transportFeedback" role="status" hidden></p><div class="control-group"><div class="control-heading"><span id="frameLabel">Packet duration</span><span class="control-value" id="frameSizeValue">2.5 ms</span></div><div class="segmented" role="group" aria-labelledby="frameLabel"><button class="btn frame-btn active" id="frameBtn2" data-ms="2" aria-pressed="true">2.5 ms</button><button class="btn frame-btn" id="frameBtn5" data-ms="5" aria-pressed="false">5 ms</button><button class="btn frame-btn" id="frameBtn10" data-ms="10" aria-pressed="false">10 ms</button><button class="btn frame-btn" id="frameBtn20" data-ms="20" aria-pressed="false">20 ms</button></div></div><div class="control-group"><div class="control-heading"><label for="jitterSlider">Playback buffer</label><span class="control-value" id="jitterValue">10 ms</span></div><input type="range" id="jitterSlider" min="0" max="100" step="1" value="10"><div class="range-caption"><span>Lower latency</span><span>More stability</span></div></div></div></details>
<details c)html") +
std::string(R"html(lass="panel peers-panel"><summary id="pairedDevicesSummary"><span>Paired devices <span class="peer-count" id="peerCount">0</span></span></summary><div class="details-content"><p class="empty-copy" id="peerRoleHelp">Read only: listen and use local phone controls. Admin: also change PC audio settings and PC media playback. Only this desktop can change permissions. New and existing pairings default to Read only.</p><p id="devicesOutcome" class="device-outcome" role="status"></p><div id="peerList"><p class="empty-copy">No paired devices yet. Connect your phone to get started.</p></div></div></details>
</div>
<section class="panel voice-panel" id="voiceCard" aria-labelledby="voiceTitle"><div class="panel-body voice-heading"><div class="panel-title"><div><div class="eyebrow">Microphone studio</div><h2 id="voiceTitle">Voice cleanup</h2></div><svg class="icon" aria-hidden="true"><use href="#icon-wave"/></svg></div><p class="processing-availability" id="processingAvailability" role="status" hidden>Loading voice models · saved cleanup preferences are inactive.</p></div><div class="panel-body voice-controls"><div class="effects-unavailable" id="effectsUnavailable" role="status" hidden><strong id="effectsUnavailableTitle">Effects unavailable · preferences inactive</strong><p id="effectsUnavailableDescription">Saved preferences below are inactive. Unprocessed audio is used when the phone microphone is streaming.</p><details><summary>Restore voice processing</summary><ol><li>Keep <code>onnxruntime.dll</code> and the complete <code>voice-models</code> folder beside <code>audiobridge.exe</code>.</li><li>If files were moved or removed, restore them from the same desktop build.</li><li>Close and relaunch AudioBridge to load the models again. Your phone app does not need an update.</li></ol></details></div><p class="local-feedback" id="settingsFeedback" role="status" hidden></p><div class="quick-effects"><div><div class="effect"><div><label class="effect-name" for="voiceNoise">Noise removal</label><p class="effect-description" id="noiseHelp">Keep the voice. Cut the background.</p></div><input class="switch" id="voiceNoise" type="checkbox" role="switch" aria-describedby="noiseHelp" checked></div><div class="strength-row"><label for="voiceStrength">Strength</label><select id="voiceStrength"><option value="6">Gentle · 6 dB</option><option value="10">Light · 10 dB</option><option value="15">Medium · 15 dB</option><option value="20">Strong · 20 dB</option><option value="30">Intense · 30 dB</option><option value="100" selected>Maximum</option></select></div></div><div class="effect"><div><label class="effect-name" for="voiceGate">Voice gate</label><p class="effect-description" id="gateHelp">Quiet the mic when you're not speaking.</p></div><input class="switch" id="voiceGate" type="checkbox" role="switch" aria-describedby="gateHelp" checked></div></div></div><div class="preview"><div class="preview-heading"><h3>Test your microphone</h3><span id="voiceTime">10-second mic check</span></div><p class="mic-readiness" id="micReadiness"></p><div class="preview-actions"><button class="btn primary voice-btn" id="voiceRecord" aria-pressed="false" disabled>Record sample</button><div class="sample-version"><label for="voiceSource">Listen to</label><select id="voiceSource"><option value="1" id="processedOption">Processed</option><option value="0">Original</option></select></div></div><p class="local-feedback" id="recordingFeedback" role="status" hidden></p><p class="local-feedback" id="clipFeedback" role="status" hidden></p><p class="sample-availability" id="sampleAvailability" role="status" hidden></p><button class="btn" id="retrySample" hidden>Reload sample</button><audio id="voiceAudio" controls preload="auto" aria-label="Phone microphone sample playback" hidden></audio><p class="preview-status" id="voicePreviewStatus" role="status">Connect your phone and enable its mic, then record a sample to compare.</p></div><div class="panel-body voice-detail"><details class="tuning"><summary>Fine-tune your microphone</summary><div class="details-content"><div class="tuning-grid"><div><div class="control-heading"><label for="voiceThreshold">Speech threshold</label><span class="control-value" id="voiceThresholdValue">50%</span></div><input id="voiceThreshold" type="range" min="10" max="90" value="50"><p class="control-note">Higher requires clearer speech.</p></div><div><div class="control-heading"><label for="voiceHold">Gate hold</label><span class="control-value" id="voiceHoldValue">0.3 s</span></div><input id="voiceHold" type="range" min="100" max="1500" step="100" value="300"><p class="control-note">Wait before quieting the mic.</p></div></div><label class="lowcut-row"><input id="voiceLowCut" type="checkbox">Reduce low rumble <span class="dim">· 80 Hz</span></label></div></details><p class="processor-status" id="voiceStatus" role="status">Loading voice models…</p></div></section>
</div></fieldset>
<details class="diagnostics"><summary>Connection diagnostics <span class="summary-info">Activity &amp; transport</span></summary><div class="details-content"><div class="stats-row" id="statsRow1">No stream activity yet.</div><div class="stats-row" id="statsRow2"></div><div class="log-container" id="logContainer" tabindex="0" role="region" aria-label="Connection activity log"><div>Waiting for events…</div></div></div></details>
)html") +
std::string(R"html(<dialog id="appUpdatesDialog" aria-labelledby="appUpdatesTitle"><h2 id="appUpdatesTitle">App updates</h2><p id="installedVersion">Installed: 0.2.0</p><p id="updateMessage" role="status" tabindex="-1">Check the official GitHub release for a newer version.</p><progress id="updateProgress" max="1" value="0" aria-label="Update download progress" hidden></progress><p>Your pairings and settings are kept. This updates Windows only; your phone has its own update button.</p><div class="btn-row"><button class="btn" id="closeAppUpdates">Close</button><button class="btn primary" id="checkAppUpdates">Check for updates</button><button class="btn primary" id="installAppUpdate" hidden>Update now</button></div></dialog><dialog id="confirmAppUpdateDialog" aria-labelledby="confirmAppUpdateTitle"><h2 id="confirmAppUpdateTitle">Install AudioBridge update?</h2><p>The complete Windows release is downloaded from the official GitHub repository and verified before installation. Audio will stop and AudioBridge will restart. Your pairings, preferences and installed drivers are not removed.</p><div class="btn-row"><button class="btn" id="cancelAppUpdate" autofocus>Not now</button><button class="btn primary" id="confirmAppUpdate">Download and restart</button></div></dialog>)html") +
std::string(R"html(<footer class="footer"><span>AudioBridge / Windows 0.2.0</span><button class="btn" id="openAppUpdates">App updates</button><span>Processed on your PC. Recordings stay in memory.</span></footer>
</main><aside class="recording-strip" id="recordingStrip" aria-label="Recording controls" hidden><div><strong id="recordingStripTitle">Recording phone microphone</strong><p id="recordingStripStatus">Recording stops at 10 seconds.</p></div><button class="btn danger" id="persistentStop">Stop recording</button></aside>
<dialog id="micSetupDialog" aria-labelledby="micSetupTitle"><h2 id="micSetupTitle">Set up your phone as a PC microphone</h2><p>For calling apps, install a virtual microphone on this PC. Playing to ordinary speakers is still possible without one.</p><ol><li>Get <strong>VB-CABLE</strong> from VB-Audio’s official download page. Follow its Windows installation instructions and restart if prompted.</li><li>Relaunch AudioBridge, then choose <strong>CABLE Input</strong> under Return phone microphone.</li><li>Choose <strong>CABLE Output</strong> as the microphone in your calling app. Enable your phone mic and check its level.</li></ol><p>Already have the AudioBridge microphone driver and service installed? Choose <strong>AudioBridge Microphone</strong> in both apps instead. An unavailable entry is not ready for use.</p><p class="local-feedback" id="setupOpenError" role="status" hidden></p><div class="btn-row"><button class="btn primary" id="openCableDownload">Open official VB-CABLE page</button><button class="btn" id="closeMicSetup" autofocus>Done</button></div></dialog><dialog id="removeDeviceDialog" aria-labelledby="removeDeviceTitle" aria-describedby="removeDeviceDescription"><h2 id="removeDeviceTitle">Remove paired device?</h2><p id="removeDeviceDescription"></p><p id="removeDeviceError" role="alert"></p><div class="btn-row"><button class="btn" id="cancelRemoveDevice" autofocus>Keep device</button><button class="btn danger" id="confirmRemoveDevice">Remove device</button></div></dialog>
<script>
let lastLogCount = 0;
let lastPeerJson = '';
let currentPairId)html") +
std::string(R"html( = '';
let currentDeviceIndex = -1;
let currentOutDeviceIndex = -1;

function frameLabel(ms) {
    return (ms === 2) ? '2.5 ms' : (ms + ' ms');
}

var operationErrors = {};
var dismissedNoticeSignature = null;
var operationPending = {};
var operationRequests = {};
var operationExpectations = {};
var confirmedCaptureMode = 'loopback';
var confirmedState = null;
var feedbackIds = {routing:'routingFeedback', output:'outputFeedback', transport:'transportFeedback', settings:'settingsFeedback', recording:'recordingFeedback', clip:'clipFeedback', playback:'clipFeedback', pairing:'pairingFeedback'};
function setText(id, value) {
    var element = document.getElementById(id);
    if (element.textContent !== value) element.textContent = value;
}
var bridgeFailed = false;
var tickBusy = false;
function showOperationError(key, message) {
    dismissedNoticeSignature = null;
    operationErrors[key] = message;
    renderOperationErrors();
}
function clearOperationError(key) {
    delete operationErrors[key];
    renderOperationErrors();
}
var taskRecovery = {
    routing:['PC audio','routing','deviceSelect'], output:['Phone microphone','routing','outDeviceSelect'],
    transport:['Stream tuning','routing','jitterSlider'], settings:['Voice cleanup','microphone','voiceNoise'],
    recording:['Recording','microphone','voiceRecord'], clip:['Sample playback','microphone','voiceSource'],
    playback:['Sample playback','microphone','voiceSource'], pairing:['Pairing request',null,'btnApprove']
};
function revealTask(key) {
    var task = taskRecovery[key];
    if (task[1]) { activeWorkspace = task[1]; renderWorkspace(false); }
    if (key === 'transport') document.querySelector('.stream-panel').open = true;
    if (key === 'pairing') document.getElementById('pairDetails').open = true;
    var target = document.getElementById(task[2]);
    target.focus();
    target.scrollIntoView({block:'center'});
}
function renderOperationErrors() {
    var keys = Object.keys(operationErrors);
    document.body.setAttribute('data-task-errors', String(keys.length > 0));
    document.getElementById('operationNotice').hidden = keys.length === 0 || bridgeFailed || dismissedNoticeSignature === JSON.stringify(operationErrors);
    setText('operationMessage', keys.length === 1 ? '1 task needs attention' : keys.length + ' tasks need attention');
    var links = document.getElementById('operationLinks');
    var signature = keys.join('|');
    if (links.getAttribute('data-tasks') !== signature) {
        links.setAttribute('data-tasks', signature);
        links.replaceChildren();
        keys.forEach(function(key) {
            var task = taskRecovery[key];
            if (!task) return;
            var button = document.createElement('button');
            button.className = 'task-link';
            button.textContent = task[0];
            button.addEventListener('click', function() { revealTask(key); });
            links.appendChild(button);
        });
    }
    Object.keys(feedbackIds).forEach(function(key) {
        var element = document.getElementById(feedbackIds[key]);
        var message = operationPending[key] ? 'Applying change…' : operationErrors[key] || '';
        if (key === 'settings' && (voiceEditing || voiceSettingsPending)) message = voiceSettingsPending ? 'Applying settings…' : 'Changes pending…';
        if (key === 'playback' && !message) return;
        if (element.hidden !== !message) element.hidden = !message;
        var pending = String(!!operationPending[key] || (key === 'settings' && (voiceEditing || voiceSettingsPending)));
        if (element.getAttribute('data-pending') !== pending) element.setAttribute('data-pending', pending);
        setText(feedbackIds[key], message);
    });
    if (operationErrors.recording) setText('recordingStripStatus', operationErrors.recording);
}
document.getElementById('dismissOperationError').addEventListener('click', function() {
    dismissedNoticeSignature = JSON.stringify(operationErrors);
    renderOperationErrors();
});
function requestBridge(action) {
    return new Promise(function(resolve, reject) {
        var timeout = setTimeout(function() { var error = new Error('Desktop response timed out'); error.timedOut = true; reject(error); }, 2000);
        Promise.resolve().then(action).then(function(value) { clearTimeout(timeout); resolve(value); }, function(error) { clearTimeout(timeout); reject(error); });
    });
}
function reconcileOperationOutcomes(state) {
    Object.keys(operationExpectations).forEach(function(key) {
        var expected = operationExpectations[key];
        if (operationPending[key] || (key === 'settings' && (voiceEditing || voiceSettingsPending)) || (key === 'recording' && voiceRecordBusy)) return;
        if (expected.request !== operationRequests[key]) { delete operationExpectations[key]; return; }
        if (!expected.matches(state)) return;
        if (key === 'pairing' && operationErrors.pairing) setText('pairingOutcome', 'Request is no longer pending. Approval or denial was not confirmed.');
        delete operationExpectations[key];
        clearOperationError(key);
    });
}
function invokeBridge(key, action, message, confirms) {
    var request = operationRequests[key] = (operationRequests[key] || 0) + 1;
    operationPending[key] = true;
    if (confirms) operationExpectations[key] = {request:request, matches:confirms};
    clearOperationError(key);
    return requestBridge(action).then(async function(result) {
        var receipt = typeof result === 'string' ? JSON.parse(result) : result;
        if (receipt && receipt.applied === false) throw new Error('Change was not applied');
        if (confirms) {
            var snapshot = await requestBridge(() => window._tick());
            var state = typeof snapshot === 'string' ? JSON.parse(snapshot) : snapshot;
            confirmedState = state;
            if (!confirms(state)) throw new Error('Change was not confirmed');
        }
        if (request !== operationRequests[key]) return false;
        delete operationPending[key];
        delete operationExpectations[key];
        clearOperationError(key);
        return true;
    }).catch(function(error) {
        if (request !== operationRequests[key]) return false;
        delete operationPending[key];
        if (confirmedState) reconcileControls(confirmedState);
        document.getElementById('captureModeSelect').value = confirmedCaptureMode;
        showOperationError(key, error.timedOut ? 'Desktop response timed out. The outcome is unknown; current state will refresh before you retry.' : message);
        return false;
    });
}
document.getElementById('retryBridge').addEventListener('click', function() { tick(); });
function updateEndpointHint(selectId, hintId) {
    var select = document.getElementById(selectId);
    if (!select.selectedOptions || !select.selectedOptions.length) return;
    var name = select.selectedOptions[0].textContent;
    select.title = name;
    var hint = document.getElementById(hintId);
    hint.textContent = name;
    var canvas = document.createElement('canvas');
    var context = canvas.getContext('2d');
    context.font = getComputedStyle(select).font;
    hint.hidden = select.clientWidth === 0 || context.measureText(name).width <= select.clientWidth - 42;
}
var compactLayout = window.matchMedia('(max-width: 1000px)');
var activeWorkspace = 'routing';
function renderWorkspace(focusTab) {
    var compact = compactLayout.matches;
    document.getElementById('workspaceTabs').hidden = !compact;
    [['routing','routingTab','routeWorkspace','routingTitle'],['microphone','microphoneTab','voiceCard','voiceTitle']].forEach(function(entry) {
        var active = activeWorkspace === entry[0];
        var tab = document.getElementById(entry[1]);
        var panel = document.getElementById(entry[2]);
        tab.setAttribute('aria-selected', String(active));
        tab.tabIndex = active ? 0 : -1;
        panel.hidden = compact && !active;
        if (compact) { panel.setAttribute('role', 'tabpanel'); panel.setAttribute('aria-labelledby', entry[1]); }
        else { panel.removeAttribute('role'); panel.setAttribute('aria-labelledby', entry[3]); }
        if (focusTab && active) tab.focus();
    });
    updateEndpointHint('deviceSelect', 'selectedDeviceName');
    updateEndpointHint('outDeviceSelect', 'selectedOutputName');
}
['routingTab','microphoneTab'].forEach(function(id, index) {
    var tab = document.getElementById(id);
    tab.addEventListener('click', function() { activeWorkspace = index === 0 ? 'routing' : 'microphone'; renderWorkspace(false); });
    tab.addEventListener('keydown', function(event) {
        if (!['ArrowLeft','ArrowRight','Home','End'].includes(event.key)) return;
        event.preventDefault();
        activeWorkspace = event.key === 'Home' ? 'routing' : event.key === 'End' ? 'microphone' : (activeWorkspace === 'routing' ? 'microphone' : 'routing');
        renderWorkspace(true);
    });
});
compactLayout.addEventListener('change', function() {
    if (compactLayout.matches && document.getElementById('voiceCard').contains(document.activeElement)) activeWorkspace = )html") +
std::string(R"html('microphone';
    renderWorkspace(false);
    if (!compactLayout.matches && document.activeElement === document.getElementById('microphoneTab')) document.getElementById('voiceNoise').focus();
    if (!compactLayout.matches && document.activeElement === document.getElementById('routingTab')) document.getElementById('deviceSelect').focus();
});
window.addEventListener('resize', function() {
    updateEndpointHint('deviceSelect', 'selectedDeviceName');
    updateEndpointHint('outDeviceSelect', 'selectedOutputName');
});
renderWorkspace(false);

// Event listeners
document.getElementById('jitterSlider').addEventListener('input', function() {
    document.getElementById('jitterValue').textContent = this.value + ' ms';
    var desired = Number(this.value);
    invokeBridge('transport', () => window._setJitter(String(desired)), 'Playback buffer could not be changed. Adjust it again to retry.', state => state.jitterBufferMs === desired);
    this.setAttribute('aria-valuetext', this.value + ' milliseconds');
});

document.querySelectorAll('.frame-btn').forEach(function(btn) {
    btn.addEventListener('click', function() {
        var ms = parseInt(this.getAttribute('data-ms'));
        document.querySelectorAll('.frame-btn').forEach(function(b) { b.classList.remove('active'); b.setAttribute('aria-pressed', 'false'); });
        this.classList.add('active');
        this.setAttribute('aria-pressed', 'true');
        document.getElementById('frameSizeValue').textContent = frameLabel(ms);
        invokeBridge('transport', () => window._setFrameSize(String(ms)), 'Packet duration could not be changed. Please try again.', state => state.frameSizeMs === ms);
    });
});

document.querySelectorAll('.codec-btn').forEach(function(btn) {
    btn.addEventListener('click', function() {
        var codec = this.getAttribute('data-codec');
        document.querySelectorAll('.codec-btn').forEach(function(b) { b.classList.remove('active'); b.setAttribute('aria-pressed', 'false'); });
        this.classList.add('active');
        this.setAttribute('aria-pressed', 'true');
        document.getElementById('codecValue').textContent = (codec === 'pcm') ? 'Raw PCM' : 'Opus';
        invokeBridge('transport', () => window._setCodec(codec), 'Audio format could not be changed. Please try again.', state => state.rawPcm === (codec === 'pcm'));
    });
});

document.getElementById('captureModeSelect').addEventListener('change', function() {
    var mode = this.value;
    invokeBridge('routing', () => window._setCaptureMode(mode), 'Capture mode was not confirmed. The native selection is shown. Check your device and retry.', state => state.captureMode === mode && state.captureReady);
});

document.getElementById('deviceSelect').addEventListener('change', function() {
    var desired = this.selectedIndex;
    invokeBridge('routing', () => window._selectDevice(String(desired)), 'PC audio source could not be changed. Please try again.', state => state.currentDeviceIndex === desired && state.captureReady);
});

document.getElementById('outDeviceSelect').addEventListener('change', function() {
    var desired = this.selectedIndex;
    invokeBridge('output', () => window._selectOutDevice(String(desired)), 'Microphone destination could not be changed. Please try again.', state => state.currentOutDeviceIndex === desired);
});

async function completePairing(approve) {
    if (!currentPairId || bridgeFailed || operationPending.pairing) return;
    var pairId = currentPairId;
    var success = await invokeBridge('pairing', () => approve ? window._approvePair(pairId) : window._denyPair(pairId), 'Pairing decision was not confirmed. Compare the codes and try again.', state => !state.pairRequest.pending || state.pairRequest.clientId !== pairId);
    if (success) {
        setText('pairingOutcome', approve ? 'Pairing approved.' : 'Pairing denied.');
        updatePairRequest(confirmedState.pairRequest);
    }
}
document.getElementById('btnApprove').addEventListener('click', function() { completePairing(true); });
document.getElementById('btnDeny').addEventListener('click', function() { completePairing(false); });

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

function outputUnavailable(state) {
    if (!state) return false;
    var name = state.currentOutDeviceIndex ? (state.outDevices || [])[state.currentOutDeviceIndex - 1] : state.defaultOutDeviceName;
    return state.outputReady === false || /unavailable/i.test(name || '');
}
function updateStatusCard(s) {
    var dot = document.getElementById('statusDot');
    var text = document.getElementById('statusText');
    document.getElementById('connectedInfo').style.display = s.connected ? '' : 'none';
    document.getElementById('disconnectedInfo').style.display = s.connected ? 'none' : '';
    dot.className = 'status-dot' + (s.connected ? (s.paused || s.captureReady === false || outputUnavailable(s) ? ' yellow' : ' green') : '');
    setText('statusText', s.connected ? (s.captureReady === false ? 'Connected · PC audio unavailable' : outputUnavailable(s) ? 'Connected · mic return unavailable' : s.paused ? 'Playback paused' : 'Streaming to ' + (s.clientCount || 1) + ((s.clientCount || 1) === 1 ? ' phone' : ' phones')) : 'Ready to connect');
    if (!s.connected) {
        document.getElementById('serverInfo').textContent = s.serverName + ' · ' + s.serverMac;
        document.getElementById('statsRow1').textContent = 'No active audio stream.';
        document.getElementById('statsRow2').textContent = '';
        return;
    }
    document.getElementById('clientInfo').textContent = s.clientName;
    document.getElementById('clientAddress').textContent = s.clientAddress;
    var security = document.getElementById('sessionSecurity');
    security.textContent = s.encrypted ? 'DTLS ENCRYPTED' : 'NOT ENCRYPTED';
    security.className = 'encryption-badge' + (s.encrypted ? '' : ' unencrypted');
    document.getElementById('micOwnerLabel').textContent = s.micOwner ? 'Mic return · ' + s.micOwner : 'No phone microphone assigned';
    document.getElementById('rttValue').textContent = s.clientRttMs > 0 ? s.clientRttMs.toFixed(0) + ' ms' : '—';
    document.getElementById('uptimeValue').textContent = formatUptime(s.uptimeSeconds);
    [['send',s.captureReady === false ? 0 : s.peakLevel],['recv',s.micOwner ? s.micPeakLevel : 0]].forEach(function(entry) {
        var level = Math.max(0, Math.min(entry[1] || 0, 1));
        var fill = document.getElementById(entry[0] + 'LevelFill');
        fill.style.width = level * 100 + '%';
        fill.style.background = level > .9 ? 'var(--red)' : (level > .7 ? 'var(--orange)' : 'var(--green)');
        document.getElementById(entry[0] + 'Meter').setAttribute('aria-valuenow', Math.round(level * 100));
    });
    document.getElementById('statsRow1').textContent = s.packetsSent + ' packets · ' + formatBytes(s.bytesSent) + ' · ' + s.kbps.toFixed(0) + ' kbps · Sequence ' + s.sequenceNum;
    var row = document.getElementById('statsRow2');
    row.textContent = 'Peak ' + s.peakLevel.toFixed(3) + (s.peakLevel > .95 ? ' · Clipping risk' : '') + (s.voiceQos ? ' · Wi-Fi voice priority' : '') + (s.kernelTap ? ' · Kernel tap active' : '');
    row.className = 'stats-row' + (s.peakLevel > .95 ? ' hot' : '');
}

)html") +
std::string(R"html(function renderPeerRole(row) {
    var status = row.querySelector('.peer-role-status');
    var current = 'Current access: ' + (row._confirmedRole === 'admin' ? 'Admin. PC settings and media controls enabled.' : 'Read only. PC settings and media controls blocked.');
    status.dataset.state = row._rolePending ? 'pending' : row._roleFeedback ? 'error' : 'confirmed';
    var requested = row._requestedRole === 'admin' ? 'Admin' : 'Read only';
    status.textContent = row._rolePending ? 'Changing to ' + requested + '… ' + current : (row._roleFeedback ? row._roleFeedback + ' ' : '') + current;
    if (row.contains(document.activeElement)) status.scrollIntoView({block:'nearest'});
}
function updatePeers(peers) {
    confirmRemovalState(peers);
    // Only rebuild DOM when peer data actually changes
    var peerJson = JSON.stringify(peers);
    if (peerJson === lastPeerJson) return;
    lastPeerJson = peerJson;

    var container = document.getElementById('peerList');
    document.getElementById('peerCount').textContent = String(peers.length);

    if (!peers.length) {
        container.innerHTML = '<div class="dim small">No paired devices</div>';
        return;
    }

    var existing = new Map(Array.from(container.querySelectorAll('.peer-item')).map(row => [row.dataset.peerId, row]));
    if (!existing.size) container.innerHTML = '';
    existing.forEach(function(row, id) { if (!peers.some(peer => peer.id === id)) row.remove(); });
    for (var i = 0; i < peers.length; i++) {
        (function(peerId) {
            var p = peers[i];
            var previous = existing.get(peerId);
            if (previous) {
                var refreshedRole = p.role === 'admin' ? 'admin' : 'reader';
                if (refreshedRole !== previous._confirmedRole) previous._roleFeedback = '';
                previous._confirmedRole = refreshedRole;
                if (!previous._rolePending) previous.querySelector('.peer-role').value = previous._confirmedRole;
                renderPeerRole(previous);
                previous.querySelector('.peer-age').textContent = formatAge(p.lastConnected);
                return;
            }
            var shortId = p.id.length > 12 ? p.id.substring(0, 12) + '...' : p.id;
            var row = document.createElement('div');
            row.className = 'peer-item';
            row.dataset.peerId = peerId;
            row._confirmedRole = p.role === 'admin' ? 'admin' : 'reader';
            row.innerHTML =
                '<span class="peer-name">\u2022 ' + escapeHtml(p.name) + '</span>' +
                '<span class="peer-id">' + escapeHtml(shortId) + '</span>' +
                '<span class="peer-age">' + formatAge(p.lastConnected) + '</span>';
            var role = document.createElement('select');
            role.className = 'peer-role';
            role.setAttribute('aria-label', 'Control level for ' + p.name);
            role.setAttribute('aria-describedby', 'peerRoleHelp');
            role.innerHTML = '<option value="reader">Read only</option><option value="admin">Admin</option>';
            role.value = row._confirmedRole;
            var status = document.createElement('span');
            status.className = 'peer-role-status';
            status.setAttribute('role', 'status');
            role.addEventListener('change', async function() {
                if (row._rolePending) { role.value = row._requestedRole; return; }
                var requested = role.value;
                row._requestedRole = requested;
                row._rolePending = true;
                role.setAttribute('aria-disabled', 'true');
                role.setAttribute('aria-busy', 'true');
                row._roleFeedback = '';
                renderPeerRole(row);
                try {
                    var receipt = await requestBridge(() => window._setPeerRole((requested === 'admin' ? '1:' : '0:') + peerId));
                    receipt = typeof receipt === 'string' ? JSON.parse(receipt) : receipt;
                    if (receipt && receipt.applied === false) throw Error('Save rejected');
                    if (!receipt || receipt.applied !== true) throw Error('Save outcome unknown');
                    var snapshot = await requestBridge(() => window._tick());
                    snapshot = typeof snapshot === 'string' ? JSON.parse(snapshot) : snapshot;
                    var confirmed = snapshot.peers.find(peer => peer.id === peerId);
                    if (!confirmed || (confirmed.role || 'reader') !== requested) throw Error('Not confirmed');
                    row._confirmedRole = requested;
                    row._roleFeedback = '';
                } catch (error) {
                    var requestedName = requested === 'admin' ? 'Admin' : 'Read only';
                    row._roleFeedback = error.message === 'Save rejected' ? 'Could not change to ' + requestedName + '.' : row._confirmedRole === requested ? '' : 'Change to ' + requestedName + ' not confirmed. Awaiting the saved level; check before retrying.';
                } finally {
                    row._rolePending = false;
                    lastPeerJson = null;
                    role.setAttribute('aria-disabled', 'false');
                    role.setAttribute('aria-busy', 'false');
                    role.value = row._confirmedRole;
                    renderPeerRole(row);
                }
            });
            role.addEventListener('keydown', function(event) {
                if (row._rolePending && event.key !== 'Tab') event.preventDefault();
            });
            var roleLabel = document.createElement('label');
            roleLabel.className = 'peer-role-label';
            roleLabel.appendChild(document.createTextNode('Control level'));
            roleLabel.appendChild(role);
            row.appendChild(roleLabel);
            var btn = document.createElement('button');
            btn.className = 'peer-revoke';
            btn.textContent = 'Remove';
            btn.setAttribute('aria-label', 'Remove pairing for ' + p.name);
            var peerName = p.name;
            btn.addEventListener('click', function() {
                confirmPeerRemoval(peerId, peerName);
            });
            row.appendChild(btn);
            row.appendChild(status);
            renderPeerRole(row);
            container.appendChild(row);
        })(peers[i].id);
    }
}

function updateLogs(logs) {
    var logIdentity = JSON.stringify(logs);
    if (logIdentity === lastLogCount) return;
    lastLogCount = logIdentity;

    var container = document.getElementById('logContainer');
    var html = '';
    for (var i = 0; i < logs.length; i++) {
        html += '<div class="log-line">' + escapeHtml(logs[i]) + '</div>';
    }
    container.innerHTML = html || '<div class="dim">Waiting for events...</div>';
    container.scrollTop = container.scrollHeight;
}

function updateDeviceList(devices, currentIndex, changed, defaultName) {
    var sel = document.getElementById('deviceSelect');
    if (!changed && sel.dataset.devices === JSON.stringify(devices) && sel.dataset.defaultName === defaultName && currentDeviceIndex === currentIndex && sel.options.length === devices.length + 1) { sel.selectedIndex = currentIndex; updateEndpointHint('deviceSelect', 'selectedDeviceName'); return; }
    currentDeviceIndex = currentIndex;
    sel.dataset.devices = JSON.stringify(devices);
    sel.dataset.defaultName = defaultName;
    sel.innerHTML = '<option></option>';
    sel.options[0].textContent = defaultName ? 'Windows default — ' + defaultName : 'Windows default — endpoint unavailable';
    for (var i = 0; i < devices.length; i++) {
        var opt = document.createElement('option');
        opt.textContent = devices[i];
        sel.appendChild(opt);
    }
    sel.selectedIndex = currentIndex;
    updateEndpointHint('deviceSelect', 'selectedDeviceName');
}

function updateOutDeviceList(outDevices, currentIndex, changed, defaultName) {
    var sel = document.getElementById('outDeviceSelect');
    if (!changed && sel.dataset.devices === JSON.stringify(outDevices) && sel.dataset.defaultName === defaultName && currentOutDeviceIndex === currentIndex && sel.options.length === outDevices.length + 1) { sel.selectedIndex = currentIndex; updateEndpointHint('outDeviceSelect', 'selectedOutputName'); updateDriverGuidance(outDevices); return; }
    currentOutDeviceIndex = currentIndex;
    sel.dataset.devices = JSON.stringify(outDevices);
    sel.dataset.defaultName = defaultName;
    sel.innerHTML = '<option></option>';
    sel.options[0].textContent = defaultName ? 'Windows default — ' + defaultName : 'Windows default — endpoint unavailable';
    for (var i = 0; i < outDevices.length; i++) {
        var opt = document.createElement('option');
        opt.textContent = outDevices[i];
        sel.appendChild(opt);
    }
    sel.selectedIndex = currentIndex;
    updateEndpointHint('outDeviceSelect', 'selectedOutputName');
    updateDriverGuidance(outDevices);
}
function updateDriverGuidance(outDevices) {
    var select = document.getElementById('outDeviceSelect');
    var selected = select.selectedOptions.length ? select.selectedOptions[0].textContent : '';
    var cable = outDevices.some(nam)html") +
std::string(R"html(e => /CABLE Input/i.test(name));
    var nativeMic = outDevices.some(name => name === 'AudioBridge Microphone');
    var unavailable = outputUnavailable(confirmedState) || /unavailable/i.test(selected);
    document.getElementById('outputReadiness').hidden = !unavailable;
    setText('outputReadiness', 'Microphone destination unavailable. Incoming phone audio is not reaching this destination. Reconnect it or select a working destination; for calls use CABLE Input or AudioBridge Microphone.');
    document.getElementById('missingMicWarning').hidden = cable || nativeMic;
    document.getElementById('driverSetupStatus').hidden = unavailable || (!cable && !nativeMic);
    setText('driverSetupStatus', selected.includes('endpoint unavailable') ? 'The Windows default endpoint is unavailable. Select an available destination; a calling app needs CABLE Input or AudioBridge Microphone.' : /AudioBridge Microphone/i.test(selected) ? 'In your calling app, select AudioBridge Microphone as the input.' : /CABLE Input/i.test(selected) ? 'In your calling app, select CABLE Output as the microphone.' : cable ? 'This routes to PC playback, not a calling-app microphone. For calls, select CABLE Input here, then CABLE Output in your calling app.' : nativeMic ? 'This routes to PC playback, not a calling-app microphone. For calls, select AudioBridge Microphone here and in your calling app.' : 'No virtual microphone detected. Install VB-CABLE or the AudioBridge microphone driver to route into a calling app.');
}

function updatePairRequest(pr) {
    var card = document.getElementById('pairRequestCard');
    document.getElementById('reviewPairing').hidden = !pr.pending;
    if (pr.pending) {
        if (currentPairId !== pr.clientId) {
            setText('pairingOutcome', ''); clearOperationError('pairing');
            document.getElementById('pairDetails').open = !compactLayout.matches;
            setText('pairingSummaryText', 'Pairing request · ' + pr.clientName);
        }
        currentPairId = pr.clientId;
        setText('pairCode', pr.code);
        setText('pairRequestName', '"' + pr.clientName + '" wants to connect');
        setText('pairRequestId', 'ID: ' + (pr.clientId.length > 20 ? pr.clientId.substring(0, 20) + '...' : pr.clientId));
        if (card.style.display !== '') card.style.display = '';
    } else {
        if (card.contains(document.activeElement) || document.activeElement.id === 'reviewPairing') document.getElementById('statusText').focus();
        if (card.style.display !== 'none') card.style.display = 'none';
        currentPairId = '';
    }
}

// Called from C++ via eval for immediate pair request notification
function showPairRequest(clientId, name, code) {
    updatePairRequest({pending:true, clientId:clientId, clientName:name, code:code});
}

document.getElementById('manageDevices').addEventListener('click', function() {
    setText('devicesOutcome', '');
    activeWorkspace = 'routing';
    renderWorkspace(false);
    document.querySelector('.peers-panel').open = true;
    document.getElementById('pairedDevicesSummary').focus();
    document.getElementById('pairedDevicesSummary').scrollIntoView({block:'start'});
});
document.getElementById('reviewPairing').addEventListener('click', function() {
    document.getElementById('pairDetails').open = true;
    document.getElementById('pairRequestCard').scrollIntoView({block:'start'});
    document.getElementById('btnApprove').focus({preventScroll:true});
});
document.getElementById('openMicSetup').addEventListener('click', function() { document.getElementById('micSetupDialog').showModal(); });
document.getElementById('closeMicSetup').addEventListener('click', function() { document.getElementById('micSetupDialog').close(); });
document.getElementById('openCableDownload').addEventListener('click', async function() {
    this.disabled = true;
    var message = document.getElementById('setupOpenError');
    message.hidden = true;
    try {
        var result = await requestBridge(() => window._openCableSetup());
        var receipt = typeof result === 'string' ? JSON.parse(result) : result;
        if (!receipt || !receipt.applied) throw new Error('Browser could not open');
    } catch (error) {
        setText('setupOpenError', 'Open https://vb-audio.com/Cable/ in your browser for the official installer and instructions.');
        message.hidden = false;
    } finally { this.disabled = false; }
});
var pendingRemoveId = '';
function confirmPeerRemoval(peerId, peerName) {
    pendingRemoveId = peerId;
    document.getElementById('removeDeviceDescription').textContent = peerName + ' will be disconnected and must pair again before connecting.';
    document.getElementById('removeDeviceError').textContent = '';
    document.getElementById('removeDeviceDialog').showModal();
}
document.getElementById('cancelRemoveDevice').addEventListener('click', function() {
    document.getElementById('removeDeviceDialog').close();
});
var removalBusy = false, removalAwaiting = '';
function confirmRemovalState(peers) {
    if (!removalAwaiting || peers.some(peer => peer.id === removalAwaiting)) return false;
    removalAwaiting = '';
    document.getElementById('removeDeviceDialog').close();
    document.getElementById('pairedDevicesSummary').focus();
    setText('devicesOutcome', 'Pairing removed. That device must pair again to reconnect.');
    return true;
}
document.getElementById('removeDeviceDialog').addEventListener('close', function() { removalAwaiting = ''; });
document.getElementById('confirmRemoveDevice').addEventListener('click', async function() {
    if (removalBusy) return;
    removalBusy = true;
    removalAwaiting = pendingRemoveId;
    this.setAttribute('aria-disabled', 'true');
    setText('removeDeviceError', 'Removing pairing…');
    try {
        var receipt = await requestBridge(() => window._revokePeer(pendingRemoveId));
        receipt = typeof receipt === 'string' ? JSON.parse(receipt) : receipt;
        if (receipt && receipt.applied === false) { removalAwaiting = ''; throw Error('Removal rejected'); }
        var snapshot = await requestBridge(() => window._tick());
        snapshot = typeof snapshot === 'string' ? JSON.parse(snapshot) : snapshot;
        if (removalAwaiting && !confirmRemovalState(snapshot.peers)) throw Error('Removal unconfirmed');
    } catch (error) {
        setText('removeDeviceError', error.message === 'Removal rejected' ? 'Pairing could not be removed. Saved permissions are unchanged. Try again.' : 'Removal is not confirmed. The device may still have access. Wait for the device list to refresh, or retry.');
    } finally { removalBusy = false; this.setAttribute('aria-disabled', 'false'); }
});

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
var voiceRequest = 0, voiceRenderTimer, voiceRecordBusy = false;
var voiceAvailable = false, voiceSettingsPending = false, voiceEditing = false;
var confirmedVoiceSettings = '', pendingVoiceSettings = '', settingsRequest = 0;
var sampleState = 'empty';
var lastVoiceState = null;
function applyVoiceSettings(text) {
    var fields = text.split(' ').map(Number);
    document.getElementById('voiceNoise').checked = !!fields[0];
    document.getElementById('voiceStrength').value = fields[1];
    document.getElementById('voiceGate').checked = !!fields[2];
    document.getElementById('voiceThreshold').value = fields[3];
    document.getElementById('voiceHold').value = fields[4];
    document.getElementById('voiceLowCut').checked = !!fields[5];
}
function voiceLabels() {
    var threshold = document.getElementById('voiceThreshold');
    var hold = document.getElementById('voiceHold');
    document.getElementById('voiceThresholdValue').textContent = threshold.value + '%';
    document.getElementById('voiceHoldValue').textContent = (hold.value / 1000).toFixed(1) + ' s';
    threshold.setAttribute('aria-valuetext', threshold.value + ' percent');
    hold.setAttribute('aria-valuetext', (hold.value / 1000).toFixed(1) + ' seconds');
    ['voiceNoise','voiceGate','voiceLowCut'].forEach(function(id) { document.getElementById(id).disabled = !voiceAvailable; });
    document.getElementById('voiceStrength').disabled = !voiceAvailable || !document.getElementById('voiceNoise').checked;
    threshold.disabled = !voiceAvailable || !document.getElementById('voiceGate').checked;
    hold.disabled = threshold.disabled;
    ['voiceNoise','voiceStrength','voiceGate','voiceThreshold','voiceHold','voiceLowCut'].forEach(function(id) { document.getElementById(id).setAttribute('aria-disabled', String(voiceSettingsPending)); });
    renderOperationErrors();
}
function voiceChanged() {
    if (voiceSettingsPending) { applyVoiceSettings(pendingVoiceSettings); voiceLabels(); return; }
    voiceEditing = true;
    clearOperationError('settings');
    voiceLabels();
    clearTimeout(voiceRenderTimer);
    voiceRenderTimer = setTimeout(async function() {
        var request = ++settingsRequest;
        operationRequests.settings = request;
        var value = [document.getElementById('voiceNoise').checked ? 1 : 0,
            document.getElementById('voiceStrength').value, document.getElementById('voiceGate').checked ? 1 : 0,
            document.getElementById('voiceThreshold').value, document.getElementById('voiceHold').value,
            document.getElementById('voiceLowCut').checked ? 1 : 0].join(' ');
        pendingVoiceSettings = value;
        operationExpectations.settings = {request:request, matches:state => state.voice && state.voice.settings === value};
        voiceSettingsPending = true;
        voiceLabels();
        try {
            var receipt = await requestBridge(() => window._voiceSettings(value));
            receipt = typeof receipt === 'string' ? JSON.parse(receipt) : receipt;
            if (receipt && receipt.applied === false) throw Error('Not applied');
            var snapshot = await requestBridge(() => window._tick());
            snapshot = typeof snapshot === 'string' ? JSON.parse(snapshot) : snapshot;
            if (request !== settingsRequest) return;
            confirmedVoiceSettings = snapshot.voice.settings;
            if (confirmedVoiceSettings !== value) throw Error('Not confirmed');
            delete operationExpectations.settings;
            clearOperationError('settings');
        } catch (error) {
            if (request !== settingsRequest) return;
            applyVoiceSettings(confirmedVoiceSettings);
)html") +
std::string(R"html(            showOperationError('settings', error.timedOut ? 'Settings response timed out. The outcome is unknown; showing the last confirmed preferences while status refreshes.' : 'Microphone settings could not be applied. Current desktop preferences are shown. Adjust them again to retry.');
        } finally {
            if (request === settingsRequest) { voiceEditing = false; voiceSettingsPending = false; voiceLabels(); }
        }
    }, 250);
}
['voiceNoise','voiceStrength','voiceGate','voiceThreshold','voiceHold','voiceLowCut'].forEach(function(id) {
    document.getElementById(id).addEventListener('input', voiceChanged);
    document.getElementById(id).addEventListener('click', function(event) { if (voiceSettingsPending) event.preventDefault(); });
    document.getElementById(id).addEventListener('keydown', function(event) { if (voiceSettingsPending && event.key !== 'Tab') event.preventDefault(); });
});
function renderSampleAvailability() {
)html") +
std::string(R"html(    var voice = lastVoiceState;
    if (!voice) return;
    var processed = document.getElementById('processedOption');
    processed.disabled = !voice.processed;
    setText('processedOption', 'Processed');
    var message = sampleState === 'loading' ? 'Loading sample…' : sampleState === 'missing' ? 'This sample is unavailable. Reload it or record a new sample.' : sampleState === 'failed' ? 'Sample loading failed. Reload to try again.' : '';
    if (!message && voice.duration > 0 && !voice.recording && !voice.processed) message = voice.rendering ? 'Processing your sample. Original is available now.' : 'Processed sample unavailable. Listen to Original or restore voice processing.';
    document.getElementById('sampleAvailability').hidden = !message || voice.recording;
    setText('sampleAvailability', message);
    document.getElementById('retrySample').hidden = !['missing','failed'].includes(sampleState) || voice.recording;
}
async function voiceLoadClip() {
    var request = ++voiceRequest;
    sampleState = 'loading';
    renderSampleAvailability();
    clearOperationError('clip');
    clearOperationError('playback');
    var audio = document.getElementById('voiceAudio');
    var position = audio.currentTime || 0, playing = !audio.paused;
    try {
        var result = await requestBridge(() => window._voiceClip(Number(document.getElementById('voiceSource').value)));
        if (request !== voiceRequest) return;
        var url = typeof result === 'string' && result.startsWith('data:') ? result : (typeof result === 'string' ? JSON.parse(result) : result);
        audio.hidden = !url;
)html") +
std::string(R"html(        if (!url) { audio.pause(); audio.onloadedmetadata = null; audio.removeAttribute('src'); audio.load(); sampleState = lastVoiceState && lastVoiceState.duration > 0 ? 'missing' : 'empty'; renderSampleAvailability(); return; }
        sampleState = 'loading';
        renderSampleAvailability();
        audio.oncanplay = function() {
            if (request !== voiceRequest) return;
            sampleState = 'available';
            renderSampleAvailability();
        };
        audio.onerror = function() {
            if (request !== voiceRequest) return;
            sampleState = 'failed';
            audio.hidden = true;
            renderSampleAvailability();
            showOperationError('clip', 'This sample could not be decoded. Reload it, select Original, or record a new sample.');
        };
        audio.src = url;
        audio.onloadedmetadata = function() {
            audio.currentTime = Math.min(position, audio.duration || 0);
            if (playing) audio.play().catch(function() { showOperationError('playback', 'Playback could not resume. Press Play in the sample player to retry.'); });
        };
        audio.load();
        clearOperationError('clip');
    } catch (error) {
        if (request !== voiceRequest) return;
        audio.pause(); audio.hidden = true; audio.onloadedmetadata = null; audio.removeAttribute('src'); audio.load();
        sampleState = 'failed';
        renderSampleAvailability();
        showOperationError('clip', 'The sample could not be loaded. Use Reload sample to retry.');
    }
}
document.getElementById('voiceSource').addEventListener('change', function() { voiceLoadClip(); });
document.getElementById('retrySample').addEventListener('click', function() { voiceLoadClip(); });
document.getElementById('voiceRecord').)html") +
std::string(R"html(addEventListener('click', async function() {
    if (voiceRecordBusy) return;
    var button = this;
    clearOperationError('recording');
    voiceRecordBusy = true; button.setAttribute('aria-disabled', 'true'); ++voiceRequest;
    var audio = document.getElementById('voiceAudio');
    audio.pause(); audio.onloadedmetadata = null;
    try {
        var expectedRecording = !voiceRecording;
        var request = operationRequests.recording = (operationRequests.recording || 0) + 1;
        var startingRevision = lastVoiceState ? lastVoiceState.revision : -1;
        operationExpectations.recording = {request:request, matches:state => state.voice && (state.voice.recording === expectedRecording || (expectedRecording && state.voice.revision > startingRevision && state.voice.duration > 0))};
        var receipt = await requestBridge(() => window._voiceRecord(expectedRecording ? 1 : 0));
        receipt = typeof receipt === 'string' ? JSON.parse(receipt) : receipt;
        if (receipt && receipt.applied === false) throw Error('Not applied');
        var snapshot = await requestBridge(() => window._tick());
        snapshot = typeof snapshot === 'string' ? JSON.parse(snapshot) : snapshot;
        if (!snapshot.voice || snapshot.voice.recording !== expectedRecording) throw Error('Recording not confirmed');
        updateVoice(snapshot);
        delete operationExpectations.recording;
        clearOperationError('recording');
    } catch (error) {
        showOperationError('recording', error.timedOut ? 'Recording response timed out. The outcome is unknown; wait for refreshed recording status before retrying.' : 'Recording could not start or stop. Current desktop state is shown. Check the microphone and retry.');
    } finally { voiceRecordBusy = false; button.setAttribute('aria-disabled', String(button.disabled || bridgeFailed)); }
});
document.getElementById('persistentStop').addEventListener('click', function() { document.getElementById('voiceRecord').click(); });
function updateVoice(s) {
    var voice = s.voice;
    if (!voice || !voice.settings) return;
    lastVoiceState = voice;
    if (!voiceInitialized || (!voiceEditing && !voiceSettingsPending)) {
        confirmedVoiceSettings = voice.settings;
        applyVoiceSettings(voice.settings);
        voiceInitialized = true;
    }
    voiceAvailable = !!voice.ready && !voice.error;
    voiceLabels();
    document.getElementById('voiceCard').setAttribute('data-unavailable', String(!voiceAvailable));
    document.getElementById('effectsUnavailable').hidden = !voice.error;
    document.getElementById('processingAvailability').hidden = voice.ready || !!voice.error;
    var recordingEnded = voiceRecording && !voice.recording;
    var restoreRecordingFocus = recordingEnded && document.getElementById('recordingStrip').contains(document.activeElement);
    voiceRecording = voice.recording;
    var audio = document.getElementById('voiceAudio');
    audio.controls = !voice.recording;
    if (voice.recording) { audio.pause(); audio.hidden = true; }
    var button = document.getElementById('voiceRecord');
    button.textContent = voice.recording ? 'Stop recording' : 'Record sample';
    button.setAttribute('aria-pressed', String(voice.recording));
    button.disabled = !voice.recording && (!s.connected || !s.micOwner);
    button.setAttribute('aria-disabled', String(voiceRecordBusy || button.disabled));
    document.getElementById('recordingStrip').hidden = !voice.recording;
    document.body.setAttribute('data-recording', String(voice.recording));
    document.getElementById('persistentStop').disabled = false;
    document.getElementById('persistentStop').setAttribute('aria-disabled', String(voiceRecordBusy));
    if (restoreRecordingFocus) {
        if (compactLayout.matches && activeWorkspace !== 'microphone') { activeWorkspace = 'microphone'; renderWorkspace(false); }
        button.focus(); button.scrollIntoView({block:'nearest'});
    }
    setText('recordingStripTitle', 'Recording phone microphone · ' + voice.duration.toFixed(1) + ' s');
    setText('recordingStripStatus', operationErrors.recording || 'Recording stops at 10 seconds.');
    var readiness = !s.connected ? 'Connect your phone to test its microphone.' : !s.micOwner ? 'No phone microphone assigned. Enable the mic in your phone app to begin.' : 'Mic assigned: ' + s.micOwner + '. ' + (outputUnavailable(s) ? 'Destination unavailable. Incoming audio can still be recorded here; choose a working return destination in Audio routing.' : 'Check Mic received for incoming audio; verify delivery in your calling app.');
    setText('micReadiness', readiness);
    setText('routeMicStatus', !s.connected || !s.micOwner ? readiness : 'Mic assigned: ' + s.micOwner + '. The meter shows reception, not calling-app delivery.');
    var sampleSource = document.getElementById('voiceSource');
    var switchToOriginal = !voice.recording && voice.duration > 0 && !voice.processed && sampleSource.value === '1';
    if (switchToOriginal) sampleSource.value = '0';
    sampleSource.disabled = voice.recording || voice.duration === 0;
    if (voice.recording) sampleState = 'empty';
    renderSampleAvailability();
    document.getElementById('voiceTime').textContent = voice.duration > 0 || voice.recording ? voice.duration.toFixed(1) + ' / 10 s' : '10-second mic check';
    document.getElementById('voiceStatus').setAttribute('data-error', String(!!voice.error));
    setText('voiceStatus', voice.error ? 'Effects unavailable; incoming microphone audio passes through unprocessed. ' + voice.error :
        (voice.ready ? (document.getElementById('voiceNoise').checked ? 'Noise removal adds 32 ms model delay, plus buffering.' : 'Noise removal off. Gate and rumble controls still apply.') : 'Loading voice models. Preferences are inactive until ready.'));
    var preview = document.getElementById('voicePreviewStatus');
    preview.setAttribute('data-error', String(!!voice.previewError));
    setText('voicePreviewStatus', voice.previewError ? 'Could not process recording: ' + voice.previewError :
        (voice.recording ? 'Recording your phone mic. Speak normally; recording stops at 10 seconds.' :
        (voice.rendering ? 'Applying your effects to the sample…' : (voice.duration > 0 ? (voice.processed && sampleState === 'available' ? 'Recording complete. Switch versions to compare. Playback uses your PC’s default output.' : 'Playback uses your PC’s default output. Your original sample is preserved.') :
        (s.connected ? 'Record a sample, then compare Original and Processed. Samples stay in memory until you close AudioBridge.' : 'Connect your phone and enable its mic, then record a sample to compare.')))));
    if ((voiceRevision !== voice.revision || switchToOriginal) && !voice.recording) { voiceRevision = voice.revision; voiceLoadClip(); }
}
function reconcileControls(s) {
    if (s.captureMode) {
        confirmedCaptureMode = s.captureMode;
        document.getElementById('captureModeSelect').value = s.captureMode;
    }
    document.getElementById('captureStatus').hidden = s.captureReady !== false;
    setText('captureStatus', 'PC audio capture is unavailable. Select a working device or change Capture mode.');
    updateDeviceList(s.devices || [], s.currentDeviceIndex || 0, s.devicesChanged, s.defaultDeviceName || '');
    updateOutDeviceList(s.outDevices || [], s.currentOutDeviceIndex || 0, s.outDevicesChanged, s.defaultOutDeviceName || '');
    var slider = document.getElementById('jitterSlider');
    slider.value = s.jitterBufferMs;
    setText('jitterValue', s.jitterBufferMs + ' ms');
    slider.setAttribute('aria-valuetext', s.jitterBufferMs + ' milliseconds');
    document.querySelectorAll('.frame-btn').forEach(function(button) {
        var selected = Number(button.getAttribute('data-ms')) === s.frameSizeMs;
        button.classList.toggle('active', selected);
        button.setAttribute('aria-pressed', String(selected));
    });
    setText('frameSizeValue', frameLabel(s.frameSizeMs));
    document.querySelectorAll('.codec-btn').forEach(function(button) {
        var selected = (button.getAttribute('data-codec') === 'pcm') === s.rawPcm;
        button.classList.toggle('active', selected);
        button.setAttribute('aria-pressed', String(selected));
    });
    setText('codecValue', s.rawPcm ? 'Raw PCM' : 'Opus');
}
async function tick() {
    if (tickBusy) return;
    tickBusy = true;
    try {
        var result = await requestBridge(() => window._tick());
        var s = (typeof result === 'string') ? JSON.parse(result) : result;
        confirmedState = s;
        bridgeFailed = false;
        setText('effectsUnavailableTitle', 'Effects unavailable · preferences inactive');
        setText('effectsUnavailableDescription', 'Saved preferences below are inactive. Unprocessed audio is used when the phone microphone is streaming.');
        document.getElementById('bridgeNotice').hidden = true;
        document.getElementById('workspaceControls').disabled = false;
        document.getElementById('btnApprove').disabled = false;
        document.getElementById('btnDeny').disabled = false;
        document.getElementById('sessionOverview').hidden = false;
        document.getElementById('streamSummary').textContent = (s.rawPcm ? 'PCM' : 'Opus') + ' · ' + frameLabel(s.frameSizeMs);
        reconcileOperationOutcomes(s);
        renderAppUpdate(s.update);
        updateStatusCard(s);
        updateVoice(s);
        updatePairRequest(s.pairRequest);
        updatePeers(s.peers);
        updateLogs(s.logs);
        if (!operationPending.routing && !operationPending.output && !operationPending.transport) reconcileControls(s);
    } catch(error) {
        bridgeFailed = true;
        document.getElementById('persistentStop').disabled = true;
        document.getElementById('voiceRecord').disabled = true;
        document.getElementById('voiceRecord').setAttribute('aria-disabled', 'true');
        setText('recordingStripStatus', 'Desktop unavailable. Recording state is unknown; retry desktop controls.');
        setText('effectsUnavailableTitle', 'Last-known processing status');
        setText('effectsUnavailableDescription', 'Desktop controls are disconnected. Current processing and microphone delivery cannot be confirmed.');
        setText('voiceStatus', 'Current microphone processing status is unavailable.');
        document.getElementById('processingAvailability').hidden = true;
        setText('micReadiness', 'Microphone status is unknown until desktop controls reconnect.');
)html") +
std::string(R"html(        setText('routeMicStatus', 'Microphone status is unknown until desktop controls reconnect.');
        document.getElementById('driverSetupStatus').hidden = true;
        document.getElementById('outputReadiness').hidden = false;
        setText('outputReadiness', 'Current destination readiness is unknown until desktop controls reconnect.');
        setText('voicePreviewStatus', 'Recording status is unavailable until desktop controls reconnect.');
        document.getElementById('bridgeNotice').hidden = false;
        document.getElementById('workspaceControls').disabled = true;
        document.getElementById('btnApprove').disabled = true;
        document.getElementById('btnDeny').disabled = true;
        document.getElementById('operationNotice').hidden = true;
        document.getElementById('pairRequestCard').style.display = 'none';
        document.getElementById('reviewPairing').hidden = true;
        document.getElementById('sessionOverview').hidden = true;
        setText('statusText', 'Status unavailable');
        document.getElementById('statusDot').className = 'status-dot yellow';
    } finally { tickBusy = false; }
}

)html") +
std::string(R"html(var appUpdateState = {};
var appUpdateError = '';
function renderAppUpdate(state) {
    if (!state) return;
    if (state.phase !== appUpdateState.phase) appUpdateError = '';
    appUpdateState = state;
    var busy = ['checking', 'downloading', 'restarting'].includes(state.phase);
    setText('installedVersion', 'Installed: ' + state.currentVersion + (state.version ? ' · Latest release: ' + state.version : ''));
    setText('updateMessage', appUpdateError || state.message);
    document.getElementById('checkAppUpdates').disabled = busy;
    var install = document.getElementById('installAppUpdate');
    install.hidden = state.phase !== 'available';
    install.disabled = busy;
    var progress = document.getElementById('updateProgress');
    progress.hidden = state.phase !== 'downloading';
    progress.value = Math.min(1, Math.max(0, state.progress || 0));
}
function requestAppUpdate(action) {
    appUpdateError = '';
    var origin = document.activeElement;
    requestBridge(action).then(function() {
        if (document.activeElement === origin || document.activeElement === document.body) document.getElementById('updateMessage').focus();
    }).catch(function() {
        appUpdateError = 'Update request was not confirmed. Check again or retry; installed files are unchanged until a verified update is applied.';
        setText('updateMessage', appUpdateError);
    });
}
document.getElementById('openAppUpdates').addEventListener('click', function() {
    document.getElementById('appUpdatesDialog').showModal();
    document.getElementById('checkAppUpdates').focus();
});
document.getElementById('closeAppUpdates').addEventListener('click', function() { document.getElementById('appUpdatesDialog').close(); });
document.getElementById('checkAppUpdates').addEventListener('click', function() { requestAppUpdate(() => window._checkForUpdates()); });
document.getElementById('installAppUpdate').addEventListener('click', function() { document.getElementById('confirmAppUpdateDialog').showModal(); });
document.getElementById('cancelAppUpdate').addEventListener('click', function() { document.getElementById('confirmAppUpdateDialog').close(); });
document.getElementById('confirmAppUpdate').addEventListener('click', function() {
    document.getElementById('confirmAppUpdateDialog').close();
    requestAppUpdate(() => window._installUpdate());
});
setInterval(tick, 50);
</script>
</body>
</html>
)html");

} // namespace gui_html
