const API = '/api/radio';
const EVENTS = '/api/radio/events';

let eventSource = null;
let currentFreq = 0;
let isScanning = false;

// Initialize on page load
function init() {
    document.getElementById('wifi-status').textContent = 'Loading...';
    setupControls();
    loadInitialData();
    try {
        connectSSE();
    } catch (e) {
        console.error('SSE init failed:', e);
    }
}

// Connect to Server-Sent Events stream
function connectSSE() {
    eventSource = new EventSource(EVENTS);

    eventSource.onopen = function () {
        const el = document.getElementById('wifi-status');
        if (el) {
            el.textContent = 'Verbunden';
            el.classList.add('connected');
        }
    };

    eventSource.addEventListener('status', (e) => {
        const s = JSON.parse(e.data);
        document.getElementById('radio-state').textContent =
            ['Off', 'Idle', 'Tuning', 'Seeking', 'Scanning'][s.state] || '?';
        updateFreq(s.frequency);
        updateVolDisplay(s.volume);

        if (document.getElementById('mute-btn')) {
            document.getElementById('mute-btn').textContent =
                s.muted ? 'Unmute' : 'Mute';
        }
    });

    eventSource.addEventListener('stations', (e) => {
        renderStations(JSON.parse(e.data));
    });

    eventSource.addEventListener('scan', (e) => {
        try {
            const scan = JSON.parse(e.data);
            isScanning = scan.scanning;
            updateScanUI(scan.progress, scan.count);
        } catch (e) {}
    });

    eventSource.onerror = () => {
        const el = document.getElementById('wifi-status');
        if (el) el.textContent = 'Getrennt';
        setTimeout(connectSSE, 5000);
    };
}

// Load initial status and stations from server
function loadInitialData() {
    fetch(`${API}/status`)
        .then(r => r.json())
        .then(s => {
            updateFreq(s.frequency);
            updateVolDisplay(s.volume);
        })
        .catch(e => console.error('status fetch error:', e));

    fetch(`${API}/stations`)
        .then(r => r.json())
        .then(renderStations)
        .catch(e => console.error('stations fetch error:', e));
}

// Setup button click handlers
function setupControls() {
    console.log('[RADIO] binding buttons');

    const scanBtn = document.getElementById('scan-btn');
    if (scanBtn) {
        scanBtn.onclick = () => {
            isScanning = true;
            updateScanUI(0, 0);
            fetch(`${API}/scan/start`, { method: 'POST' })
                .catch(console.error);
        };
    }

    const cancelBtn = document.getElementById('cancel-scan-btn');
    if (cancelBtn) {
        cancelBtn.onclick = () => {
            fetch(`${API}/scan/cancel`, { method: 'POST' })
                .catch(console.error);
        };
    }

    const volSlider = document.getElementById('volume');
    if (volSlider) {
        volSlider.onchange = (e) => {
            const percent = parseInt(e.target.value);
            fetch(`${API}/volume`, {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ volume: percent })
            }).catch(console.error);
        };
    }

    const muteBtn = document.getElementById('mute-btn');
    if (muteBtn) {
        muteBtn.onclick = () => {
            const muted = muteBtn.textContent === 'Mute';
            fetch(`${API}/mute`, {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ muted: muted })
            }).catch(console.error);
        };
    }

    const settingsBtn = document.getElementById('settings-btn');
    if (settingsBtn) {
        settingsBtn.onclick = openSettings;
    }
}

// Settings modal
function openSettings() {
    document.getElementById('settings-modal').classList.add('show');
}

function closeSettings() {
    document.getElementById('settings-modal').classList.remove('show');
}

function saveSettings() {
    const ssid = document.getElementById('wifi-ssid').value;
    const pass = document.getElementById('wifi-pass').value;
    console.log('Settings saved (placeholder)', ssid, pass);
    closeSettings();
}

// Update current frequency display
function updateFreq(freq) {
    currentFreq = freq;
}

// Update volume display (0-100%)
function updateVolDisplay(percent) {
    const el = document.getElementById('volume-value');
    const slider = document.getElementById('volume');

    if (el) {
        el.textContent = percent + '%';
    }

    if (slider && slider.value != percent) {
        slider.value = percent;
    }
}

// Update scan progress UI
function updateScanUI(progress, count) {
    const scanBtn = document.getElementById('scan-btn');
    const cancelBtn = document.getElementById('cancel-scan-btn');
    const progressDiv = document.getElementById('scan-progress');

    if (isScanning) {
        if (scanBtn) scanBtn.classList.add('hidden');
        if (cancelBtn) cancelBtn.classList.remove('hidden');
        if (progressDiv) progressDiv.style.display = 'block';
    } else {
        if (scanBtn) scanBtn.classList.remove('hidden');
        if (cancelBtn) cancelBtn.classList.add('hidden');
        if (progressDiv) progressDiv.style.display = 'none';
    }

    if (progressDiv) {
        document.getElementById('scan-bar').style.width = progress + '%';
        document.getElementById('scan-text').textContent = progress + '% - ' + count + ' Sender';
    }
}

// Render station list
function renderStations(stations) {
    const list = document.getElementById('station-list');
    if (!list) return;

    list.innerHTML = (stations || [])
        .map(st => {
            const isCurrentStation = st.frequency === currentFreq;
            const stationName = st.programService || ((st.frequency / 100).toFixed(2) + ' MHz');
            const freq = (st.frequency / 100).toFixed(2);
            const stereo = st.stereo ? 'Stereo' : 'Mono';
            const rssi = 'RSSI ' + st.rssi;

            // +/- shift buttons tune to the station, then fine-shift by 0.1 MHz
            return `<div class="station-item${isCurrentStation ? ' current' : ''}" onclick="selectStation(${st.frequency})">
                <div class="station-head">
                    <div class="station-name">${stationName}</div>
                    <div class="station-nudge">
                        <button class="nudge-btn" onclick="event.stopPropagation();nudgeFrom(${st.frequency},-10)">−</button>
                        <button class="nudge-btn" onclick="event.stopPropagation();nudgeFrom(${st.frequency},10)">+</button>
                    </div>
                </div>
                <div class="station-meta">
                    <span>${freq} MHz</span>
                    <span>${stereo}</span>
                    <span>${rssi}</span>
                </div>
            </div>`;
        })
        .join('');
}

// Tune to the given frequency first, then shift by step (e.g. 10 = +0.1 MHz)
function nudgeFrom(baseFreq, step) {
    fetch(`${API}/frequency`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ frequency: baseFreq })
    }).then(() => {
        fetch(`${API}/nudge`, {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ step: step })
        }).catch(console.error);
    }).catch(console.error);
}

// Select and tune to a station
function selectStation(freq) {
    fetch(`${API}/frequency`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ frequency: freq })
    }).catch(console.error);
}

// Bind init to DOMContentLoaded
document.addEventListener('DOMContentLoaded', init);
if (document.readyState !== 'loading') {
    init();
}