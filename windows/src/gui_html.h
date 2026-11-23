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
:root {
  color-scheme: dark;
  --bg: #0a0e0d;
  --card: #131a17;
  --raised: #18211d;
  --inset: #0d1311;
  --border: rgba(255, 255, 255, .07);
  --border-soft: rgba(255, 255, 255, .045);
  --border-strong: #32423a;
  --text: #eef3ef;
  --dim: #9baaa2;
  --label: #bac8c0;
  --accent: #9ce6be;
  --accent-strong: #b8f0d1;
  --accent-bg: #172a20;
  --info: #93d0ec;
  --info-bg: #13242d;
  --gpu: #cbbef7;
  --gpu-bg: #1f1b2e;
  --green: #9ce6be;
  --red: #ff8c82;
  --yellow: #f0d188;
  --orange: #f2b488;
  --mono: 'Cascadia Code', 'Consolas', monospace;
}
* { box-sizing: border-box; }
html { height: 100%; overflow: hidden; }
body {
  margin: 0;
  height: 100vh;
  overflow-y: auto;
  scroll-padding-top: 72px;
  background: var(--bg);
  color: var(--text);
  font: 14px/1.5 'Segoe UI', system-ui, sans-serif;
}
body[data-recording=true] { height: calc(100vh - 76px); }
button, input, select { font: inherit; }
button, select, summary { cursor: pointer; }
button, select, input { accent-color: var(--accent); }
button { color: inherit; }
button:disabled, select:disabled, input:disabled { cursor: not-allowed; opacity: .48; }
[aria-disabled=true] { opacity: .65; cursor: wait; }
:focus-visible { outline: 2px solid rgba(156, 230, 190, .85); outline-offset: 2px; }
[hidden] { display: none !important; }
h1, h2, h3, p { margin: 0; }
h1 { font-size: 20px; font-weight: 650; letter-spacing: -.4px; line-height: 1.2; }
h2 { font-size: 16px; font-weight: 650; letter-spacing: -.2px; }
h3 { font-size: 14px; font-weight: 600; }
svg.icon { width: 20px; height: 20px; fill: none; stroke: currentColor; stroke-width: 1.7; stroke-linecap: round; stroke-linejoin: round; flex-shrink: 0; }
.symbols { position: absolute; width: 0; height: 0; overflow: hidden; }

/* Header */
.app-header {
  position: sticky; top: 0; z-index: 12;
  height: 54px;
  border-bottom: 1px solid var(--border);
  background: rgba(10, 14, 13, .94);
  backdrop-filter: blur(8px);
}
.app-header-inner {
  max-width: 980px; height: 100%; margin: 0 auto; padding: 0 24px;
  display: flex; align-items: center; justify-content: space-between; gap: 14px;
}
.brand { display: flex; align-items: center; gap: 10px; font-size: 16px; font-weight: 650; letter-spacing: -.3px; }
.brand-mark { display: grid; place-items: center; width: 30px; height: 30px; color: var(--accent); background: var(--accent-bg); border: 1px solid #2c4b3b; border-radius: 9px; }
.brand-mark .icon { width: 18px; height: 18px; }
.app-label, .eyebrow { text-transform: uppercase; letter-spacing: 1.3px; font-size: 11px; font-weight: 650; color: var(--dim); }
.header-note { display: flex; align-items: center; gap: 7px; color: var(--dim); font-size: 12px; margin-left: auto; }
.app-header:has(.pairing-shortcut:not([hidden])) .header-note { display: none; }
#manageDevices { font-size: 12.5px; font-weight: 500; white-space: nowrap; min-height: 32px; padding: 5px 11px; background: transparent; border-color: var(--border); color: var(--label); }
#manageDevices:hover:not(:disabled) { background: var(--raised); border-color: var(--border-strong); color: var(--text); }
.pairing-shortcut { color: var(--yellow); border-color: #8f7b48; background: #262216; font-size: 12.5px; min-height: 32px; padding: 5px 11px; }

/* Workspace shell */
.workspace { max-width: 980px; margin: 0 auto; padding: 14px 24px 20px; }
.workspace-heading { display: flex; align-items: center; justify-content: space-between; gap: 14px; margin-bottom: 12px; }
.subtitle { display: none; }
.pairing-outcome {
  color: var(--accent); margin: 0 auto 0 0; font-size: 12.5px; font-weight: 500;
}
.pairing-outcome::before { content: '✓ '; }
.pairing-outcome:empty { display: none; }
.status-pill {
  display: inline-flex; align-items: center; gap: 8px;
  border: 1px solid var(--border); background: var(--card);
  padding: 6px 12px; border-radius: 999px; white-space: nowrap; font-size: 13px;
}
.status-dot { width: 7px; height: 7px; border-radius: 50%; background: var(--dim); flex-shrink: 0; }
.status-dot.green { background: var(--green); box-shadow: 0 0 0 4px #9ce6be1a; }
.status-dot.yellow { background: var(--yellow); box-shadow: 0 0 0 4px #f0d1881a; }

/* Alerts & Pairing */
.pair-request {
  border: 1px solid #96814c; border-radius: 12px; background: #242117;
  padding: 12px 18px; margin-bottom: 14px;
}
#pairDetails > summary { min-height: 34px; padding: 0; font-size: 14px; color: var(--yellow); }
.pair-content { display: flex; align-items: center; justify-content: space-between; gap: 24px; padding-top: 12px; }
.pair-content > div:first-child { flex: 1; }
.pair-request .eyebrow { color: var(--yellow); }
.pair-name { font-weight: 600; margin: 4px 0; overflow-wrap: anywhere; }
.pair-id { font: 12px var(--mono); color: var(--dim); overflow-wrap: anywhere; }
.pair-content p { font-size: 13px; color: var(--dim); margin-top: 6px; }
.pair-consent { margin-top: 8px; color: #e2d7b8; }
.pair-code { font: 26px var(--mono); letter-spacing: 5px; color: var(--yellow); white-space: nowrap; }
.notice {
  display: flex; justify-content: space-between; align-items: center; gap: 16px;
  padding: 13px 16px; margin-bottom: 14px; border-radius: 12px;
  border: 1px solid #96814c; background: #242117;
}
.notice h2 { font-size: 14px; letter-spacing: 0; }
.notice p { font-size: 13px; margin-top: 3px; }
.notice .btn { flex-shrink: 0; }
.notice.error { background: #2d1e1b; border-color: #8f5e55; color: #ffd2ca; }
.operation-links { display: flex; gap: 8px; flex-wrap: wrap; margin-top: 8px; }
.task-link { color: var(--text); background: #201513; border: 1px solid #8f5e55; border-radius: 6px; padding: 4px 10px; font-size: 13px; min-height: 32px; }

/* Session Telemetry Strip */
.session {
  border: 1px solid var(--border); border-radius: 14px;
  background: linear-gradient(180deg, #161f1c 0%, #121816 100%);
  margin-bottom: 14px;
}
.session-connected {
  display: grid; grid-template-columns: 1.15fr 1fr auto;
  gap: 0; padding: 13px 18px; align-items: center;
}
.session-identity { min-width: 0; padding-right: 18px; }
.session-name { display: flex; align-items: center; gap: 9px; }
.session-name .icon { color: var(--accent); width: 17px; height: 17px; }
#clientInfo { font-size: 15px; font-weight: 650; overflow-wrap: anywhere; }
.session-meta { display: flex; flex-wrap: wrap; gap: 4px 10px; margin-top: 3px; font-size: 12px; color: var(--dim); }
.encryption-badge { font-size: 11px; letter-spacing: .5px; font-weight: 650; color: var(--accent); }
.encryption-badge.unencrypted { color: var(--yellow); }
.session-meters { padding: 0 18px; border-left: 1px solid var(--border); border-right: 1px solid var(--border); }
.level-row { display: grid; grid-template-columns: 78px 1fr; align-items: center; gap: 10px; margin: 5px 0; }
.level-label { font-size: 12px; color: #b4c2ba; }
.level-bar {
  position: relative; height: 7px; border-radius: 3px; background: #232f29; overflow: hidden;
}
.level-bar::after {
  content: ''; position: absolute; inset: 0; pointer-events: none;
  background: repeating-linear-gradient(90deg, transparent 0 10px, rgba(10,14,13,.78) 10px 12px);
}
.level-fill {
  height: 100%; width: 100%; border-radius: 2px;
  clip-path: inset(0 calc(100% - var(--lvl, 0%)) 0 0);
  transition: clip-path .1s;
  background: linear-gradient(90deg, #4db883 0%, #7adba9 66%, #ebb85e 80%, #f07b70 93%);
}
.session-metrics {
  display: grid; grid-template-columns: repeat(3, minmax(76px, auto)); gap: 16px;
  padding-left: 18px;
  font-variant-numeric: tabular-nums;
}
.metric-value { font: 14px/1.25 var(--mono); font-weight: 600; letter-spacing: -.2px; white-space: nowrap; display: block; }
.metric-value[data-empty=true] { color: #8a9991; font-weight: 400; }
.metric-label { display: block; font-size: 11.5px; color: var(--dim); margin-top: 3px; white-space: nowrap; }
.session-empty { display: flex; gap: 14px; align-items: center; padding: 16px 18px; }
.empty-icon { display: grid; place-items: center; width: 40px; height: 40px; flex-shrink: 0; border: 1px dashed #475b50; border-radius: 10px; color: var(--accent); }
.session-empty h2 { font-size: 15px; }
.session-empty p { font-size: 13px; color: var(--dim); margin-top: 2px; }
.server-info { color: var(--accent); font: 12px var(--mono); margin-top: 5px; overflow-wrap: anywhere; }
.discovery-help summary { padding: 6px 0 0; min-height: 28px; justify-content: flex-start; gap: 8px; color: var(--accent); font-size: 13px; }
.discovery-help summary::after { margin-left: 4px; }
.discovery-help ol { padding-left: 18px; font-size: 13px; color: var(--dim); margin: 6px 0 0; }
.discovery-help li + li { margin-top: 4px; }

/* Compact Workspace Tabs (<= 1000px only) — shared segmented primitive */
.workspace-tabs {
  display: flex; gap: 3px; border: 1px solid var(--border);
  border-radius: 9px; background: var(--inset); padding: 3px; margin-bottom: 12px;
}
.workspace-tab {
  flex: 1; min-height: 36px; border: 1px solid transparent; border-radius: 6px;
  color: var(--dim); background: transparent; font-weight: 600; font-size: 13px;
}
.workspace-tab[aria-selected=true] { background: #21362c; border-color: rgba(156, 230, 190, .3); color: #ebf9f1; }

/* 3-Section Accordion Deck */
.control-fieldset { margin: 0; padding: 0; border: 0; min-width: 0; }
.deck { display: flex; flex-direction: column; gap: 12px; }
.major-section {
  border: 1px solid var(--border);
  border-radius: 14px;
  background: var(--card);
  overflow: hidden;
  transition: border-color .15s;
}
.major-section[open] { border-color: var(--border-strong); }
.major-summary {
  min-height: 60px;
  padding: 13px 18px;
  display: flex; align-items: center; justify-content: space-between; gap: 14px;
  list-style: none;
  background: var(--card);
  transition: background .15s;
}
.major-summary:hover { background: var(--raised); }
.major-section[open] > .major-summary {
  background: #151d1a;
  border-bottom: 1px solid var(--border);
}
.major-summary::-webkit-details-marker { display: none; }
.major-summary::after {
  content: '';
  width: 7px; height: 7px;
  border-right: 1.75px solid var(--dim);
  border-bottom: 1.75px solid var(--dim);
  transform: rotate(45deg);
  transition: transform .15s, border-color .15s;
  margin-right: 4px;
  flex-shrink: 0;
}
.major-section[open] > .major-summary::after { transform: rotate(-135deg); border-color: var(--text); margin-top: 4px; }
.major-lead { display: flex; align-items: center; gap: 12px; min-width: 0; flex: 1; }
.major-num {
  width: 28px; height: 28px; border-radius: 8px;
  background: var(--inset); border: 1px solid var(--border);
  display: grid; place-items: center;
  font: 12px/1 var(--mono); font-weight: 600; color: var(--dim);
  flex-shrink: 0;
}
.major-section[open] > .major-summary .major-num {
  color: var(--text);
  border-color: var(--border-strong);
  background: var(--raised);
)html") +
std::string(R"html(}
.major-heading-wrap { min-width: 0; flex: 1; }
.major-title-row { display: flex; align-items: center; gap: 8px; }
.major-title-row h2 { font-size: 16px; font-weight: 650; line-height: 1.25; }
.major-live-summary {
  display: block;
  margin-top: 2px;
  font: 12px/1.35 var(--mono);
  color: var(--dim);
  white-space: nowrap; overflow: hidden; text-overflow: ellipsis;
}
.major-body { padding: 16px 18px 18px; }
.section-badge {
  display: inline-flex; align-items: center; gap: 6px;
  padding: 4px 10px; border-radius: 999px;
  font-size: 12px; font-weight: 600;
  border: 1px solid var(--border);
  background: var(--inset); color: var(--dim);
  white-space: nowrap; flex-shrink: 0;
}
.section-badge::before { content: ''; width: 6px; height: 6px; border-radius: 50%; background: currentColor; flex-shrink: 0; }
.section-badge[data-kind=meta] { color: #c4d1ca; border-color: var(--border); background: var(--inset); padding: 4px 11px; }
.section-badge[data-kind=meta]::before { background: #6b7d74; }
.section-badge[data-kind=cpu] { color: #bdd7e6; border-color: #263a45; background: #131e24; }
.section-badge[data-kind=cpu]::before { background: #7dbcd9; }
.section-badge[data-kind=gpu] { color: #d1c7eb; border-color: #342d47; background: #1a1724; }
.section-badge[data-kind=gpu]::before { background: #b4a5e3; }
.section-badge[data-kind=live] { color: var(--accent); border-color: #2f5240; background: var(--accent-bg); }
.section-badge[data-kind=warn] { color: var(--yellow); border-color: #7d6938; background: #292416; }

/* Open two-column panes (no concentric box-in-box borders) */
.io-grid, .proc-grid { display: grid; grid-template-columns: 1fr 1fr; gap: 0; align-items: stretch; }
.subcard {
  padding: 2px 18px 2px 0;
  min-width: 0;
}
.subcard + .subcard {
  padding: 2px 0 2px 18px;
  border-left: 1px solid rgba(255, 255, 255, .08);
}
.subcard-head {
  display: flex; align-items: center; justify-content: space-between; gap: 8px;
  margin-bottom: 10px;
}
.subcard-kicker {
  display: inline-flex; align-items: center; gap: 7px;
  font-size: 11px; font-weight: 700; letter-spacing: 1px; text-transform: uppercase;
  color: var(--dim);
}
.subcard-kicker .icon { width: 14px; height: 14px; color: var(--accent); }
.subcard-label { display: block; font-size: 12.5px; font-weight: 500; color: var(--label); margin-bottom: 5px; }
select {
  width: 100%; min-width: 0; min-height: 36px;
  border: 1px solid var(--border-strong); border-radius: 8px;
  background: var(--inset); color: var(--text);
  padding: 6px 28px 6px 11px; text-overflow: ellipsis;
  transition: border-color .15s, color .15s;
}
select:disabled {
  opacity: .56; color: #9aa9a1; border: 1px solid rgba(255, 255, 255, .07); background: #0c100e;
}
select[data-inactive=true] {
  opacity: 1; color: #8c9b93; border: 1px solid rgba(255, 255, 255, .065); background: #0c100e;
}
.sample-version:has(select:disabled) label { color: #7e8e86; }
.route-mode { margin-top: 10px; display: block; }
.route-mode label { display: block; color: var(--label); font-size: 12.5px; font-weight: 500; margin-bottom: 5px; }
.route-mode select { font-size: 13px; min-height: 36px; }
.route-note, .pc-mic-note, .endpoint-name { color: var(--dim); font-size: 12.5px; margin-top: 7px; overflow-wrap: anywhere; }
.setup-status {
  padding: 6px 10px; background: rgba(156, 230, 190, .05);
  border: 0; border-left: 2px solid #49946d;
  border-radius: 0 6px 6px 0; margin: 11px 0 0; color: #d5e4dc; font-size: 12.5px;
}
.pc-mic {
  margin-top: 14px; padding-top: 14px; border-top: 1px solid rgba(255, 255, 255, .07);
  display: grid; grid-template-columns: 1fr 1fr; gap: 0 36px; align-items: start;
}
.pc-mic-toggle-col .effect { padding: 0; border-top: 0; }
.pc-mic-select-col label { display: block; font-size: 12.5px; font-weight: 500; color: var(--label); margin-bottom: 5px; }
.pc-mic-note { margin-top: 4px; }
.stream-top-grid { display: grid; grid-template-columns: 1fr 1.35fr; gap: 18px; align-items: start; }
.stream-top-grid .control-group { margin-top: 0; }

/* Unified nested disclosure groups (distinct from dropdown selects via left disclosure triangle) */
.drawer-stack {
  display: flex; flex-direction: column; margin-top: 14px;
  border: 1px solid var(--border); border-radius: 10px;
  background: var(--inset); overflow: hidden;
}
details.subdrawer, details.stream-panel, details.voice-drawer, details.tuning {
  border: 0; border-radius: 0; background: transparent; overflow: hidden;
}
.drawer-stack > details + details { border-top: 1px solid rgba(255, 255, 255, .055); }
details.subdrawer > summary, details.stream-panel > summary, details.voice-drawer > summary, details.tuning > summary {
  min-height: 40px; padding: 9px 14px;
  display: flex; align-items: center; gap: 9px;
  list-style: none; font-size: 13px; font-weight: 600;
  transition: background .15s;
}
details.subdrawer > summary:hover, details.stream-panel > summary:hover, details.voice-drawer > summary:hover, details.tuning > summary:hover { background: rgba(255, 255, 255, .03); }
details.subdrawer > summary::-webkit-details-marker,
details.stream-panel > summary::-webkit-details-marker,
details.voice-drawer > summary::-webkit-details-marker,
details.tuning > summary::-webkit-details-marker,
details.diagnostics > summary::-webkit-details-marker { display: none; }
details.subdrawer > summary::before,
details.stream-panel > summary::before,
details.voice-drawer > summary::before,
details.tuning > summary::before {
  content: '';
  width: 5.5px; height: 5.5px;
  border-right: 1.6px solid var(--dim);
  border-bottom: 1.6px solid var(--dim);
  transform: rotate(-45deg);
  transition: transform .15s, border-color .15s;
  flex-shrink: 0;
}
details.subdrawer[open] > summary::before,
details.stream-panel[open] > summary::before,
details.voice-drawer[open] > summary::before,
details.tuning[open] > summary::before { transform: rotate(45deg); border-color: var(--accent); margin-bottom: 2px; }
details.diagnostics > summary::after {
  content: '';
  width: 6px; height: 6px;
  border-right: 1.6px solid var(--dim);
  border-bottom: 1.6px solid var(--dim);
  transform: rotate(45deg);
  transition: transform .15s, border-color .15s;
  margin-right: 4px;
  flex-shrink: 0;
}
details.diagnostics[open] > summary::after { transform: rotate(-135deg); border-color: var(--text); margin-top: 3px; }
.summary-info, .drawer-summary-val {
  margin-left: auto; color: var(--dim);
  font: 12px var(--mono); font-weight: 400;
  white-space: nowrap; overflow: hidden; text-overflow: ellipsis; max-width: 58%;
}
.details-content { padding: 6px 14px 14px 28px; }
.help-copy { color: var(--dim); font-size: 13px; }
.help-copy ol { margin: 0; padding-left: 18px; }
.help-copy li + li { margin-top: 6px; }
.help-copy p { margin-top: 10px; }
.help-copy strong { color: var(--text); font-weight: 600; }

/* Controls & switches */
.control-group + .control-group, .stream-top-grid + .control-group { margin-top: 16px; }
.control-heading { display: flex; align-items: center; justify-content: space-between; margin-bottom: 7px; gap: 10px; font-size: 13px; }
.control-value { color: var(--accent); font: 12px var(--mono); }
.segmented { display: flex; border: 1px solid var(--border); border-radius: 9px; padding: 3px; gap: 3px; background: var(--card); }
.segmented .btn { flex: 1; min-width: 0; border: 1px solid transparent; background: transparent; padding: 5px 4px; min-height: 32px; color: var(--dim); font-size: 12px; border-radius: 6px; }
.segmented .btn.active { background: #21362c; border-color: rgba(156, 230, 190, .3); color: #ebf9f1; }
input[type=range] { width: 100%; height: 22px; margin: 0; background: transparent; }
.range-caption { display: flex; justify-content: space-between; font-size: 12px; color: var(--dim); }
.control-note { font-size: 13px; color: var(--dim); margin-top: 5px; }

.effect { display: flex; align-items: center; justify-content: space-between; gap: 14px; padding: 12px 0; border-top: 1px solid var(--border); }
.effect:first-child { border-top: 0; padding-top: 0; }
.effect-name { display: block; font-size: 14px; font-weight: 600; cursor: pointer; }
.effect-description { font-size: 13px; color: var(--dim); margin-top: 2px; }
.switch {
  appearance: none; -webkit-appearance: none; position: relative; display: block;
  width: 40px; height: 22px; min-width: 40px;
  border: 1px solid #5c6e64; border-radius: 99px; background: #2c3832;
  margin: 0; cursor: pointer; transition: background .15s, border-color .15s;
}
.switch::before {
  content: ''; position: absolute; width: 14px; height: 14px; border-radius: 50%;
  background: #c8d4cd; top: 3px; left: 3px; transition: transform .15s;
}
.switch:checked { background: #2e7f5a; border-color: #3e9970; }
.switch:checked::before { background: #f0f6f2; transform: translateX(18px); }
.switch:focus-visible { outline: none; box-shadow: 0 0 0 2px var(--card), 0 0 0 4px rgba(156, 230, 190, .75); }
.strength-row { display: flex; align-items: center; justify-content: space-between; gap: 12px; padding: 2px 0 12px; }
.strength-row label { color: var(--label); font-size: 12.5px; font-weight: 500; }
.strength-row select { width: 176px; min-height: 34px; font-size: 13px; padding: 4px 26px 4px 10px; }

/* Buttons & Sample preview */
.btn {
  min-height: 36px; border: 1px solid var(--border-strong); border-radius: 8px;
  padding: 6px 13px; font-weight: 600; background: #1c2622; font-size: 13px;
  transition: background .15s, border-color .15s;
}
.btn:hover:not(:disabled) { background: #26342d; border-color: #5b7367; }
.btn.primary { color: #0e2419; border-color: var(--accent); background: var(--accent); }
.btn.primary:hover:not(:disabled) { background: var(--accent-strong); }
.btn.danger { color: var(--red); border-color: #7c4f48; background: #261816; }
.btn.voice-btn {
  display: inline-flex; align-items: center; gap: 8px; white-space: nowrap;
  min-height: 34px; padding: 5px 12px;
  background: #1c2421; border-color: #3a4a43; color: var(--text);
}
.btn.voice-btn:hover:not(:disabled) { background: #252e2b; border-color: #7d5350; }
.btn.voice-btn::before { content: ''; width: 8px; height: 8px; border-radius: 50%; background: #ff7a70; flex-shrink: 0; }
.btn.voice-btn[aria-pressed=true], .btn.voice-btn[aria-pressed=true]:hover:not(:disabled) { color: #2e1613; background: #ffb5a8; border-color: #ffb5a8; }
.btn.voice-btn[aria-pressed=true]::before { background: currentColor; border-radius: 2px; }

.preview-heading { display: flex; align-items: center; justify-content: space-between; gap: 10px; margin-bottom: 8px; }
.preview-heading span { color: var(--dim); font: 12px var(--mono); }
.preview-actions { display: flex; justify-content: space-between; align-items: center; gap: 10px; flex-wrap: wrap; }
.sample-version { display: flex; align-items: center; gap: 8px; color: var(--label); font-size: 12.5px; font-weight: 500; }
.sample-version select { width: 126px; min-height: 34px; font-size: 13px; }
)html") +
std::string(R"html(.sr-audio { position: fixed; top: 0; left: 0; width: 1px; height: 1px; opacity: 0; overflow: hidden; clip-path: inset(50%); border: 0; pointer-events: none; }
.studio-player {
  display: flex; align-items: center; gap: 10px;
  margin-top: 10px; padding: 6px 10px;
  border: 1px solid var(--border); border-radius: 9px; background: var(--inset);
}
.player-play {
  width: 28px; height: 28px; padding: 0; border-radius: 7px; flex-shrink: 0;
  border: 1px solid var(--border-strong); background: var(--raised); color: var(--accent);
  display: grid; place-items: center; font-size: 12px; line-height: 1;
}
.player-play:hover { background: var(--accent-bg); border-color: #365c48; }
.player-time { font: 11.5px var(--mono); color: var(--dim); font-variant-numeric: tabular-nums; white-space: nowrap; }
.player-seek {
  -webkit-appearance: none; appearance: none; flex: 1; min-width: 60px; height: 20px; margin: 0; background: transparent;
}
.player-seek::-webkit-slider-runnable-track {
  height: 5px; border-radius: 99px;
  background: linear-gradient(90deg, #4db883 var(--seek, 0%), #232f29 var(--seek, 0%));
}
.player-seek::-webkit-slider-thumb {
  -webkit-appearance: none; appearance: none;
  width: 12px; height: 12px; border-radius: 50%; margin-top: -3.5px;
  background: var(--accent); border: 0; box-shadow: 0 0 0 2px var(--inset);
}
.mic-readiness { color: var(--dim); font-size: 13px; margin-bottom: 10px; }
.preview-status { color: var(--dim); font-size: 13px; margin-top: 8px; }
.preview-status[data-error=true] { color: var(--red); }
.sample-availability { color: var(--yellow); font-size: 13px; margin-top: 8px; }

/* Model & Rendering Device drawer */
.noise-subgrid { display: grid; grid-template-columns: 1fr 1fr; gap: 12px; margin-top: 4px; }
.noise-subgrid > div { min-width: 0; }
.noise-subgrid label { display: flex; align-items: center; justify-content: space-between; gap: 6px; color: var(--label); font-size: 12.5px; font-weight: 500; margin-bottom: 5px; white-space: nowrap; }
.noise-subgrid select { min-height: 36px; font-size: 13px; padding: 4px 24px 4px 10px; }
.model-table-wrap { margin-top: 12px; overflow-x: auto; }
.model-table { width: 100%; border-collapse: collapse; font-size: 12px; }
.model-table th { text-align: left; font-size: 11px; font-weight: 600; color: var(--dim); padding: 6px 10px; border-bottom: 1px solid var(--border); white-space: nowrap; }
.model-table td { padding: 8px 10px; border-bottom: 1px solid rgba(255,255,255,.045); vertical-align: middle; color: var(--dim); }
.model-table th.num, .model-table td.num { text-align: right; font-variant-numeric: tabular-nums; }
.model-table th:last-child, .model-table td:last-child { padding-left: 18px; }
.model-table tr:last-child td { border-bottom: 0; }
.model-table tr[data-selected=true] { background: rgba(156, 230, 190, .065); }
.model-table tr[data-selected=true] td:first-child { box-shadow: inset 3px 0 0 var(--accent); }
.model-table tr[data-selected=true] td { color: var(--text); }
.model-pick { background: transparent; border: 0; padding: 0; font: inherit; font-weight: 600; color: var(--text); text-align: left; cursor: pointer; white-space: nowrap; text-decoration: underline; text-decoration-color: transparent; text-underline-offset: 3px; }
.model-pick:hover { text-decoration-color: var(--accent); color: var(--accent); }
.model-tag { display: inline-block; font-size: 11px; font-weight: 600; padding: 2px 7px; border-radius: 4px; white-space: nowrap; }
.model-tag.cpu { background: #142026; color: #b5d1e2; }
.model-tag.gpu { background: #1c1926; color: #cbc1e6; }
.nowrap { white-space: nowrap; }
.mono-cell { font: 11.5px var(--mono); white-space: nowrap; }
.tuning-top-row { display: grid; grid-template-columns: 1fr 1fr; gap: 16px; align-items: end; margin-bottom: 14px; }
.tuning-makeup { min-width: 0; }
.tuning-makeup label, .tuning-lowcut-label { display: block; font-size: 12.5px; font-weight: 500; color: var(--label); margin-bottom: 5px; }
.tuning-makeup select { min-height: 36px; font-size: 13px; padding: 4px 26px 4px 10px; }
.tuning-grid { display: grid; grid-template-columns: 1fr 1fr; gap: 16px; }
.lowcut-row { display: inline-flex; gap: 8px; align-items: center; min-height: 36px; font-size: 13px; cursor: pointer; }
.lowcut-row input { width: 16px; height: 16px; margin: 0; }
.processor-status { font-size: 13px; color: var(--dim); margin-top: 10px; overflow-wrap: anywhere; }
.processor-status[data-error=true] { color: var(--red); }
.processor-status[data-overload=true] { color: var(--yellow); background: #292416; border: 1px solid #7d6938; border-radius: 8px; padding: 9px 11px; }
.processing-availability { color: var(--yellow); font-size: 13px; border-left: 3px solid var(--yellow); padding: 6px 10px; margin-bottom: 10px; background: #221e14; border-radius: 0 6px 6px 0; }
.effects-unavailable { padding: 13px 14px; border: 1px solid #8f5e55; border-radius: 10px; color: #ffd2ca; background: #2d1e1b; margin-bottom: 12px; }
.effects-unavailable strong { font-size: 14px; }
.effects-unavailable p, .effects-unavailable ol { font-size: 13px; margin-top: 6px; }
.effects-unavailable summary { padding: 10px 0 0; min-height: 32px; font-size: 13px; list-style: none; display: flex; align-items: center; justify-content: space-between; }
.effects-unavailable summary::after { content: '+'; font-family: var(--mono); }
.effects-unavailable details[open] > summary::after { content: '−'; }
.effects-unavailable ol { padding-left: 20px; }
.effects-unavailable code { font-size: 12px; overflow-wrap: anywhere; }
.voice-panel[data-unavailable=true] .switch:checked { background: #5a6760; border-color: #88968e; }

/* Devices section */
.peer-count { font: 12px var(--mono); color: var(--dim); }
#peerRoleHelp { color: var(--dim); font-size: 13px; margin-bottom: 12px; line-height: 1.5; }
.device-outcome { color: var(--accent); font-size: 13px; margin-bottom: 10px; }
.device-outcome:empty { display: none; }
.peer-item {
  display: flex; gap: 8px 12px; align-items: center; flex-wrap: wrap;
  padding: 12px 14px; border: 1px solid rgba(255, 255, 255, .05); border-radius: 10px;
  background: rgba(255, 255, 255, .02);
}
.peer-item[data-connected=true] { border-color: rgba(156, 230, 190, .2); background: rgba(156, 230, 190, .035); }
.peer-item + .peer-item { margin-top: 8px; }
.peer-info { display: flex; flex-direction: column; gap: 2px; min-width: 140px; flex: 1; }
.peer-name { font-size: 14px; font-weight: 600; overflow-wrap: anywhere; display: flex; align-items: center; gap: 8px; }
.peer-dot { width: 7px; height: 7px; border-radius: 50%; background: #4a5a52; flex-shrink: 0; }
.peer-item[data-connected=true] .peer-dot { background: var(--accent); box-shadow: 0 0 0 3px rgba(156, 230, 190, .14); }
.peer-live-tag { font-size: 11px; font-weight: 600; color: var(--accent); background: var(--accent-bg); border: 1px solid #2d4c3c; border-radius: 99px; padding: 1px 8px; }
.peer-meta-row { display: flex; align-items: center; gap: 8px; padding-left: 15px; }
.peer-id { font: 11px var(--mono); color: #8e9e96; }
.peer-age { font: 11.5px var(--mono); color: var(--dim); }
.peer-role-label { display: flex; align-items: center; gap: 8px; color: var(--label); font-size: 12.5px; font-weight: 500; margin-left: auto; }
.peer-role-label select { font-size: 13px; width: 132px; min-height: 34px; }
.peer-revoke { min-height: 34px; border: 1px solid rgba(255, 255, 255, .08); border-radius: 7px; background: rgba(255, 255, 255, .02); color: #b5c3bb; font-size: 13px; padding: 5px 11px; transition: color .15s, border-color .15s, background .15s; }
.peer-revoke:hover { color: var(--red); border-color: rgba(255, 140, 130, .4); background: rgba(255, 140, 130, .09); }
.peer-role-status { width: 100%; font-size: 13px; color: var(--dim); line-height: 1.45; }
.peer-role-status[data-state=error] { color: var(--red); border-left: 3px solid var(--red); padding-left: 10px; }
.peer-role-status[data-state=pending] { color: var(--yellow); }

/* Setup warnings & feedback */
.local-feedback { color: var(--red); font-size: 13px; margin: 8px 0; line-height: 1.45; }
.local-feedback[data-pending=true] { color: var(--accent); }
.setup-warning {
  display: flex; align-items: center; justify-content: space-between; gap: 12px;
  background: #242117; color: #f3dfab; border: 1px solid #96814c;
  border-radius: 10px; padding: 11px 14px; margin-bottom: 12px;
}
.setup-warning p { margin-top: 3px; font-size: 13px; }
.setup-warning .btn { flex-shrink: 0; }
.peer-item, .local-feedback, #pairedDevicesSummary { scroll-margin-top: 72px; }

/* Diagnostics & Footer */
.diagnostics {
  margin-top: 12px; border: 1px solid rgba(255, 255, 255, .055);
  border-radius: 11px; background: rgba(255, 255, 255, .015); overflow: hidden;
}
.diagnostics > summary {
  min-height: 40px; padding: 9px 18px;
  display: flex; align-items: center; justify-content: space-between; gap: 10px;
  list-style: none; font-size: 13px; font-weight: 600; color: var(--dim);
  transition: color .15s, background .15s;
}
.diagnostics > summary:hover { color: var(--text); background: rgba(255, 255, 255, .02); }
.diagnostics .details-content { padding: 4px 18px 14px; }
.stats-row { font: 12px/1.8 var(--mono); color: var(--dim); overflow-wrap: anywhere; }
.log-container { margin-top: 10px; max-height: 150px; overflow: auto; padding: 10px 12px; border: 1px solid var(--border); border-radius: 8px; background: var(--inset); font: 12px/1.75 var(--mono); color: var(--dim); user-select: text; }
.log-line { overflow-wrap: anywhere; }
.hot { color: var(--red); }
.footer {
  display: flex; align-items: center; justify-content: space-between; gap: 12px;
  color: var(--dim); font-size: 12px; margin-top: 12px; padding-top: 10px;
  border-top: 1px solid rgba(255, 255, 255, .06);
}
.footer-left { display: flex; align-items: center; gap: 8px; flex-wrap: wrap; }
#openAppUpdates {
  min-height: 26px; padding: 2px 8px; font-size: 12px; font-weight: 500;
  background: transparent; border: 1px solid transparent; color: var(--label);
  border-radius: 6px;
}
#openAppUpdates:hover:not(:disabled) { color: var(--text); background: rgba(255, 255, 255, .05); border-color: var(--border); }
#openAppUpdates[data-available=true] { color: var(--accent); background: var(--accent-bg); border-color: #2d4c3c; }

/* Recording Dock & Dialogs */
.recording-strip {
  position: fixed; bottom: 0; left: 0; z-index: 10; width: 100%; height: 76px;
  display: flex; align-items: center; justify-content: space-between; gap: 12px;
  padding: 8px 24px; border-top: 1px solid #96665d; background: #2d1e1b;
  box-shadow: 0 -8px 28px #0008;
}
.recording-strip p { font-size: 13px; color: #f4d3cd; }
.btn-row { display: flex; gap: 8px; margin-top: 12px; flex-wrap: wrap; }
dialog {
  background: var(--card); color: var(--text); border: 1px solid var(--border-strong);
  border-radius: 14px; max-width: 440px; width: calc(100% - 36px); padding: 24px;
}
dialog::backdrop { background: #060a08d9; }
)html") +
std::string(R"html(dialog p { color: var(--dim); margin-top: 10px; font-size: 13px; }
dialog .btn-row { justify-content: flex-end; margin-top: 20px; }
#micSetupDialog { max-width: 540px; max-height: calc(100vh - 32px); overflow: auto; }
#micSetupDialog ol { margin: 14px 0; padding-left: 20px; font-size: 14px; }
#micSetupDialog li + li { margin-top: 10px; }
.empty-copy, .dim { color: var(--dim); font-size: 13px; }

/* Responsive <= 1000px (Compact & Tabbed) */
@media (max-width: 1000px) {
  .app-header { height: 50px; }
  .app-header-inner { padding: 0 18px; }
  .app-header .header-note { display: none; }
  #manageDevices { margin-left: auto; }
  .workspace { padding: 12px 18px 18px; }
  .workspace-heading { margin-bottom: 10px; }
  h1 { font-size: 20px; }
  .subtitle { display: none; }
  .session { margin-bottom: 10px; }
  .session-connected { grid-template-columns: 1.15fr 1fr; gap: 8px 16px; padding: 10px 14px; }
  .session-identity { padding-right: 0; }
  .session-meters { padding: 0; border: 0; }
  .session-metrics {
    grid-column: 1 / -1; justify-content: flex-start; display: flex; gap: 18px;
    border-left: 0; padding-left: 0; border-top: 1px solid var(--border); padding-top: 7px;
  }
  .session-metrics > div { display: flex; align-items: center; gap: 6px; }
  .metric-value { font-size: 12px; }
  .metric-label { margin: 0; font-size: 12px; }
  .session-empty { padding: 12px 14px; }
  .session-empty .empty-icon { display: none; }
  .io-grid, .proc-grid, .pc-mic, .stream-top-grid { grid-template-columns: 1fr; gap: 0; }
  .pc-mic { gap: 8px; margin-top: 10px; padding-top: 10px; }
  .stream-top-grid { gap: 14px; }
  .subcard { padding: 0 0 12px; }
  .subcard + .subcard { padding: 12px 0 0; border-left: 0; border-top: 1px solid rgba(255, 255, 255, .06); }
  .major-summary.compact-tab-owned { display: none; }
  .io-section, .voice-panel { border: 1px solid var(--border); border-radius: 12px; background: var(--card); overflow: hidden; }
  .io-section > .major-body, .voice-panel > .major-body { padding: 12px 14px; }
  .pair-content { flex-wrap: wrap; gap: 12px; }
  .pair-content > div:first-child { flex-basis: 100%; }
}
@media (max-width: 1000px) and (max-height: 700px) {
  .app-header { height: 44px; }
  #manageDevices, .pairing-shortcut { min-height: 30px; padding: 4px 10px; font-size: 12px; }
  .workspace { padding-top: 8px; }
  .workspace-heading > div:first-child { display: none; }
  .workspace-heading { justify-content: space-between; margin-bottom: 6px; }
  .workspace-heading:has(.pairing-outcome:empty) { justify-content: flex-end; }
  .status-pill { padding: 5px 10px; font-size: 12px; }
  .session { margin-bottom: 6px; }
  .session-connected { padding: 7px 12px; gap: 2px 14px; }
  .session-metrics { display: none; }
  #micOwnerLabel { display: none; }
  .workspace-tabs { margin-bottom: 6px; }
  .workspace-tab { min-height: 32px; }
  .io-section > .major-body, .voice-panel > .major-body { padding: 10px 12px; }
  .subcard { padding: 0 0 8px; }
  .subcard + .subcard { padding: 8px 0 0; }
  .subcard-head { margin-bottom: 5px; }
  .subcard-label { margin-bottom: 4px; }
  select { min-height: 32px; padding: 4px 26px 4px 10px; }
  .route-mode { margin-top: 6px; }
  .route-mode label { margin-bottom: 3px; }
  .route-mode select { min-height: 32px; }
  .pc-capture-hint { display: none; }
  .setup-status { padding: 5px 9px; margin-top: 6px; }
  .setup-warning { margin-bottom: 8px; padding: 8px 10px; }
  .setup-warning p { display: none; }
  .io-section:has(#missingMicWarning:not([hidden])) .route-note { display: none; }
  .effect { padding: 7px 0; }
  .strength-row { padding: 0 0 7px; }
  .preview-heading { margin-bottom: 4px; }
  .mic-readiness { margin-bottom: 6px; }
  .preview-status { margin-top: 6px; }
  .drawer-stack { margin-top: 8px; }
  details.voice-drawer > summary, details.tuning > summary, details.stream-panel > summary, details.subdrawer > summary { min-height: 34px; padding: 6px 12px; }
  body[data-task-errors=true] .session-connected { padding: 6px 12px; gap: 8px; }
  body[data-task-errors=true] #clientAddress { display: none; }
  .pair-request { margin-bottom: 6px; padding: 8px 12px; }
  .notice { padding: 8px 12px; margin-bottom: 6px; }
  .operation-links { margin-top: 4px; }
}
@media (max-width: 520px) {
  .app-header-inner { padding: 0 14px; }
  .app-header:has(.pairing-shortcut:not([hidden])) .brand { font-size: 0; gap: 0; }
  #manageDevices, .pairing-shortcut { font-size: 12px; padding: 5px 9px; }
  .workspace { padding: 12px 14px; }
  .session-connected { grid-template-columns: 1fr; gap: 10px; }
  .noise-subgrid, .tuning-grid, .tuning-top-row { grid-template-columns: 1fr; gap: 12px; }
  .preview-actions, .notice, .footer { flex-wrap: wrap; }
  .peer-id { display: none; }
  .peer-role-label { width: 100%; margin-left: 0; justify-content: space-between; }
  .recording-strip { height: 92px; }
  body[data-recording=true] { height: calc(100vh - 92px); }
}
@media (prefers-reduced-motion: reduce) { *, *::before { transition: none !important; } }
@media (forced-colors: active) { .switch:checked { background: Highlight; } .switch:checked::before { background: HighlightText; } .level-fill { background: Highlight; } .btn.active { outline: 2px solid Highlight; } }
</style>
</head>
<body>
<svg class="symbols" aria-hidden="true" xmlns="http://www.w3.org/2000/svg"><symbol id="icon-wave" viewBox="0 0 24 24"><path d="M3 10v4m5-8v12m4-15v18m4-15v12m5-8v4"/></symbol><symbol id="icon-pc" viewBox="0 0 24 24"><rect x="3" y="4" width="18" height="13" rx="2"/><path d="M8 21h8m-4-4v4"/></symbol><symbol id="icon-mic" viewBox="0 0 24 24"><rect x="9" y="2" width="6" height="12" rx="3"/><path d="M5 10v2a7 7 0 0 0 14 0v-2m-7 9v3m-4 0h8"/></symbol><symbol id="icon-phone" viewBox="0 0 24 24"><rect x="6" y="2" width="12" height="20" rx="3"/><path d="M10 5h4m-3 14h2"/></symbol><symbol id="icon-route" viewBox="0 0 24 24"><path d="M4 7h16m-4-4 4 4-4 4M20 17H4m4-4-4 4 4 4"/></symbol><symbol id="icon-lock" viewBox="0 0 24 24"><rect x="5" y="10" width="14" height="11" rx="2"/><path d="M8 10V7a4 4 0 0 1 8 0v3m-4 5v2"/></symbol></svg>
<header class="app-header"><div class="app-header-inner"><div class="brand"><span class="brand-mark"><svg class="icon" aria-hidden="true"><use href="#icon-wave"/></svg></span>AudioBridge</div><div class="header-note"><svg class="icon" aria-hidden="true" style="width:13px;height:13px"><use href="#icon-lock"/></svg><span>Local LAN · Windows desktop</span></div><button class="btn" id="manageDevices">Devices &amp; access</button><button class="btn pairing-shortcut" id="reviewPairing" aria-label="Review pending pairing request" hidden>Review pairing</button></div></header>
<main class="workspace">
<div class="workspace-heading"><div><h1>Audio workspace</h1><p class="subtitle">Your PC's sound. Your phone's freedom.</p></div><p class="pairing-outcome" id="pairingOutcome" role="status"></p><div class="status-pill" role="status"><span class="status-dot" id="statusDot"></span><span id="statusText" tabindex="-1">Ready to connect</span></div></div>
<section class="pair-request" id="pairRequestCard" style="display:none" aria-label="Pairing request" role="alert"><details id="pairDetails" open><summary><span id="pairingSummaryText">Pairing request · review codes</span></summary><div class="pair-content"><div><div class="eyebrow">New connection request</div><div class="pair-name" id="pairRequestName"></div><div class="pair-id" id="pairRequestId"></div><p>Compare with your phone. Only approve if both codes match.</p><p class="pair-consent">New devices start <strong>Read only</strong>: listen and use their own phone controls and mic. Grant PC settings and media control separately in <strong>Devices</strong>.</p><p class="local-feedback" id="pairingFeedback" role="status" hidden></p></div><div><div class="pair-code" id="pairCode"></div><div class="btn-row"><button class="btn primary" id="btnApprove">Codes match · approve</button><button class="btn" id="btnDeny">Deny</button></div></div></div></details></section>
<div class="notice warning" id="bridgeNotice" role="alert" hidden><div><h2>Desktop controls are disconnected</h2><p>Current connection status is unavailable. Audio may still be running. Controls will return when the desktop responds.</p></div><button class="btn" id="retryBridge">Retry</button></div><div class="notice error" id="operationNotice" role="alert" hidden><div><p id="operationMessage"></p><div class="operation-links" id="operationLinks"></div></div><button class="btn" id="dismissOperationError">Hide summary</button></div>
<section class="session" id="sessionOverview" aria-label="Connection overview"><div class="session-connected" id="connectedInfo" style="display:none"><div class="session-identity"><div class="session-name"><svg class="icon" aria-hidden="true"><use href="#icon-phone"/></svg><span id="clientInfo"></span></div><div class="session-meta"><span id="clientAddress"></span><span class="encryption-badge" id="sessionSecurity"></span></div><div class="session-meta" id="micOwnerLabel"></div></div><div class="session-meters"><div class="level-row"><span class="level-label">PC audio</span><div class="level-bar" role="meter" aria-label="PC audio level" aria-valuemin="0" aria-valuemax="100" aria-valuenow="0" id="sendMeter"><div class="level-fill" id="sendLevelFill"></div></div></div><div class="level-row"><span class="level-label" title="Incoming phone audio, not proof of delivery to a calling app">Mic received</span><div class="level-bar" role="meter" aria-label="Phone microphone received level" aria-valuemin="0" aria-valuemax="100" aria-valuenow="0" id="recvMeter"><div class="level-fill" id="recvLevelFill"></div></div></div></div><div class="session-metrics"><div><span class="metric-value" id="rttValue">—</span><span class="metric-label">Network RTT</span></div><div><span class="metric-value" id="micLatencyValue">—</span><span class="metric-label" title="Input latency from microphone (local or phone) to post-processed audio">Mic → DSP</span></div><div><span class="metric-value" id="uptimeValue">—</span><span class="metric-label">Session time</span></div></div></div><div class="session-empty" id="disconnectedInfo"><div class="empty-icon"><svg class="icon" aria-hidden="true"><use href="#icon-phone"/></svg></div><div><h2>Let's connect your phone</h2><p>Use the same Wi-Fi, open AudioBridge on your phone, and select this PC. Then compare the pairing codes.</p><div class="server-info" id="serverInfo">Finding your server…</div><details class="discovery-help"><summary>Can’t find this PC?</summary><ol><li>Connect both devices to the same local network; guest Wi-Fi may isolate them.</li><li>Rescan in the phone app and allow AudioBridge through Windows Firewall on your private network.</li><li>If a VPN blocks local devices, allow local network access. Keep pairing-code verification enabled.</li></ol></details></div></div></section>
)html") +
std::string(R"html(<nav class="workspace-tabs" id="workspaceTabs" role="tablist" aria-label="Audio workspace" hidden><button id="routingTab" class="workspace-tab" role="tab" aria-selected="true" aria-controls="routeWorkspace">Audio I/O &amp; Devices</button><button id="microphoneTab" class="workspace-tab" role="tab" aria-selected="false" aria-controls="voiceCard" tabindex="-1">Audio Processing</button></nav>
<fieldset class="control-fieldset" id="workspaceControls" aria-label="Audio controls"><div class="deck">
<div id="routeWorkspace" aria-labelledby="routingTitle">
<details class="major-section io-section" id="ioSection" open>
<summary class="major-summary compact-tab-owned" id="ioSummaryHeader"><div class="major-lead"><span class="major-num">01</span><div class="major-heading-wrap"><div class="major-title-row"><h2 id="routingTitle">Audio I/O</h2></div><span class="major-live-summary" id="ioSectionSummary">PC playback → Phone · Microphone → PC</span></div></div><span class="section-badge" id="ioSectionBadge" data-kind="meta">PCM · 2.5 ms</span></summary>
<div class="major-body">
<div class="setup-warning" id="missingMicWarning" role="status" hidden><div><strong>No virtual microphone for calls</strong><p>Install a virtual input to send your phone mic into a calling app.</p></div><button class="btn" id="openMicSetup">Setup guide</button></div>
<div class="io-grid">
<div class="subcard"><div class="subcard-head"><span class="subcard-kicker"><svg class="icon" aria-hidden="true"><use href="#icon-pc"/></svg>PC → Phone</span></div><label class="subcard-label" for="deviceSelect">Send PC audio</label><select id="deviceSelect" aria-describedby="selectedDeviceName"><option>Default device</option></select><p class="endpoint-name" id="selectedDeviceName" hidden></p><p class="local-feedback" id="routingFeedback" role="status" hidden></p><p class="local-feedback" id="captureStatus" role="status" hidden></p><div class="route-mode"><label for="captureModeSelect">Capture</label><select id="captureModeSelect"><option value="loopback">System audio</option><option value="recording">Recording device</option></select></div><p class="route-note pc-capture-hint">Choose the Windows playback bus used by your games, media, and calls.</p></div>
<div class="subcard"><div class="subcard-head"><span class="subcard-kicker" id="micRouteTitle"><svg class="icon" aria-hidden="true"><use href="#icon-mic"/></svg>Microphone → PC</span></div><label class="subcard-label" for="outDeviceSelect">Microphone destination</label><select id="outDeviceSelect" aria-describedby="selectedOutputName"><option>Default device</option></select><p class="endpoint-name" id="selectedOutputName" hidden></p><p class="local-feedback" id="outputReadiness" role="status" hidden></p><p class="setup-status" id="driverSetupStatus">Checking microphone destinations…</p><p class="route-note" id="routeMicStatus"></p><p class="local-feedback" id="outputFeedback" role="status" hidden></p></div>
</div>
<div class="pc-mic"><div class="pc-mic-toggle-col"><div class="effect"><label class="effect-name" for="localMic">Use a PC microphone</label><input class="switch" type="checkbox" role="switch" id="localMic" aria-describedby="localMicStatus"></div><p class="pc-mic-note" id="localMicStatus" aria-live="polite">Off. The phone microphone is sent to the destination above.</p><p class="local-feedback" id="localMicFeedback" role="status" hidden></p></div><div class="pc-mic-select-col"><label for="localInput">PC microphone</label><select id="localInput" aria-describedby="localMicStatus"></select></div></div>
<div class="drawer-stack">
<details class="stream-panel"><summary><span>Stream tuning</span><span class="summary-info" id="streamSummary">PCM · 2.5 ms</span></summary><div class="details-content"><div class="stream-top-grid"><div class="control-group"><div class="control-heading"><span id="codecLabel">Audio format</span><span class="control-value" id="codecValue" hidden>Raw PCM</span></div><div class="segmented" role="group" aria-labelledby="codecLabel"><button class="btn codec-btn active" id="codecBtnPcm" data-codec="pcm" aria-pressed="true">Raw PCM</button><button class="btn codec-btn" id="codecBtnOpus" data-codec="opus" aria-pressed="false">Opus</button></div><p class="control-note">PCM skips codec lookahead. Opus uses less bandwidth.</p></div><div class="control-group"><div class="control-heading"><span id="frameLabel">Packet duration</span><span class="control-value" id="frameSizeValue" hidden>2.5 ms</span></div><div class="segmented" role="group" aria-labelledby="frameLabel"><button class="btn frame-btn active" id="frameBtn2" data-ms="2" aria-pressed="true">2.5 ms</button><button class="btn frame-btn" id="frameBtn5" data-ms="5" aria-pressed="false">5 ms</button><button class="btn frame-btn" id="frameBtn10" data-ms="10" aria-pressed="false">10 ms</button><button class="btn frame-btn" id="frameBtn20" data-ms="20" aria-pressed="false">20 ms</button></div></div></div><p class="local-feedback" id="transportFeedback" role="status" hidden></p><div class="control-group"><div class="control-heading"><label for="jitterSlider">Playback buffer</label><span class="control-value" id="jitterValue">10 ms</span></div><input type="range" id="jitterSlider" min="0" max="100" step="1" value="10"><div class="range-caption"><span>Lower latency</span><span>More stability</span></div></div></div></details>
<details class="subdrawer"><summary><span>Headset setup checklist</span><span class="summary-info">CABLE Output guide</span></summary><div class="details-content help-copy"><ol><li><strong>Check the virtual microphone.</strong> If CABLE Input is missing, install VB-CABLE from its official vendor and restart AudioBridge. You can instead use AudioBridge Microphone when its driver and service are available. Do not select an unavailable endpoint.</li><li>Use <strong>System audio</strong> and select the PC playback device used by your games and calls.</li><li>Return the phone microphone to <strong>CABLE Input</strong>. Choose <strong>CABLE Output</strong> as your microphone in Discord, Zoom, or your game.</li><li>Listen through headphones connected to your phone. One VB-CABLE is enough.</li><li><strong>Verify the route.</strong> Enable your phone mic and speak. Watch the Phone mic meter here, then the input meter in your calling app. The calling app must use CABLE Output (or AudioBridge Microphone), not CABLE Input.</li></ol><p>Keep CABLE Input dedicated to your mic. Do not send it back to the phone or into its own Voicemeeter bus. AudioBridge Microphone is an alternative with its working driver and service installed.</p></div></details>
</div>
</div>
</details>
</div>

<details class="major-section voice-panel" id="voiceCard" aria-labelledby="voiceTitle" open>
<summary class="major-summary compact-tab-owned" id="voiceSummaryHeader"><div class="major-lead"><span class="major-num">02</span><div class="major-heading-wrap"><div class="major-title-row"><h2 id="voiceTitle">Audio Processing</h2></div><span class="major-live-summary" id="voiceSectionSummary">DeepFilterNet3 · 20 dB · Gate on</span></div></div><span class="section-badge" id="voiceDeviceBadge" data-kind="cpu">CPU · 0% GPU</span></summary>
<div class="major-body">
<p class="processing-availability" id="processingAvailability" role="status" hidden>Loading voice models · saved cleanup preferences are inactive.</p>
<div class="effects-unavailable" id="effectsUnavailable" role="status" hidden><strong id="effectsUnavailableTitle">Effects unavailable · preferences inactive</strong><p id="effectsUnavailableDescription">Saved preferences below are inactive. Unprocessed microphone audio is sent to the destination.</p><details id="effectsRestoreDetails"><summary>Restore voice processing</summary><ol><li>Keep <code>onnxruntime.dll</code> and the complete <code>voice-models</code> folder beside <code>audiobridge.exe</code>.</li><li>If files were moved or removed, restore them from the same desktop build.</li><li>Close and relaunch AudioBridge to load the models again. Your phone app does not need an update.</li></ol></details></div>
<p class="local-feedback" id="settingsFeedback" role="status" hidden></p>
<div class="proc-grid">
<div class="subcard"><div class="effect"><div><label class="effect-name" for="voiceNoise">Noise removal</label><p class="effect-description" id="noiseHelp">Keep the voice. Cut the background.</p></div><input class="switch" id="voiceNoise" type="checkbox" role="switch" aria-describedby="noiseHelp" checked></div><div class="strength-row"><label for="voiceStrength">Strength</label><select id="voiceStrength"><option value="6">Gentle · 6 dB</option><option value="10">Light · 10 dB</option><option value="15">Medium · 15 dB</option><option value="20">Strong · 20 dB</option><option value="30">Intense · 30 dB</option><option value="100" selected>Maximum</option></select></div><div class="effect"><div><label class="effect-name" for="voiceGate">Voice gate</label><p class="effect-description" id="gateHelp">Quiet the mic when you're not speaking.</p></div><input class="switch" id="voiceGate" type="checkbox" role="switch" aria-describedby="gateHelp" checked></div></div>
<div class="subcard"><div class="preview-heading"><h3>Test your microphone</h3><span id="voiceTime">10-second mic check</span></div><p class="mic-readiness" id="micReadiness"></p><div class="preview-actions"><button class="btn voice-btn" id="voiceRecord" aria-pressed="false" disabled>Record sample</button><div class="sample-version"><label for="voiceSource">Listen to</label><select id="voiceSource"><option value="1" id="processedOption">Processed</option><option value="0">Original</option></select></div></div><p class="local-feedback" id="recordingFeedback" role="status" hidden></p><p class="local-feedback" id="clipFeedback" role="status" hidden></p><p class="sample-availability" id="sampleAvailability" role="status" hidden></p><button class="btn" id="retrySample" hidden>Reload sample</button><div class="studio-player" id="voicePlayer" hidden><button type="button" class="player-play" id="voicePlayBtn" aria-label="Play sample">&#9654;</button><span class="player-time" id="voicePlayerTime">0:00 / 0:00</span><input type="range" class="player-seek" id="voiceSeek" min="0" max="1000" value="0" aria-label="Seek sample playback"></div><audio id="voiceAudio" class="sr-audio" tabindex="-1" aria-hidden="true" controls preload="auto" aria-label="Microphone sample playback" hidden></audio><p class="preview-status" id="voicePreviewStatus" role="status">Connect your phone and enable its mic, then record a sample to compare.</p></div>
</div>
<div class="drawer-stack">
)html") +
std::string(R"html(<details class="voice-drawer" id="voiceModelDrawer"><summary><span>Model &amp; rendering device</span><span class="drawer-summary-val" id="voiceModelSummary">DeepFilterNet3 · CPU · 12 MB</span></summary><div class="details-content"><div class="noise-subgrid"><div><label for="voiceModel"><span>Model</span><span class="mono-cell" id="voiceSizeHint">12.3 MB</span></label><select id="voiceModel"><optgroup label="CPU · 0% GPU usage"><option value="0" selected>DeepFilterNet3 (12 MB · 32 ms)</option><option value="1">FastEnhancer-L (4.4 MB · 17 ms)</option><option value="2">DPDFNet8 HR (14 MB · 50 ms)</option></optgroup><optgroup label="Discrete GPU · DirectML FP16"><option value="3">Inworld Realtime (129 MB · 53 ms)</option></optgroup></select></div><div><label for="voiceGpu"><span>Rendering device</span><span id="voiceGpuHint">CPU only</span></label><select id="voiceGpu"><option value="-1" selected>Auto (fastest GPU)</option></select></div></div><div class="model-table-wrap"><table class="model-table" aria-label="Compare noise removal models"><thead><tr><th>Model</th><th>Runs on</th><th class="num">Size</th><th class="num">Delay</th><th>Best for</th></tr></thead><tbody id="voiceCompareBody"><tr data-model="0" data-selected="true"><td class="nowrap"><button type="button" class="model-pick" data-model="0">DeepFilterNet3</button></td><td class="nowrap"><span class="model-tag cpu">CPU</span></td><td class="mono-cell num">12.3 MB</td><td class="mono-cell num">32 ms</td><td>Balanced everyday cleanup (~3% of 1 CPU core, 0% GPU).</td></tr><tr data-model="1"><td class="nowrap"><button type="button" class="model-pick" data-model="1">FastEnhancer-L</button></td><td class="nowrap"><span class="model-tag cpu">CPU</span></td><td class="mono-cell num">4.4 MB</td><td class="mono-cell num">17 ms</td><td>Lowest latency &amp; smallest model (0% GPU).</td></tr><tr data-model="2"><td class="nowrap"><button type="button" class="model-pick" data-model="2">DPDFNet8 HR</button></td><td class="nowrap"><span class="model-tag cpu">CPU</span></td><td class="mono-cell num">14.2 MB</td><td class="mono-cell num">50 ms</td><td>Strongest CPU suppression (-92 dB floor). Best while gaming (0% GPU).</td></tr><tr data-model="3"><td class="nowrap"><button type="button" class="model-pick" data-model="3">Inworld Realtime</button></td><td class="nowrap"><span class="model-tag gpu">Discrete GPU</span></td><td class="mono-cell num">129.4 MB</td><td class="mono-cell num">53 ms</td><td>58.2M-param studio voice isolator. Needs dedicated GPU (4 GB+ VRAM).</td></tr></tbody></table></div></div></details>
<details class="tuning" id="voiceTuningDrawer"><summary><span>Fine-tune audio &amp; gate</span><span class="drawer-summary-val" id="voiceTuningSummary">Gate 50% / 0.3 s</span></summary><div class="details-content"><div class="tuning-top-row"><div class="tuning-makeup"><label for="voiceMakeup">Level after cleanup</label><select id="voiceMakeup"><option value="0">0 dB</option><option value="1">+1 dB</option><option value="2">+2 dB</option><option value="3">+3 dB</option><option value="4">+4 dB</option><option value="5">+5 dB</option><option value="6">+6 dB</option><option value="7">+7 dB</option><option value="8">+8 dB</option><option value="9">+9 dB</option><option value="10">+10 dB</option><option value="11">+11 dB</option><option value="12">+12 dB</option></select></div><div><span class="tuning-lowcut-label">High-pass filter</span><label class="lowcut-row"><input id="voiceLowCut" type="checkbox">Reduce low rumble <span class="dim">· 80 Hz</span></label></div></div><div class="tuning-grid"><div><div class="control-heading"><label for="voiceThreshold">Speech threshold</label><span class="control-value" id="voiceThresholdValue">50%</span></div><input id="voiceThreshold" type="range" min="10" max="90" value="50"><p class="control-note">Higher requires clearer speech.</p></div><div><div class="control-heading"><label for="voiceHold">Gate hold</label><span class="control-value" id="voiceHoldValue">0.3 s</span></div><input id="voiceHold" type="range" min="100" max="1500" step="100" value="300"><p class="control-note">Wait before quieting the mic.</p></div></div></div></details>
</div>
<p class="processor-status" id="voiceStatus" role="status">Loading voice models…</p>
</div>
</details>

<details class="major-section peers-panel" id="devicesSection">
<summary class="major-summary" id="pairedDevicesSummary"><div class="major-lead"><span class="major-num">03</span><div class="major-heading-wrap"><div class="major-title-row"><h2>Devices</h2><span class="peer-count" id="peerCount">0</span></div><span class="major-live-summary" id="devicesSectionSummary">No paired devices yet · Ready to pair</span></div></div><span class="section-badge" id="devicesSectionBadge" data-kind="off">Waiting</span></summary>
<div class="major-body"><p id="peerRoleHelp">Read only: listen and use local phone controls. Admin: also change PC audio settings and PC media playback. Only this desktop can change permissions. New and existing pairings default to Read only.</p><p id="devicesOutcome" class="device-outcome" role="status"></p><div id="peerList"><p class="empty-copy">No paired devices yet. Connect your phone to get started.</p></div></div>
</details>
</div></fieldset>
<details class="diagnostics"><summary><span>Connection diagnostics</span><span class="summary-info">Activity &amp; transport</span></summary><div class="details-content"><div class="stats-row" id="statsRow1">No stream activity yet.</div><div class="stats-row" id="statsRow2"></div><div class="log-container" id="logContainer" tabindex="0" role="region" aria-label="Connection activity log"><div>Waiting for events…</div></div></div></details>
<dialog id="appUpdatesDialog" aria-labelledby="appUpdatesTitle"><h2 id="appUpdatesTitle">App updates</h2><p id="installedVersion">Installed: 0.2.5</p><p id="updateMessage" role="status" tabindex="-1">Check the official GitHub release for a newer version.</p><progress id="updateProgress" max="1" value="0" aria-label="Update download progress" hidden></progress><p>Your pairings and settings are kept. This updates Windows only; your phone has its own update button.</p><div class="btn-row"><button class="btn" id="closeAppUpdates">Close</button><button class="btn primary" id="checkAppUpdates">Check for updates</button><button class="btn primary" id="installAppUpdate" hidden>Update now</button></div></dialog>
<dialog id="confirmAppUpdateDialog" aria-labelledby="confirmAppUpdateTitle"><h2 id="confirmAppUpdateTitle">Install AudioBridge update?</h2><p>The complete Windows release is downloaded from the official GitHub repository and verified before installation. Audio will stop and AudioBridge will restart. Your pairings, preferences and installed drivers are not removed.</p><div class="btn-row"><button class="btn" id="cancelAppUpdate" autofocus>Not now</button><button class="btn primary" id="confirmAppUpdate">Download and restart</button></div></dialog>
<footer class="footer"><div class="footer-left"><span>AudioBridge / Windows 0.2.5</span><span aria-hidden="true">·</span><button class="btn" id="openAppUpdates">App updates</button></div><span>Processed on your PC. Recordings stay in memory.</span></footer>
</main>
<aside class="recording-strip" id="recordingStrip" aria-label="Recording controls" hidden><div><strong id="recordingStripTitle">Recording phone microphone</strong><p id="recordingStripStatus">Recording stops at 10 seconds.</p></div><button class="btn danger" id="persistentStop">Stop recording</button></aside>
<dialog id="micSetupDialog" aria-labelledby="micSetupTitle"><h2 id="micSetupTitle">Set up your phone as a PC microphone</h2><p>For calling apps, install a virtual microphone on this PC. Playing to ordinary speakers is still possible without one.</p><ol><li>Get <strong>VB-CABLE</strong> from VB-Audio’s official download page. Follow its Windows installation instructions and restart if prompted.</li><li>Relaunch AudioBridge, then choose <strong>CABLE Input</strong> under Microphone destination.</li><li>Choose <strong>CABLE Output</strong> as the microphone in your calling app. Enable your phone mic and check its level.</li></ol><p>Already have the AudioBridge microphone driver and service installed? Choose <strong>AudioBridge Microphone</strong> in both apps instead. An unavailable entry is not ready for use.</p><p class="local-feedback" id="setupOpenError" role="status" hidden></p><div class="btn-row"><button class="btn primary" id="openCableDownload">Open official VB-CABLE page</button><button class="btn" id="closeMicSetup" autofocus>Done</button></div></dialog>
<dialog id="removeDeviceDialog" aria-labelledby="removeDeviceTitle" aria-describedby="removeDeviceDescription"><h2 id="removeDeviceTitle">Remove paired device?</h2><p id="removeDeviceDescription"></p><p id="removeDeviceError" role="alert"></p><div class="btn-row"><button class="btn" id="cancelRemoveDevice" autofocus>Keep device</button><button class="btn danger" id="confirmRemoveDevice">Remove device</button></div></dialog>
<script>
let lastLogCount = 0;
let lastPeerJson = '';
let currentPairId = '';
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
var feedbackIds = {routing:'routingFeedback', localMic:'localMicFeedback', output:'outputFeedback', transport:'transportFeedback', settings:'settingsFeedback', recording:'recordingFeedback', clip:'clipFeedback', playback:'clipFeedback', pairing:'pairingFeedback'};
function setText(id, value) {
    var element = document.getElementById(id);
    if (element && element.textContent !== value) element.textContent = value;
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
    routing:['PC audio','routing','deviceSelect'], localMic:['PC microphone','routing','localMic'], output:['Phone microphone','routing','outDeviceSelect'],
    transport:['Stream tuning','routing','jitterSlider'], settings:['Audio Processing','microphone','voiceNoise'],
    recording:['Recording','microphone','voiceRecord'], clip:['Sample playback','microphone','voiceSource'],
    playback:['Sample playback','microphone','voiceSource'], pairing:['Pairing request',null,'btnApprove']
};
function revealTask(key) {
    var task = taskRecovery[key];
    if (task[1]) { activeWorkspace = task[1]; renderWorkspace(false); }
    if (task[1] === 'routing') { var io = document.getElementById('ioSection'); if (io) io.open = true; }
)html") +
std::string(R"html(    if (task[1] === 'microphone') { var vc = document.getElementById('voiceCard'); if (vc) vc.open = true; }
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
        showOperationError(key, error.timedOut ? 'Desktop response timed out. The outcome is unknown; current state will refresh before you retry.' : typeof message === 'function' ? message() : message);
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
    if (compact) {
        var ioSec = document.getElementById('ioSection'); if (ioSec) ioSec.open = true;
        var voiceSec = document.getElementById('voiceCard'); if (voiceSec) voiceSec.open = true;
    }
    var devSec = document.getElementById('devicesSection');
    if (devSec) devSec.hidden = compact && activeWorkspace !== 'routing';
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
    if (compactLayout.matches && document.getElementById('voiceCard').contains(document.activeElement)) activeWorkspace = 'microphone';
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
var localMicFailure = '';
function changeLocalMic() {
    var source = document.getElementById('localMic').checked ? document.getElementById('localInput').value : 'off';
    if (!source) { document.getElementById('localMic').checked = false; showOperationError('localMic', 'No usable PC microphone found. Connect one and allow microphone access in Windows privacy settings.'); return; }
    localMicFailure = '';
    invokeBridge('localMic', () => Promise.resolve(window._setLocalMic(source)).then(function(result) {
        var receipt = typeof result === 'string' ? JSON.parse(result) : result;
)html") +
std::string(R"html(        if (receipt && receipt.error) localMicFailure = receipt.error;
        return result;
    }), () => localMicFailure || 'Could not open the selected microphone. Check the device and Windows microphone access.',
    state => !!state.localMicEnabled === (source !== 'off') && (source === 'off' || (!!state.localMic && state.localMicId === source)));
}
document.getElementById('localMic').addEventListener('change', changeLocalMic);
document.getElementById('localInput').addEventListener('change', function() {
    if (document.getElementById('localMic').checked) changeLocalMic();
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
function shortEndpoint(name) {
    if (!name) return 'Default';
    var cleaned = name.replace(/^Windows default — /i, '').replace(/\s*\([^)]*\)\s*$/g, '').trim();
    return cleaned.length > 26 ? cleaned.substring(0, 25) + '…' : (cleaned || name);
}
function outputUnavailable(state) {
    if (!state) return false;
    var name = state.currentOutDeviceIndex ? (state.outDevices || [])[state.currentOutDeviceIndex - 1] : state.defaultOutDeviceName;
    return state.outputReady === false || /unavailable/i.test(name || '');
}
function computeMicLatency(s) {
    var voice = s && s.voice;
    if (!voice || !voice.active) return null;
    var isLocal = !!s.localMic;
    var sourceName = isLocal ? 'Local mic' : 'Phone mic';
    var rttHalf = (!isLocal && s.clientRttMs > 0) ? (s.clientRttMs / 2) : 0;
    var sourceMs = isLocal ? 10.0 : (12.5 + rttHalf);
    var modelMs = Number(voice.modelDelayMs) || 0;
    var dspMs = Number(voice.pipelineMs) || Number(voice.processMs) || 0;
    return { totalMs: sourceMs + modelMs + dspMs, sourceName: sourceName, sourceMs: sourceMs, modelMs: modelMs, dspMs: dspMs, processMs: Number(voice.processMs) || 0 };
}
function updateSectionSummaries(s) {
    var srcName = s.currentDeviceIndex ? ((s.devices || [])[s.currentDeviceIndex - 1] || '') : (s.defaultDeviceName || '');
    var dstName = s.currentOutDeviceIndex ? ((s.outDevices || [])[s.currentOutDeviceIndex - 1] || '') : (s.defaultOutDeviceName || '');
    var micSrc = s.localMic ? 'PC mic' : 'Phone mic';
    setText('ioSectionSummary', shortEndpoint(srcName) + ' → Phone · ' + micSrc + ' → ' + shortEndpoint(dstName));
    var ioBadge = document.getElementById('ioSectionBadge');
    if (ioBadge) {
        var ioWarn = s.captureReady === false || outputUnavailable(s);
        ioBadge.setAttribute('data-kind', ioWarn ? 'warn' : 'meta');
        ioBadge.textContent = ioWarn ? 'Check route' : ((s.rawPcm ? 'PCM' : 'Opus') + ' · ' + frameLabel(s.frameSizeMs));
    }
    var peerCount = (s.peers || []).length;
    var devParts = [];
    if (s.connected) devParts.push(s.clientName + ' connected');
    else devParts.push('Waiting for phone');
    devParts.push(peerCount === 1 ? '1 paired' : peerCount + ' paired');
    if (s.pairRequest && s.pairRequest.pending) devParts.push('1 pending');
    setText('devicesSectionSummary', devParts.join(' · '));
    var devBadge = document.getElementById('devicesSectionBadge');
    if (devBadge) {
        var hasPending = !!(s.pairRequest && s.pairRequest.pending);
        devBadge.setAttribute('data-kind', hasPending ? 'warn' : (s.connected ? 'live' : 'off'));
        devBadge.textContent = hasPending ? 'Pairing request' : (s.connected ? (s.encrypted ? 'DTLS live' : 'Connected') : (peerCount ? peerCount + ' paired' : 'Ready'));
    }
}
function updateStatusCard(s) {
    var dot = document.getElementById('statusDot');
    document.getElementById('connectedInfo').style.display = s.connected ? '' : 'none';
    document.getElementById('disconnectedInfo').style.display = s.connected ? 'none' : '';
    dot.className = 'status-dot' + (s.connected ? (s.paused || s.captureReady === false || outputUnavailable(s) ? ' yellow' : ' green') : '');
    setText('statusText', s.connected ? (s.captureReady === false ? 'Connected · PC audio unavailable' : outputUnavailable(s) ? 'Connected · mic return unavailable' : s.paused ? 'Playback paused' : 'Streaming to ' + (s.clientCount || 1) + ((s.clientCount || 1) === 1 ? ' phone' : ' phones')) : 'Ready to connect');
    var micLat = computeMicLatency(s);
    var micLatEl = document.getElementById('micLatencyValue');
    if (micLatEl) {
        micLatEl.textContent = micLat ? Math.round(micLat.totalMs) + ' ms' : '—';
        micLatEl.setAttribute('data-empty', String(!micLat));
        micLatEl.title = micLat ? (micLat.sourceName + ' ' + micLat.sourceMs.toFixed(1) + ' ms + model ' + micLat.modelMs.toFixed(1) + ' ms + DSP ' + micLat.dspMs.toFixed(1) + ' ms') : 'No active microphone stream';
    }
    if (!s.connected) {
        document.getElementById('serverInfo').textContent = s.serverName + ' · ' + s.serverMac;
        document.getElementById('statsRow1').textContent = 'No active audio stream.';
        document.getElementById('statsRow2').textContent = micLat ? ('Mic → post-processed ' + micLat.totalMs.toFixed(1) + ' ms (' + micLat.sourceName + ' ' + micLat.sourceMs.toFixed(1) + ' + model ' + micLat.modelMs.toFixed(1) + ' + DSP ' + micLat.dspMs.toFixed(1) + ' ms)') : '';
        return;
    }
    document.getElementById('clientInfo').textContent = s.clientName;
    document.getElementById('clientAddress').textContent = s.clientAddress;
    var security = document.getElementById('sessionSecurity');
    security.textContent = s.encrypted ? 'DTLS ENCRYPTED' : 'NOT ENCRYPTED';
    security.className = 'encryption-badge' + (s.encrypted ? '' : ' unencrypted');
    document.getElementById('micOwnerLabel').textContent = s.localMic ? 'PC microphone' : s.micOwner ? 'Mic return · ' + s.micOwner : 'No phone microphone assigned';
    document.getElementById('rttValue').textContent = s.clientRttMs > 0 ? s.clientRttMs.toFixed(0) + ' ms' : '—';
    document.getElementById('uptimeValue').textContent = formatUptime(s.uptimeSeconds);
    [['send',s.captureReady === false ? 0 : s.peakLevel],['recv',(s.localMic || s.micOwner) ? s.micPeakLevel : 0]].forEach(function(entry) {
        var level = Math.max(0, Math.min(entry[1] || 0, 1));
        var fill = document.getElementById(entry[0] + 'LevelFill');
        fill.style.setProperty('--lvl', (level * 100).toFixed(1) + '%');
        document.getElementById(entry[0] + 'Meter').setAttribute('aria-valuenow', Math.round(level * 100));
    });
    document.getElementById('statsRow1').textContent = s.packetsSent + ' packets · ' + formatBytes(s.bytesSent) + ' · ' + s.kbps.toFixed(0) + ' kbps · Sequence ' + s.sequenceNum;
    var row = document.getElementById('statsRow2');
    row.textContent = 'Peak ' + s.peakLevel.toFixed(3) + (s.peakLevel > .95 ? ' · Clipping risk' : '') + (s.voiceQos ? ' · Wi-Fi voice priority' : '') + (s.kernelTap ? ' · Kernel tap active' : '') + (micLat ? ' · Mic → DSP ' + micLat.totalMs.toFixed(1) + ' ms' : '');
    row.className = 'stats-row' + (s.peakLevel > .95 ? ' hot' : '');
}
function renderPeerRole(row) {
    var status = row.querySelector('.peer-role-status');
    var current = 'Current access: ' + (row._confirmedRole === 'admin' ? 'Admin. PC settings and media controls enabled.' : 'Read only. PC settings and media controls blocked.');
    status.dataset.state = row._rolePending ? 'pending' : row._roleFeedback ? 'error' : 'confirmed';
    var requested = row._requestedRole === 'admin' ? 'Admin' : 'Read only';
    status.textContent = row._rolePending ? 'Changing to ' + requested + '… ' + current : (row._roleFeedback ? row._roleFeedback + ' ' : '') + current;
    if (row.contains(document.activeElement)) status.scrollIntoView({block:'nearest'});
}
function updatePeers(peers, activeClientName) {
    confirmRemovalState(peers);
    var peerJson = JSON.stringify([peers, activeClientName || '']);
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
)html") +
std::string(R"html(            var isLive = !!(activeClientName && p.name === activeClientName);
            var previous = existing.get(peerId);
            if (previous) {
                var refreshedRole = p.role === 'admin' ? 'admin' : 'reader';
                if (refreshedRole !== previous._confirmedRole) previous._roleFeedback = '';
                previous._confirmedRole = refreshedRole;
                previous.dataset.connected = String(isLive);
                var liveTag = previous.querySelector('.peer-live-tag');
                if (liveTag) liveTag.hidden = !isLive;
                if (!previous._rolePending) previous.querySelector('.peer-role').value = previous._confirmedRole;
                renderPeerRole(previous);
                previous.querySelector('.peer-age').textContent = isLive ? 'Active now' : formatAge(p.lastConnected);
                return;
            }
            var shortId = p.id.length > 12 ? p.id.substring(0, 12) + '...' : p.id;
            var row = document.createElement('div');
            row.className = 'peer-item';
            row.dataset.peerId = peerId;
            row.dataset.connected = String(isLive);
            row._confirmedRole = p.role === 'admin' ? 'admin' : 'reader';
            row.innerHTML =
                '<div class="peer-info">' +
                '<span class="peer-name"><span class="peer-dot" aria-hidden="true"></span>' + escapeHtml(p.name) + '<span class="peer-live-tag"' + (isLive ? '' : ' hidden') + '>Connected</span></span>' +
                '<span class="peer-meta-row"><span class="peer-id">' + escapeHtml(shortId) + '</span><span class="peer-age">' + (isLive ? 'Active now' : formatAge(p.lastConnected)) + '</span></span>' +
                '</div>';
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
            btn.addEventListener('click', function() { confirmPeerRemoval(peerId, peerName); });
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
    for (var i = 0; i < logs.length; i++) html += '<div class="log-line">' + escapeHtml(logs[i]) + '</div>';
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
    var cable = outDevices.some(name => /CABLE Input/i.test(name));
    var nativeMic = outDevices.some(name => name === 'AudioBridge Microphone');
    var unavailable = outputUnavailable(confirmedState) || /unavailable/i.test(selected);
    document.getElementById('outputReadiness').hidden = !unavailable;
    setText('outputReadiness', 'Microphone destination unavailable. Microphone audio is not reaching this destination. Reconnect it or select a working destination; for calls use CABLE Input or AudioBridge Microphone.');
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
function showPairRequest(clientId, name, code) {
    updatePairRequest({pending:true, clientId:clientId, clientName:name, code:code});
}
document.getElementById('manageDevices').addEventListener('click', function() {
)html") +
std::string(R"html(    setText('devicesOutcome', '');
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
document.getElementById('cancelRemoveDevice').addEventListener('click', function() { document.getElementById('removeDeviceDialog').close(); });
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

var lastJitterMs = -1, lastFrameSizeMs = -1, lastRawPcm = null;
var voiceInitialized = false, voiceRecording = false, voiceRevision = -1;
var voiceRequest = 0, voiceRenderTimer, voiceRecordBusy = false;
var voiceAvailable = false, voiceRecoverable = false, voiceSettingsPending = false, voiceEditing = false;
var confirmedVoiceSettings = '', pendingVoiceSettings = '', settingsRequest = 0;
var sampleState = 'empty', lastVoiceState = null, voiceGpuPref = -1, voiceKnownGpus = [], voiceDrawerAutoOpened = false;
var VOICE_MODELS = [
    { name: 'DeepFilterNet3', shortSize: '12 MB', size: '12.3 MB', sizeFull: '12.3 MB · lightweight RNN', delay: 32.0, kind: 'cpu', desc: 'Balanced everyday denoiser (~3% of 1 CPU core). 0% GPU usage.' },
    { name: 'FastEnhancer-L', shortSize: '4.4 MB', size: '4.4 MB', sizeFull: '4.4 MB · ultra-compact', delay: 17.2, kind: 'cpu', desc: 'Lowest-latency 48 kHz neural cleanup (17 ms). 0% GPU usage.' },
    { name: 'DPDFNet8 HR', shortSize: '14 MB', size: '14.2 MB', sizeFull: '14.2 MB · dual-path HR', delay: 50.0, kind: 'cpu', desc: 'Strongest CPU suppression (-92 dB floor). Best while gaming (0% GPU).' },
    { name: 'Inworld Realtime', shortSize: '129 MB', size: '129.4 MB', sizeFull: '129.4 MB · 58.2M params', delay: 52.7, kind: 'gpu', desc: 'Studio Transformer+Mamba isolator; requires a discrete GPU (4 GB+ VRAM).' }
];
function formatGpuLabel(dev) {
    var vram = dev.vramMb >= 1024 ? (dev.vramMb / 1024).toFixed(dev.vramMb >= 10240 ? 0 : 1) + ' GB' : dev.vramMb + ' MB';
    var slow = dev.vramMb > 0 && dev.vramMb < 4096 ? ' · iGPU / slow' : '';
    return 'GPU ' + dev.id + ' · ' + dev.name + (dev.vramMb ? ' (' + vram + slow + ')' : '');
}
function shortGpuName(name) {
    if (!name) return '';
    return name.replace(/^AMD Radeon\s*/i, '').replace(/^NVIDIA GeForce\s*/i, '').replace(/^Intel\(R\)\s*/i, '').trim() || name;
}
function syncVoiceGpuSelect(modelVal) {
    var sel = document.getElementById('voiceGpu');
    if (!sel || !document.createElement) return;
    var isGpuModel = modelVal === 3;
    var sig = (isGpuModel ? 'gpu:' : 'cpu:') + JSON.stringify(voiceKnownGpus);
    if (sel.dataset && sel.dataset.modeSig === sig) {
        if (isGpuModel) sel.value = String(voiceGpuPref);
        return;
    }
    sel.innerHTML = '';
    if (!isGpuModel) {
        var cpuOpt = document.createElement('option');
        cpuOpt.value = String(voiceGpuPref);
        cpuOpt.textContent = 'CPU (0% GPU usage)';
        sel.appendChild(cpuOpt);
        sel.value = String(voiceGpuPref);
    } else {
        var autoOpt = document.createElement('option');
        autoOpt.value = '-1';
        var best = voiceKnownGpus.slice().sort(function(a, b) { return (b.vramMb || 0) - (a.vramMb || 0); })[0];
        autoOpt.textContent = best ? ('Auto · ' + shortGpuName(best.name)) : 'Auto (fastest GPU)';
        sel.appendChild(autoOpt);
        for (var i = 0; i < voiceKnownGpus.length; i++) {
            var opt = document.createElement('option');
            opt.value = String(voiceKnownGpus[i].id);
            opt.textContent = formatGpuLabel(voiceKnownGpus[i]);
            sel.appendChild(opt);
        }
        sel.value = String(voiceGpuPref);
        if (sel.value !== String(voiceGpuPref)) sel.value = '-1';
    }
    if (sel.dataset) sel.dataset.modeSig = sig;
}
function updateVoiceGpuList(devices) {
    voiceKnownGpus = devices || [];
    var modelVal = Number(document.getElementById('voiceModel').value) || 0;
    syncVoiceGpuSelect(modelVal);
}
function applyVoiceSettings(text) {
    var fields = text.split(' ').map(Number);
    document.getElementById('voiceNoise').checked = !!fields[0];
    document.getElementById('voiceStrength').value = fields[1];
    document.getElementById('voiceMakeup').value = fields[6] || 0;
    var modelVal = fields[7] || 0;
    document.getElementById('voiceModel').value = String(modelVal);
    voiceGpuPref = fields.length > 8 && !isNaN(fields[8]) ? fields[8] : -1;
    var gpuSel = document.getElementById('voiceGpu');
    if (gpuSel) {
        syncVoiceGpuSelect(modelVal);
        gpuSel.value = String(voiceGpuPref);
    }
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
    var canSelectBackend = voiceAvailable || voiceRecoverable;
    document.getElementById('voiceNoise').disabled = !canSelectBackend;
    ['voiceGate','voiceLowCut'].forEach(function(id) { document.getElementById(id).disabled = !voiceAvailable; });
    var noiseOn = document.getElementById('voiceNoise').checked;
    var modelVal = Number(document.getElementById('voiceModel').value) || 0;
    var info = VOICE_MODELS[modelVal] || VOICE_MODELS[0];
    document.getElementById('voiceModel').disabled = !canSelectBackend || !noiseOn;
    var gpuSel = document.getElementById('voiceGpu');
    if (gpuSel) {
        syncVoiceGpuSelect(modelVal);
        gpuSel.disabled = !canSelectBackend || !noiseOn || modelVal !== 3;
        gpuSel.title = modelVal === 3 ? (gpuSel.selectedOptions && gpuSel.selectedOptions.length ? gpuSel.selectedOptions[0].textContent : 'GPU rendering device') : 'CPU model (0% GPU usage). Select Inworld Realtime to pick a GPU.';
    }
    var noiseDisabled = !voiceAvailable || !noiseOn;
    document.getElementById('voiceStrength').disabled = noiseDisabled;
    document.getElementById('voiceMakeup').disabled = noiseDisabled;
    threshold.disabled = !voiceAvailable || !document.getElementById('voiceGate').checked;
    hold.disabled = threshold.disabled;
    ['voiceNoise','voiceModel','voiceGpu','voiceStrength','voiceMakeup','voiceGate','voiceThreshold','voiceHold','voiceLowCut'].forEach(function(id) { document.getElementById(id).setAttribute('aria-disabled', String(voiceSettingsPending)); });
    setText('voiceSizeHint', info.size);
    setText('voiceGpuHint', info.kind === 'gpu' ? 'DirectML' : 'CPU only');
    setText('voiceSpecDevice', info.kind === 'gpu' ? 'GPU · DirectML FP16' : 'CPU · 0% GPU');
    setText('voiceSpecSize', info.sizeFull);
    setText('voiceSpecDelay', info.delay.toFixed(1) + ' ms delay');
    setText('voiceSpecDesc', info.desc);
    var activeGpuId = (lastVoiceState && lastVoiceState.activeGpu >= 0) ? lastVoiceState.activeGpu : (voiceGpuPref >= 0 ? voiceGpuPref : 0);
    var gpuSummary = info.kind === 'gpu' ? ('GPU ' + activeGpuId) : 'CPU';
)html") +
std::string(R"html(    setText('voiceModelSummary', info.name + ' · ' + gpuSummary + ' · ' + info.shortSize);
    var makeupVal = Number(document.getElementById('voiceMakeup').value) || 0;
    var tuneParts = [];
    if (makeupVal > 0) tuneParts.push('+' + makeupVal + ' dB');
    tuneParts.push(document.getElementById('voiceGate').checked ? ('Gate ' + threshold.value + '% / ' + (hold.value / 1000).toFixed(1) + ' s') : 'Gate off');
    if (document.getElementById('voiceLowCut').checked) tuneParts.push('80 Hz');
    setText('voiceTuningSummary', tuneParts.join(' · '));
    var strVal = Number(document.getElementById('voiceStrength').value) || 100;
    var secParts = noiseOn ? [info.name, strVal >= 100 ? 'Max' : (strVal + ' dB')] : ['Noise off'];
    secParts.push(document.getElementById('voiceGate').checked ? 'Gate on' : 'Gate off');
    if (makeupVal > 0) secParts.push('+' + makeupVal + ' dB');
    setText('voiceSectionSummary', secParts.join(' · '));
    var compareBody = document.getElementById('voiceCompareBody');
    if (compareBody && compareBody.querySelectorAll) {
        compareBody.querySelectorAll('tr').forEach(function(tr) {
            tr.setAttribute('data-selected', String(Number(tr.getAttribute('data-model')) === modelVal));
        });
    }
    var badge = document.getElementById('voiceDeviceBadge');
    if (badge) {
        var overloaded = !!(lastVoiceState && lastVoiceState.overloaded);
        var activeGpuObj = voiceKnownGpus.find(function(d) { return d.id === (lastVoiceState ? lastVoiceState.activeGpu : -1); });
        var kind = !noiseOn ? 'off' : (overloaded || (lastVoiceState && lastVoiceState.error) ? 'warn' : info.kind);
        badge.setAttribute('data-kind', kind);
        badge.textContent = !noiseOn ? 'Noise off'
            : (lastVoiceState && lastVoiceState.error) ? 'Device error'
            : overloaded ? ('GPU ' + activeGpuId + ' overloaded')
            : info.kind === 'cpu' ? 'CPU · 0% GPU'
            : (activeGpuObj ? shortGpuName(activeGpuObj.name) : 'Discrete GPU');
    }
    renderOperationErrors();
}
function voiceChanged() {
    if (voiceSettingsPending) { applyVoiceSettings(pendingVoiceSettings); voiceLabels(); return; }
    var modelNow = Number(document.getElementById('voiceModel').value) || 0;
    var gpuElNow = document.getElementById('voiceGpu');
    if (modelNow === 3 && gpuElNow && gpuElNow.value !== '') {
        var parsedGpu = Number(gpuElNow.value);
        if (!isNaN(parsedGpu)) voiceGpuPref = parsedGpu;
    }
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
        var makeup = Number(document.getElementById('voiceMakeup').value);
        var model = Number(document.getElementById('voiceModel').value) || 0;
        var gpu = voiceGpuPref;
        if (isNaN(gpu)) gpu = -1;
        if (gpu >= 0) value += ' ' + makeup + ' ' + model + ' ' + gpu;
        else if (model) value += ' ' + makeup + ' ' + model;
        else if (makeup) value += ' ' + makeup;
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
            showOperationError('settings', error.timedOut ? 'Settings response timed out. The outcome is unknown; showing the last confirmed preferences while status refreshes.' : 'Microphone settings could not be applied. Current desktop preferences are shown. Adjust them again to retry.');
        } finally {
            if (request === settingsRequest) { voiceEditing = false; voiceSettingsPending = false; voiceLabels(); }
        }
    }, 250);
}
['voiceNoise','voiceModel','voiceGpu','voiceStrength','voiceMakeup','voiceGate','voiceThreshold','voiceHold','voiceLowCut'].forEach(function(id) {
    document.getElementById(id).addEventListener('input', voiceChanged);
    document.getElementById(id).addEventListener('click', function(event) { if (voiceSettingsPending) event.preventDefault(); });
    document.getElementById(id).addEventListener('keydown', function(event) { if (voiceSettingsPending && event.key !== 'Tab') event.preventDefault(); });
});
document.querySelectorAll('.model-pick').forEach(function(btn) {
    btn.addEventListener('click', function() {
        if (voiceSettingsPending || (!voiceAvailable && !voiceRecoverable)) return;
        document.getElementById('voiceModel').value = btn.getAttribute('data-model');
        if (!document.getElementById('voiceNoise').checked) document.getElementById('voiceNoise').checked = true;
        voiceChanged();
    });
});
function renderSampleAvailability() {
    var voice = lastVoiceState;
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
function formatClipTime(sec) {
    var s = isFinite(sec) && sec > 0 ? Math.floor(sec) : 0;
    return Math.floor(s / 60) + ':' + String(s % 60).padStart(2, '0');
}
function syncStudioPlayer() {
    var audio = document.getElementById('voiceAudio');
    var player = document.getElementById('voicePlayer');
    if (!audio || !player) return;
    player.hidden = !!audio.hidden;
    var playBtn = document.getElementById('voicePlayBtn');
    if (playBtn) {
        playBtn.textContent = audio.paused ? '\u25B6' : '\u275A\u275A';
        playBtn.setAttribute('aria-label', audio.paused ? 'Play sample' : 'Pause sample');
    }
    var dur = isFinite(audio.duration) && audio.duration > 0 ? audio.duration : 0;
    var cur = isFinite(audio.currentTime) ? audio.currentTime : 0;
    setText('voicePlayerTime', formatClipTime(cur) + ' / ' + formatClipTime(dur));
    var seek = document.getElementById('voiceSeek');
    if (seek) {
        var pct = dur > 0 ? (cur / dur) * 100 : 0;
        seek.value = String(Math.round(pct * 10));
        if (seek.style && seek.style.setProperty) seek.style.setProperty('--seek', pct.toFixed(1) + '%');
    }
}
var voicePlayBtnEl = document.getElementById('voicePlayBtn');
if (voicePlayBtnEl) {
    voicePlayBtnEl.addEventListener('click', function() {
        var audio = document.getElementById('voiceAudio');
        if (!audio) return;
        if (audio.paused) audio.play().catch(function() { showOperationError('playback', 'Playback could not start. Press Play to retry.'); });
        else audio.pause();
        syncStudioPlayer();
    });
}
var voiceSeekEl = document.getElementById('voiceSeek');
if (voiceSeekEl) {
    voiceSeekEl.addEventListener('input', function() {
        var audio = document.getElementById('voiceAudio');
        if (!audio || !isFinite(audio.duration) || audio.duration <= 0) return;
        audio.currentTime = (Number(this.value) / 1000) * audio.duration;
        syncStudioPlayer();
    });
}
['play','pause','timeupdate','loadedmetadata','ended'].forEach(function(ev) {
    var audio = document.getElementById('voiceAudio');
    if (audio) audio.addEventListener(ev, syncStudioPlayer);
});
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
        syncStudioPlayer();
        if (!url) { audio.pause(); audio.onloadedmetadata = null; audio.removeAttribute('src'); audio.load(); sampleState = lastVoiceState && lastVoiceState.duration > 0 ? 'missing' : 'empty'; renderSampleAvailability(); syncStudioPlayer(); return; }
        sampleState = 'loading';
        renderSampleAvailability();
        audio.oncanplay = function() {
            if (request !== voiceRequest) return;
            sampleState = 'available';
            renderSampleAvailability();
            syncStudioPlayer();
        };
        audio.onerror = function() {
            if (request !== voiceRequest) return;
            sampleState = 'failed';
            audio.hidden = true;
            renderSampleAvailability();
            syncStudioPlayer();
            showOperationError('clip', 'This sample could not be decoded. Reload it, select Original, or record a new sample.');
        };
        audio.src = url;
        audio.onloadedmetadata = function() {
            audio.currentTime = Math.min(position, audio.duration || 0);
            syncStudioPlayer();
)html") +
std::string(R"html(            if (playing) audio.play().catch(function() { showOperationError('playback', 'Playback could not resume. Press Play in the sample player to retry.'); });
        };
        audio.load();
        clearOperationError('clip');
    } catch (error) {
        if (request !== voiceRequest) return;
        audio.pause(); audio.hidden = true; audio.onloadedmetadata = null; audio.removeAttribute('src'); audio.load();
        sampleState = 'failed';
        renderSampleAvailability();
        syncStudioPlayer();
        showOperationError('clip', 'The sample could not be loaded. Use Reload sample to retry.');
    }
}
document.getElementById('voiceSource').addEventListener('change', function() { voiceLoadClip(); });
document.getElementById('retrySample').addEventListener('click', function() { voiceLoadClip(); });
document.getElementById('voiceRecord').addEventListener('click', async function() {
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
    if (voice.gpuDevices) updateVoiceGpuList(voice.gpuDevices);
    if (!voiceInitialized || (!voiceEditing && !voiceSettingsPending)) {
        confirmedVoiceSettings = voice.settings;
        applyVoiceSettings(voice.settings);
        voiceInitialized = true;
    }
    voiceAvailable = !!voice.ready && !voice.error;
    voiceRecoverable = !!voice.recoverable;
    voiceLabels();
    document.getElementById('voiceCard').setAttribute('data-unavailable', String(!voiceAvailable));
    document.getElementById('effectsUnavailable').hidden = !voice.error;
    var restoreDetails = document.getElementById('effectsRestoreDetails');
    if (restoreDetails) restoreDetails.hidden = !!voice.recoverable;
    if (voice.error && voice.recoverable) {
        setText('effectsUnavailableTitle', 'Selected rendering device unavailable');
        setText('effectsUnavailableDescription', 'Unprocessed microphone audio is passing through. Choose another Rendering device or Model below to recover immediately.');
    } else if (!bridgeFailed) {
        setText('effectsUnavailableTitle', 'Effects unavailable · preferences inactive');
        setText('effectsUnavailableDescription', 'Saved preferences below are inactive. Unprocessed microphone audio is sent to the destination.');
    }
    document.getElementById('processingAvailability').hidden = voice.ready || !!voice.error;
    var recordingEnded = voiceRecording && !voice.recording;
    var restoreRecordingFocus = recordingEnded && document.getElementById('recordingStrip').contains(document.activeElement);
    voiceRecording = voice.recording;
    var audio = document.getElementById('voiceAudio');
    audio.controls = !voice.recording;
    if (voice.recording) { audio.pause(); audio.hidden = true; syncStudioPlayer(); }
    var button = document.getElementById('voiceRecord');
    button.textContent = voice.recording ? 'Stop recording' : 'Record sample';
    button.setAttribute('aria-pressed', String(voice.recording));
    button.disabled = !voice.recording && (!s.localMic && (!s.connected || !s.micOwner));
    button.setAttribute('aria-disabled', String(voiceRecordBusy || button.disabled));
    document.getElementById('recordingStrip').hidden = !voice.recording;
    document.body.setAttribute('data-recording', String(voice.recording));
    document.getElementById('persistentStop').disabled = false;
    document.getElementById('persistentStop').setAttribute('aria-disabled', String(voiceRecordBusy));
    if (restoreRecordingFocus) {
        if (compactLayout.matches && activeWorkspace !== 'microphone') { activeWorkspace = 'microphone'; renderWorkspace(false); }
        button.focus(); button.scrollIntoView({block:'nearest'});
    }
    setText('recordingStripTitle', (s.localMic ? 'Recording PC microphone · ' : 'Recording phone microphone · ') + voice.duration.toFixed(1) + ' s');
    setText('recordingStripStatus', operationErrors.recording || 'Recording stops at 10 seconds.');
    var readiness = s.localMic ? 'PC microphone is active. ' + (outputUnavailable(s) ? 'Destination unavailable. Samples can still be recorded here; choose a working destination in Audio I/O.' : 'Verify delivery in your calling app.') : !s.connected ? 'Connect your phone to test its microphone.' : !s.micOwner ? 'No phone microphone assigned. Enable the mic in your phone app to begin.' : 'Mic assigned: ' + s.micOwner + '. ' + (outputUnavailable(s) ? 'Destination unavailable. Incoming audio can still be recorded here; choose a working return destination in Audio I/O.' : 'Check Mic received for incoming audio; verify delivery in your calling app.');
    setText('micReadiness', readiness);
    setText('routeMicStatus', s.localMic || !s.connected || !s.micOwner ? readiness : 'Mic assigned: ' + s.micOwner + '. The meter shows reception, not calling-app delivery.');
    var sampleSource = document.getElementById('voiceSource');
    var switchToOriginal = !voice.recording && voice.duration > 0 && !voice.processed && sampleSource.value === '1';
    if (switchToOriginal) sampleSource.value = '0';
    sampleSource.disabled = voice.recording || voice.duration === 0;
    if (voice.recording) sampleState = 'empty';
    renderSampleAvailability();
    document.getElementById('voiceTime').textContent = voice.duration > 0 || voice.recording ? voice.duration.toFixed(1) + ' / 10 s' : '10-second mic check';
    document.getElementById('voiceStatus').setAttribute('data-error', String(!!voice.error));
    document.getElementById('voiceStatus').setAttribute('data-overload', String(!!voice.overloaded && !voice.error));
    var modelDrawer = document.getElementById('voiceModelDrawer');
    if (modelDrawer && (voice.recoverable || voice.overloaded)) {
        if (!voiceDrawerAutoOpened) { modelDrawer.open = true; voiceDrawerAutoOpened = true; }
    } else if (!voice.recoverable && !voice.overloaded) {
        voiceDrawerAutoOpened = false;
    }
    var activeModel = Number(document.getElementById('voiceModel').value) || 0;
    var activeGpuObj = (voice.gpuDevices || []).find(function(d) { return d.id === voice.activeGpu; });
    var gpuTag = activeGpuObj ? ' · ' + activeGpuObj.name : '';
    var modelName = activeModel === 1 ? 'FastEnhancer-L'
        : activeModel === 2 ? 'DPDFNet8 HR'
        : activeModel === 3 ? 'Inworld Realtime (GPU' + gpuTag + ')'
        : 'DeepFilterNet3';
    var modelStatus = activeModel === 1 ? 'FastEnhancer-L adds 17 ms model delay, plus buffering.'
        : activeModel === 2 ? 'DPDFNet8 HR adds 50 ms model delay, plus buffering.'
        : activeModel === 3 ? 'Inworld Realtime (GPU' + gpuTag + ') adds 53 ms model delay, plus buffering.'
        : 'DeepFilterNet3 adds 32 ms model delay, plus buffering.';
    var micLat = computeMicLatency(s);
    var liveLatText = micLat ? ('Mic → post-processed: ' + micLat.totalMs.toFixed(1) + ' ms (' + micLat.sourceName + ' ' + micLat.sourceMs.toFixed(1) + ' + ' + (document.getElementById('voiceNoise').checked ? ('model ' + micLat.modelMs.toFixed(1) + ' + ') : '') + 'DSP ' + micLat.dspMs.toFixed(1) + ' ms) · ' + (document.getElementById('voiceNoise').checked ? modelName : 'Noise removal off.')) : '';
    var overloadText = voice.overloaded ? ((activeGpuObj ? ('GPU ' + activeGpuObj.id + ' (' + activeGpuObj.name + ')') : 'Selected GPU') + ' is too slow for real time (' + (voice.rtf ? voice.rtf.toFixed(1) + 'x — ' : '') + 'dropping audio). Switch Rendering device to a discrete GPU or pick a CPU model like DPDFNet8 HR.') : '';
    setText('voiceStatus', voice.error ? 'Effects unavailable; incoming microphone audio passes through unprocessed. ' + voice.error :
        (voice.ready ? (overloadText || liveLatText || (document.getElementById('voiceNoise').checked ? modelStatus : 'Noise removal off. Gate and rumble controls still apply.')) : 'Loading voice models. Preferences are inactive until ready.'));
    var preview = document.getElementById('voicePreviewStatus');
    preview.setAttribute('data-error', String(!!voice.previewError));
    setText('voicePreviewStatus', voice.previewError ? 'Could not process recording: ' + voice.previewError :
        (voice.recording ? 'Recording your microphone. Speak normally; recording stops at 10 seconds.' :
        (voice.rendering ? 'Applying your effects to the sample…' : (voice.duration > 0 ? (voice.processed && sampleState === 'available' ? 'Recording complete. Switch versions to compare. Playback uses your PC’s default output.' : 'Playback uses your PC’s default output. Your original sample is preserved.') :
        (s.localMic || s.connected ? 'Record a sample, then compare Original and Processed. Samples stay in memory until you close AudioBridge.' : 'Connect your phone and enable its mic, then record a sample to compare.')))));
    if ((voiceRevision !== voice.revision || switchToOriginal) && !voice.recording) { voiceRevision = voice.revision; voiceLoadClip(); }
}
function reconcileControls(s) {
    if (s.localInputs) {
        var source = document.getElementById('localInput');
)html") +
std::string(R"html(        var signature = JSON.stringify([s.localInputs, !!s.localMicEnabled, s.localMicId || '', s.localMicName || '']);
        if (source.dataset.inputs !== signature) {
            var previous = source.value;
            source.innerHTML = '';
            s.localInputs.forEach(function(entry) {
                var option = document.createElement('option');
                option.value = entry[0];
                option.textContent = entry[1];
                source.appendChild(option);
            });
            if (s.localMicEnabled && s.localMicId && !source.querySelector('option[value="' + CSS.escape(s.localMicId) + '"]')) {
                var waiting = document.createElement('option');
                waiting.value = s.localMicId;
                waiting.textContent = (s.localMicName || 'Selected microphone') + ' (unavailable)';
                waiting.disabled = true;
                source.appendChild(waiting);
            }
            source.value = previous;
            if (!source.value && source.options.length) source.selectedIndex = 0;
            source.dataset.inputs = signature;
        }
        if (s.localMicEnabled) source.value = s.localMicId;
        var toggle = document.getElementById('localMic');
        toggle.checked = !!s.localMicEnabled;
        toggle.disabled = !s.localMicEnabled && !source.options.length;
        source.disabled = !source.options.length;
        source.setAttribute('data-inactive', String(!s.localMicEnabled));
        setText('localMicStatus', !source.options.length && !s.localMicEnabled ? 'No usable PC microphone found. AudioBridge devices and the recording side of your microphone destination are hidden to prevent feedback.' :
            !s.localMicEnabled ? 'Off. The phone microphone is sent to the destination above.' :
            s.localMic ? 'On. This microphone goes through Audio Processing to the destination above; phone microphone audio is ignored.' :
            (s.localMicError || 'Waiting for this microphone.') + ' Phone microphone audio is used until it is available.');
    }
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
        setText('effectsUnavailableDescription', 'Saved preferences below are inactive. Unprocessed microphone audio is sent to the destination.');
        document.getElementById('bridgeNotice').hidden = true;
        document.getElementById('workspaceControls').disabled = false;
        document.getElementById('btnApprove').disabled = false;
        document.getElementById('btnDeny').disabled = false;
        document.getElementById('sessionOverview').hidden = false;
        document.getElementById('streamSummary').textContent = (s.rawPcm ? 'PCM' : 'Opus') + ' · ' + frameLabel(s.frameSizeMs);
        reconcileOperationOutcomes(s);
        renderAppUpdate(s.update);
        updateStatusCard(s);
        updateSectionSummaries(s);
        updateVoice(s);
        updatePairRequest(s.pairRequest);
        updatePeers(s.peers, s.connected ? s.clientName : '');
        updateLogs(s.logs);
        if (!operationPending.routing && !operationPending.localMic && !operationPending.output && !operationPending.transport) reconcileControls(s);
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
        setText('routeMicStatus', 'Microphone status is unknown until desktop controls reconnect.');
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
var appUpdateState = {};
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
    var openUpd = document.getElementById('openAppUpdates');
    if (openUpd) openUpd.setAttribute('data-available', String(state.phase === 'available'));
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
