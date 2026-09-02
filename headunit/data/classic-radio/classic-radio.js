// Opel Radio · Classic 1935 frontend
// Uses the same REST/SSE backend as /radio (see data/app.js). This file is
// intentionally self-contained (no shared code with /radio's app.js) so the
// two frontends can evolve independently, as required by issue #8.

const API = '/api/radio';
const EVENTS = '/api/radio/events';
const MIN_MHZ = 87.5;
const MAX_MHZ = 108.0;

let currentFreq = 0;
let currentVolumePercent = 50;
let currentMuted = false;
let stations = [];
let isScanning = false;
let eventSource = null;

// ------------------------------------------------------------------
// Navigation
// ------------------------------------------------------------------
function showScreen(name) {
    document.querySelectorAll('.screen').forEach((el) => {
        el.classList.toggle('active', el.id === 'screen-' + name);
    });
    document.querySelectorAll('.cr-nav-btn').forEach((el) => {
        el.classList.toggle('active', el.dataset.screen === name);
    });
    if (name === 'stations') renderStationList();
}

function setupNav() {
    document.querySelectorAll('.cr-nav-btn').forEach((btn) => {
        btn.addEventListener('click', () => showScreen(btn.dataset.screen));
    });
}

// ------------------------------------------------------------------
// Dial scale (87.5 - 108 MHz)
// ------------------------------------------------------------------
function buildDialScale() {
    const scale = document.getElementById('cr-dial-scale');
    if (!scale) return;
    // Ticks every 0.5 MHz, major tick + label every 2 MHz.
    for (let mhz = MIN_MHZ; mhz <= MAX_MHZ + 0.001; mhz += 0.5) {
        const pct = ((mhz - MIN_MHZ) / (MAX_MHZ - MIN_MHZ)) * 100;
        const isMajor = Math.round(mhz * 10) % 20 === 0; // every 2 MHz
        const tick = document.createElement('div');
        tick.className = 'cr-tick' + (isMajor ? ' major' : '');
        tick.style.left = pct + '%';
        tick.style.height = isMajor ? '55%' : '30%';
        scale.appendChild(tick);

        if (isMajor) {
            const label = document.createElement('div');
            label.className = 'cr-tick-label';
            label.style.left = pct + '%';
            label.textContent = Math.round(mhz);
            scale.appendChild(label);
        }
    }
    // Keep the pointer element as the last child (it's positioned absolute
    // regardless of DOM order, but this avoids ticks visually overlapping it).
    const pointer = document.getElementById('cr-pointer');
    if (pointer) scale.appendChild(pointer);
}

function updatePointer(freq10k) {
    const mhz = freq10k / 100;
    const clamped = Math.min(MAX_MHZ, Math.max(MIN_MHZ, mhz));
    const pct = ((clamped - MIN_MHZ) / (MAX_MHZ - MIN_MHZ)) * 100;
    const pointer = document.getElementById('cr-pointer');
    if (pointer) pointer.style.left = pct + '%';
    const freqEl = document.getElementById('cr-freq');
    if (freqEl) freqEl.textContent = mhz.toFixed(2);
}

// ------------------------------------------------------------------
// Status / SSE
// ------------------------------------------------------------------
function applyStatus(s) {
    currentFreq = s.frequency;
    updatePointer(s.frequency);

    const psEl = document.getElementById('cr-ps');
    // programService/radioText are RDS text broadcast by the tuned FM
    // station - untrusted input, so textContent (not innerHTML) is
    // required to avoid script injection via a crafted RDS broadcast.
    if (psEl) psEl.textContent = (s.programService && s.programService.trim()) || '\u00A0';
    const rtEl = document.getElementById('cr-rt');
    if (rtEl) rtEl.textContent = (s.radioText && s.radioText.trim()) || '\u00A0';

    const rds = s.rds || {};
    setLamp('lamp-stereo', !!s.stereo);
    setLamp('lamp-rds', !!rds.synced);

    const rssiPct = Math.max(0, Math.min(100, Math.round((s.rssi / 75) * 100)));
    // Gauge sweeps -55deg (empty) to +55deg (full scale) like a small
    // analog field-strength instrument rather than a modern progress bar.
    const needle = document.getElementById('cr-rssi-needle');
    if (needle) needle.style.transform = 'rotate(' + (rssiPct / 100 * 110 - 55) + 'deg)';

    currentVolumePercent = s.volume;
    currentMuted = s.muted;
    updateVolumeDisplay();
    updateMuteButton();

    renderPresets();
    highlightCurrentStation();
    applyRdsDetail(s);
}

function setLamp(id, on) {
    const el = document.getElementById(id);
    if (el) el.classList.toggle('on', on);
}

function applyRdsDetail(s) {
    const rds = s.rds || {};
    setText('rds-ps', (s.programService && s.programService.trim()) || '--');
    setText('rds-rt', (s.radioText && s.radioText.trim()) || '--');
    setText('rds-pi', rds.piCode || '--');
    setText('rds-pty', rds.ptyName ? (rds.ptyName + ' (' + rds.pty + ')') : '--');
    setText('rds-tpta', (rds.tp ? 'ja' : 'nein') + ' / ' + (rds.ta ? 'ja' : 'nein'));
    setText('rds-sync', rds.synced ? 'synchron' : 'kein Sync');
    setText('rds-rssi', s.rssi + ' dBµV');
    setText('rds-stereo', s.stereo ? 'Stereo' : 'Mono');
    setText('rds-freq', (s.frequency / 100).toFixed(2) + ' MHz');

    const bler = rds.bler || {};
    ['a', 'b', 'c', 'd'].forEach((block) => {
        const el = document.getElementById('rds-bler-' + block);
        if (!el) return;
        const level = bler[block];
        el.className = 'bler-dot' + (level !== undefined ? ' bler-' + level : '');
    });
}

function setText(id, text) {
    const el = document.getElementById(id);
    if (el) el.textContent = text;
}

function connectSSE() {
    eventSource = new EventSource(EVENTS);

    eventSource.addEventListener('status', (e) => {
        try { applyStatus(JSON.parse(e.data)); } catch (err) { console.error(err); }
    });

    eventSource.addEventListener('stations', (e) => {
        try {
            stations = JSON.parse(e.data) || [];
            renderStationList();
            renderPresets();
            highlightCurrentStation();
        } catch (err) { console.error(err); }
    });

    eventSource.addEventListener('scan', (e) => {
        try {
            const scan = JSON.parse(e.data);
            isScanning = !!scan.scanning;
            updateScanOverlay(scan.progress || 0, scan.count || 0);
        } catch (err) { console.error(err); }
    });

    eventSource.onerror = () => {
        setTimeout(connectSSE, 5000);
    };
}

function loadInitialData() {
    fetch(`${API}/status`).then((r) => r.json()).then(applyStatus).catch(console.error);
    fetch(`${API}/stations`).then((r) => r.json()).then((list) => {
        stations = list || [];
        renderStationList();
        renderPresets();
        highlightCurrentStation();
    }).catch(console.error);
}

// ------------------------------------------------------------------
// Volume knob
// ------------------------------------------------------------------
function updateVolumeDisplay() {
    const valEl = document.getElementById('cr-vol-value');
    if (valEl) valEl.textContent = currentVolumePercent + '%';
    const knob = document.getElementById('knob-volume');
    if (knob) knob.style.transform = 'rotate(' + (currentVolumePercent * 2.7 - 135) + 'deg)';

    const settingsVal = document.getElementById('settings-vol-value');
    if (settingsVal) settingsVal.textContent = currentVolumePercent;
    const settingsSlider = document.getElementById('settings-volume');
    if (settingsSlider && settingsSlider.value != currentVolumePercent) {
        settingsSlider.value = currentVolumePercent;
    }
}

function updateMuteButton() {
    const btn = document.getElementById('tune-mute');
    if (btn) btn.textContent = currentMuted ? 'Unmute' : 'Mute';
    const settingsBtn = document.getElementById('settings-mute');
    if (settingsBtn) settingsBtn.textContent = currentMuted ? 'Unmute' : 'Mute';
}

function setVolume(percent) {
    const clamped = Math.max(0, Math.min(100, percent));
    currentVolumePercent = clamped;
    updateVolumeDisplay();
    fetch(`${API}/volume`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ volume: clamped })
    }).catch(console.error);
}

function setMuted(muted) {
    currentMuted = muted;
    updateMuteButton();
    fetch(`${API}/mute`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ muted })
    }).catch(console.error);
}

// ------------------------------------------------------------------
// Fine tuning
// ------------------------------------------------------------------
function nudge(step) {
    fetch(`${API}/nudge`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ step })
    }).catch(console.error);
}

function tuneTo(freq10k) {
    fetch(`${API}/frequency`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ frequency: freq10k })
    }).catch(console.error);
}

// ------------------------------------------------------------------
// Presets (V1: first 6 favorites, per the plan doc - a dedicated
// presets API is deferred to a later phase)
// ------------------------------------------------------------------
function renderPresets() {
    const container = document.getElementById('cr-presets');
    if (!container) return;
    const favorites = stations.filter((s) => s.favorite).slice(0, 6);

    container.innerHTML = '';
    for (let i = 0; i < 6; i++) {
        const st = favorites[i];
        const btn = document.createElement('button');
        btn.className = 'cr-preset-btn' + (!st ? ' empty' : '') + (st && st.frequency === currentFreq ? ' current' : '');

        const lamp = document.createElement('span');
        lamp.className = 'cr-preset-lamp';
        const label = document.createElement('span');
        label.className = 'cr-preset-label';
        label.textContent = st ? (st.programService && st.programService.trim() ? st.programService : (st.frequency / 100).toFixed(2)) : (i + 1);

        btn.appendChild(lamp);
        btn.appendChild(label);
        if (st) btn.onclick = () => tuneTo(st.frequency);
        container.appendChild(btn);
    }
}

function highlightCurrentStation() {
    document.querySelectorAll('.cr-station-item').forEach((el) => {
        el.classList.toggle('current', Number(el.dataset.frequency) === currentFreq);
    });
    renderPresets();
}

// ------------------------------------------------------------------
// Station list screen
// ------------------------------------------------------------------
function renderStationList() {
    const list = document.getElementById('cr-station-list');
    if (!list) return;

    // Favorites first, then the rest, each in ascending frequency order.
    const favorites = stations.filter((s) => s.favorite).sort((a, b) => a.frequency - b.frequency);
    const others = stations.filter((s) => !s.favorite).sort((a, b) => a.frequency - b.frequency);
    const ordered = favorites.concat(others);

    list.innerHTML = '';
    ordered.forEach((st) => {
        const li = document.createElement('li');
        li.className = 'cr-station-item' + (st.frequency === currentFreq ? ' current' : '');
        li.dataset.frequency = st.frequency;

        const favBtn = document.createElement('button');
        favBtn.className = 'cr-station-fav' + (st.favorite ? ' active' : '');
        favBtn.textContent = st.favorite ? '★' : '☆';
        favBtn.onclick = (e) => {
            e.stopPropagation();
            toggleFavorite(st.frequency, !st.favorite);
        };

        const main = document.createElement('div');
        main.className = 'cr-station-main';
        const name = document.createElement('div');
        name.className = 'cr-station-name';
        name.textContent = (st.programService && st.programService.trim()) || (st.frequency / 100).toFixed(2) + ' MHz';
        const meta = document.createElement('div');
        meta.className = 'cr-station-meta';
        meta.innerHTML = `<span>${(st.frequency / 100).toFixed(2)} MHz</span><span>${st.stereo ? 'Stereo' : 'Mono'}</span><span>RSSI ${st.rssi}</span>`;
        main.appendChild(name);
        main.appendChild(meta);

        li.appendChild(favBtn);
        li.appendChild(main);
        li.onclick = () => tuneTo(st.frequency);

        list.appendChild(li);
    });
}

function toggleFavorite(freq10k, favorite) {
    fetch(`${API}/favorite`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ frequency: freq10k, favorite })
    }).then(() => {
        fetch(`${API}/stations`).then((r) => r.json()).then((list) => {
            stations = list || [];
            renderStationList();
            renderPresets();
        });
    }).catch(console.error);
}

// ------------------------------------------------------------------
// Scan dialog
// ------------------------------------------------------------------
function updateScanOverlay(progress, count) {
    const overlay = document.getElementById('scan-overlay');
    if (overlay) overlay.classList.toggle('hidden', !isScanning);
    const fill = document.getElementById('scan-fill');
    if (fill) fill.style.width = progress + '%';
    const text = document.getElementById('scan-text');
    if (text) text.textContent = progress + ' % · ' + count + ' Sender';
    if (!isScanning) {
        // Scan just finished - refresh the station list and jump there.
        fetch(`${API}/stations`).then((r) => r.json()).then((list) => {
            stations = list || [];
            renderStationList();
            renderPresets();
        }).catch(console.error);
    }
}

function startScan() {
    isScanning = true;
    updateScanOverlay(0, 0);
    document.getElementById('scan-overlay').classList.remove('hidden');
    fetch(`${API}/scan/start`, { method: 'POST' }).catch(console.error);
}

function cancelScan() {
    fetch(`${API}/scan/cancel`, { method: 'POST' }).catch(console.error);
    isScanning = false;
    document.getElementById('scan-overlay').classList.add('hidden');
}

// ------------------------------------------------------------------
// Clock screen (V1: browser system time - see plan doc "Phase 2" for
// the eventual server-side GET /api/time source)
// ------------------------------------------------------------------
function buildClockFace() {
    const face = document.getElementById('cr-clock-ticks');
    if (!face) return;
    const radius = 100; // matches .cr-clock-big diameter/2 in classic-radio.css
    for (let i = 0; i < 12; i++) {
        const angle = (i * 30) * (Math.PI / 180);
        const isQuarter = i % 3 === 0;
        const tick = document.createElement('div');
        tick.className = 'cr-clock-tick' + (isQuarter ? ' major' : '');
        const dist = radius - (isQuarter ? 16 : 12);
        tick.style.left = (radius + Math.sin(angle) * dist) + 'px';
        tick.style.top = (radius - Math.cos(angle) * dist) + 'px';
        tick.style.transform = 'translate(-50%, -50%) rotate(' + (i * 30) + 'deg)';
        face.appendChild(tick);

        if (isQuarter) {
            const num = document.createElement('div');
            num.className = 'cr-clock-num';
            num.textContent = i === 0 ? '12' : String(i * 1);
            const numDist = radius - 32;
            num.style.left = (radius + Math.sin(angle) * numDist) + 'px';
            num.style.top = (radius - Math.cos(angle) * numDist) + 'px';
            face.appendChild(num);
        }
    }
}

function tickClock() {
    const now = new Date();
    const h = now.getHours() % 12;
    const m = now.getMinutes();
    const s = now.getSeconds();

    const hourDeg = h * 30 + m * 0.5;
    const minDeg = m * 6 + s * 0.1;
    const secDeg = s * 6;

    setRotation('mini-hand-hour', hourDeg);
    setRotation('mini-hand-min', minDeg);
    setRotation('big-hand-hour', hourDeg);
    setRotation('big-hand-min', minDeg);
    setRotation('big-hand-sec', secDeg);

    const pad = (n) => String(n).padStart(2, '0');
    setText('cr-clock-digital', `${pad(now.getHours())}:${pad(m)}:${pad(s)}`);

    const days = ['Sonntag', 'Montag', 'Dienstag', 'Mittwoch', 'Donnerstag', 'Freitag', 'Samstag'];
    setText('cr-clock-date', `${days[now.getDay()]}, ${pad(now.getDate())}.${pad(now.getMonth() + 1)}.${now.getFullYear()}`);
}

function setRotation(id, deg) {
    const el = document.getElementById(id);
    if (el) el.style.transform = 'rotate(' + deg + 'deg)';
}

// ------------------------------------------------------------------
// Settings screen
// ------------------------------------------------------------------
function setupSettings() {
    const slider = document.getElementById('settings-volume');
    if (slider) {
        slider.oninput = (e) => setVolume(parseInt(e.target.value, 10));
    }
    const muteBtn = document.getElementById('settings-mute');
    if (muteBtn) muteBtn.onclick = () => setMuted(!currentMuted);

    const wifiSave = document.getElementById('settings-wifi-save');
    if (wifiSave) {
        wifiSave.onclick = () => {
            const ssid = document.getElementById('settings-ssid').value;
            const pass = document.getElementById('settings-pass').value;
            const statusEl = document.getElementById('settings-wifi-status');
            if (!ssid) return;
            if (statusEl) statusEl.textContent = 'Verbinde …';
            fetch('/api/wifi/connect', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ ssid, password: pass })
            }).then(() => pollWifiStatus()).catch(console.error);
        };
    }
}

function pollWifiStatus(attempt) {
    attempt = attempt || 0;
    fetch('/api/wifi/status').then((r) => r.json()).then((s) => {
        const statusEl = document.getElementById('settings-wifi-status');
        if (!statusEl) return;
        if (s.state === 'connected') {
            statusEl.textContent = 'Verbunden (' + s.ip + ')';
        } else if (s.state === 'failed') {
            statusEl.textContent = 'Verbindung fehlgeschlagen';
        } else if (attempt < 15) {
            statusEl.textContent = 'Verbinde …';
            setTimeout(() => pollWifiStatus(attempt + 1), 1000);
        } else {
            statusEl.textContent = 'Zeitüberschreitung';
        }
    }).catch(console.error);
}

// ------------------------------------------------------------------
// Control bindings
// ------------------------------------------------------------------
function setupControls() {
    const volMinus = document.getElementById('vol-minus');
    if (volMinus) volMinus.onclick = () => setVolume(currentVolumePercent - 5);
    const volPlus = document.getElementById('vol-plus');
    if (volPlus) volPlus.onclick = () => setVolume(currentVolumePercent + 5);

    const tuneMinus = document.getElementById('tune-minus');
    if (tuneMinus) tuneMinus.onclick = () => nudge(-10);
    const tunePlus = document.getElementById('tune-plus');
    if (tunePlus) tunePlus.onclick = () => nudge(10);
    const tuneMute = document.getElementById('tune-mute');
    if (tuneMute) tuneMute.onclick = () => setMuted(!currentMuted);

    const scanStart = document.getElementById('scan-start-btn');
    if (scanStart) scanStart.onclick = startScan;
    const scanCancel = document.getElementById('scan-cancel-btn');
    if (scanCancel) scanCancel.onclick = cancelScan;
}

// ------------------------------------------------------------------
// Init
// ------------------------------------------------------------------
function init() {
    buildDialScale();
    buildClockFace();
    setupNav();
    setupControls();
    setupSettings();
    loadInitialData();
    connectSSE();
    tickClock();
    setInterval(tickClock, 1000);
}

document.addEventListener('DOMContentLoaded', init);
if (document.readyState !== 'loading') {
    init();
}
