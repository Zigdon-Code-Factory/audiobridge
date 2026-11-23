// Exercise the embedded production UI's bridge wiring without audio hardware.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const source = fs.readFileSync(path.join(__dirname, '../windows/src/gui_html.h'), 'utf8');
const html = [...source.matchAll(/R"html\(([\s\S]*?)\)html"/g)].map(m => m[1]).join('');
const script = html.match(/<script>([\s\S]*?)<\/script>/)[1];
const elements = new Map();
function element(id) {
    if (!elements.has(id)) elements.set(id, {
        value: '', checked: false, textContent: '', disabled: false, paused: true,
        currentTime: 0, duration: 3, events: {}, style: {}, options: [],
        classList: { toggle() {}, add() {}, remove() {} },
        listeners: {},
        getAttribute(name) { return this.attributes?.[name] ?? null; },
        setAttribute(name, value) { (this.attributes ??= {})[name] = String(value); },
        addEventListener(name, fn) {
            (this.listeners[name] ??= []).push(fn);
            this.events[name] = (...args) => {
                return Promise.all(this.listeners[name].map(listener => listener.apply(this, args)));
            };
        },
        pause() { this.paused = true; },
        play() { this.paused = false; return Promise.resolve(); },
        load() {}, removeAttribute(name) { delete this[name]; },
        replaceChildren() {}, contains() { return false; }, scrollIntoView() {}, focus() {},
    });
    return elements.get(id);
}
for (const match of html.matchAll(/<[^>]+\bid="([^"]+)"[^>]*>/g)) {
    element(match[1]).attributes = Object.fromEntries(
        [...match[0].matchAll(/([\w-]+)="([^"]*)"/g)].map(attribute => [attribute[1], attribute[2]]));
}
const frameChanges = [];
let timer, sent, recorded, clip = 'data:audio/wav;base64,UklGRg==';
const context = {
    document: { body: element('body'), getElementById: element, querySelectorAll: selector =>
        [...elements.values()].filter(entry => selector.startsWith('.') &&
            (entry.attributes?.class ?? '').split(' ').includes(selector.slice(1))) }, console,
    setInterval() {}, setTimeout(fn) { timer = fn; return 1; }, clearTimeout() {},
    window: {
        matchMedia: () => ({ matches: false, addEventListener() {} }),
        addEventListener() {},
        _voiceSettings: async value => { sent = value; },
        _voiceRecord: async value => { recorded = value; },
        _voiceClip: async () => clip,
        _setFrameSize: value => { frameChanges.push(value); },
    },
};
vm.createContext(context); vm.runInContext(script, context);
async function main() {
    const v = { settings: '1 20 1 35 800 1', ready: true, recording: false, duration: 3,
        processed: true, revision: 1 };
    context.window._tick = async () => ({ connected: true, micOwner: 'Test phone', frameSizeMs: Number(frameChanges.at(-1) || 2),
        voice: { ...v, settings: sent || v.settings, recording: recorded === 1 } });
    context.updateVoice({ connected: true, micOwner: 'Test phone', voice: v });
    assert.equal(element('voiceStrength').value, 20);
    assert.equal(element('voiceModel').value, '0');
    assert.equal(element('voiceThresholdValue').textContent, '35%');
    assert.equal(element('voiceHoldValue').textContent, '0.8 s');
    assert.equal(element('voiceLowCut').checked, true);
    element('voiceModel').value = '3';
    element('voiceModel').events.input(); await timer();
    assert.equal(sent, '1 20 1 35 800 1 0 3');
    context.updateVoice({ connected: true, micOwner: 'Test phone', voice: { ...v, settings: sent, activeGpu: 0, gpuDevices: [{ id: 0, name: 'RX 6900 XT', vramMb: 16337 }, { id: 1, name: 'Radeon Graphics', vramMb: 2021 }] } });
    assert.match(element('voiceStatus').textContent, /Inworld Realtime \(GPU/);
    assert.equal(element('voiceGpu').disabled, false);
    element('voiceGpu').value = '1';
    element('voiceGpu').events.input(); await timer();
    assert.equal(sent, '1 20 1 35 800 1 0 3 1');
    context.updateVoice({ connected: true, localMic: true, clientRttMs: 4, voice: { ...v, settings: sent, active: true, modelDelayMs: 52.7, processMs: 8.2, pipelineMs: 11.4, activeGpu: 0, gpuDevices: [{ id: 0, name: 'RX 6900 XT', vramMb: 16337 }] } });
    assert.match(element('voiceStatus').textContent, /Mic → post-processed: 74\.1 ms \(Local mic 10\.0 \+ model 52\.7 \+ DSP 11\.4 ms\)/);
    assert.match(element('voiceModelSummary').textContent, /Inworld Realtime · GPU 0 · 129 MB/);
    context.updateVoice({ connected: true, localMic: true, voice: { ...v, settings: sent, active: true, overloaded: true, rtf: 13.0, activeGpu: 1, gpuDevices: [{ id: 1, name: 'AMD Radeon(TM) Graphics', vramMb: 2021 }] } });
    assert.match(element('voiceStatus').textContent, /too slow for real time.*13\.0x/);
    assert.equal(element('voiceModelDrawer').open, true);
    element('voiceGpu').value = '-1';
    element('voiceGpu').events.input(); await timer();
    element('voiceModel').value = '0';
    element('voiceNoise').checked = false;
    element('voiceNoise').events.input(); await timer();
    assert.equal(sent, '0 20 1 35 800 1');
    assert.equal(element('voiceModel').disabled, true);
    assert.equal(element('voiceGpu').disabled, true);
    assert.equal(element('voiceStrength').disabled, true);
    await element('voiceRecord').events.click.call(element('voiceRecord'));
    assert.equal(recorded, 1);
    assert.deepEqual(frameChanges, []);
    context.updateVoice({ connected: true, voice: { ...v, recording: true } });
    assert.equal(element('voiceSource').disabled, true);
    assert.equal(element('voiceAudio').controls, false);
    await element('voiceRecord').events.click.call(element('voiceRecord'));
    assert.equal(recorded, 0);
    assert.deepEqual(frameChanges, []);
    await element('frameBtn10').events.click.call(element('frameBtn10'));
    assert.deepEqual(frameChanges, ['10']);
    const audio = element('voiceAudio');
    audio.currentTime = 1.5; audio.paused = false;
    await context.voiceLoadClip(); audio.onloadedmetadata();
    assert.equal(audio.currentTime, 1.5); assert.equal(audio.paused, false);
    clip = null; await context.voiceLoadClip();
    assert.equal(audio.paused, true); assert.equal(audio.src, undefined);
    context.updateVoice({ connected: false, voice: { ...v, error: 'missing model' } });
    assert.equal(element('voiceRecord').disabled, true);
    assert.match(element('voiceStatus').textContent, /passes through.*missing model/);
    console.log('Voice UI: saved controls, bridge settings, record/stop, playback switching and errors passed');
}
main().catch(error => { console.error(error); process.exitCode = 1; });
