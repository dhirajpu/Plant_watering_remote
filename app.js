function buildRemoteUrl() {
  const config = window.PLANT_APP_CONFIG;
  let url = `${config.firebaseBaseUrl}${config.devicePath}.json`;
  if (config.authToken) url += `?auth=${encodeURIComponent(config.authToken)}`;
  return url;
}

function buildWeatherUrl() {
  return `https://api.open-meteo.com/v1/forecast?latitude=12.9716&longitude=77.5946&current=temperature_2m,weather_code&timezone=auto`;
}

const DEFAULT_REMOTE_STALE_MS = 20000;
let lastRemoteSignature = '';
let lastRemoteChangeAtMs = 0;
let hasConfirmedLiveRemote = false;
const MIN_EPOCH_MS = 1577836800000;
const MAX_FUTURE_DRIFT_MS = 24 * 60 * 60 * 1000;

function getRemoteStaleMs() {
  const timeoutSec = Number(window.PLANT_APP_CONFIG?.staleTimeoutSec);
  return (Number.isNaN(timeoutSec) || timeoutSec <= 0) ? DEFAULT_REMOTE_STALE_MS : timeoutSec * 1000;
}

function setText(id, value) {
  const el = document.getElementById(id);
  if (el) el.textContent = value;
}

function setSystemCardState(isOnline) {
  const body = document.body;
  const alert = document.getElementById('systemAlert');
  const sysChip = document.getElementById('systemStatusChip');
  const verifyingChip = document.getElementById('verifyingChip');
  if (body) body.classList.toggle('system-off-mode', !isOnline);
  if (alert) alert.hidden = isOnline;
  if (sysChip) sysChip.classList.toggle('chip-off', !isOnline);
  if (verifyingChip) verifyingChip.hidden = isOnline;
}

function setVerifyingState(isVerifying) {
  const chip = document.getElementById('verifyingChip');
  if (chip) chip.hidden = !isVerifying;
}

function getWeatherDescription(code) {
  const map = {
    0:'Clear sky',1:'Mostly clear',2:'Partly cloudy',3:'Overcast',
    45:'Foggy',51:'Light drizzle',61:'Light rain',63:'Rainy',65:'Heavy rain',
    80:'Rain showers',95:'Thunderstorm'
  };
  return map[code] ?? 'Weather unavailable';
}

function formatSeconds(s) {
  if (s == null || Number.isNaN(Number(s))) return '--';
  const t = Number(s);
  if (t < 60) return `${t}s ago`;
  const m = Math.floor(t / 60), r = t % 60;
  return `${m}m ${r}s ago`;
}

function formatMinutes(m) {
  if (m == null || Number.isNaN(Number(m))) return '--';
  const t = Number(m);
  if (t < 60) return `${t} min`;
  const h = Math.floor(t / 60), r = t % 60;
  return `${h}h ${r}m`;
}

function getMoistureStatus(moisture, targetLow, targetHigh) {
  const m = Number(moisture), lo = Number(targetLow), hi = Number(targetHigh);
  if (Number.isNaN(m) || Number.isNaN(lo) || Number.isNaN(hi)) return 'unknown';
  if (m < lo) return 'dry';
  if (m > hi) return 'wet';
  return 'ok';
}

function getStatusEmoji(status, watering) {
  if (watering) return '💧';
  if (status === 'Sensor Fault' || status === 'Fault') return '⚠️';
  if (status === 'Check Tank' || status === 'Water Issue') return '🚱';
  if (status === 'Wet' || status === 'wet') return '💦';
  if (status === 'OK' || status === 'Monitoring') return '✅';
  if (status === 'Dry' || status === 'Need Water') return '🌵';
  return '🌱';
}

function buildPlantCard(plant, index) {
  const moisture = Number(plant.moisture);
  const moistureStatus = getMoistureStatus(plant.moisture, plant.targetLow, plant.targetHigh);
  const isWatering = Boolean(plant.watering);
  const hasFault = Boolean(plant.fault);
  const hasWaterIssue = Boolean(plant.waterIssue);
  const emoji = getStatusEmoji(plant.status, isWatering);

  const statusClass = hasFault ? 'status-fault'
    : hasWaterIssue ? 'status-water-issue'
    : isWatering ? 'status-watering'
    : moistureStatus === 'dry' ? 'status-dry'
    : moistureStatus === 'wet' ? 'status-wet'
    : 'status-ok';

  const barPercent = Number.isNaN(moisture) ? 0 : Math.min(100, Math.max(0, moisture));
  const barClass = moistureStatus === 'dry' ? 'bar-dry' : moistureStatus === 'wet' ? 'bar-wet' : 'bar-ok';

  return `
    <div class="plant-card ${statusClass}">
      <div class="plant-card-header">
        <div class="plant-name-row">
          <span class="plant-emoji">${emoji}</span>
          <span class="plant-name">${plant.name ?? `Plant ${index + 1}`}</span>
        </div>
        <span class="plant-status-badge">${plant.status ?? '--'}</span>
      </div>

      <div class="plant-moisture-row">
        <span class="plant-moisture-value">${Number.isNaN(moisture) ? '--' : moisture}%</span>
        <span class="plant-target-range">${plant.targetRange ?? '--'}</span>
      </div>

      <div class="plant-bar-track">
        <div class="plant-bar-fill ${barClass}" style="width:${barPercent}%"></div>
        <div class="plant-bar-marker" style="left:${barPercent}%"></div>
      </div>
      <div class="plant-bar-labels">
        <span class="${moistureStatus === 'dry' ? 'bar-label-active' : ''}">Dry</span>
        <span class="${moistureStatus === 'ok' ? 'bar-label-active' : ''}">Ideal</span>
        <span class="${moistureStatus === 'wet' ? 'bar-label-active' : ''}">Wet</span>
      </div>

      <div class="plant-metrics">
        <div class="plant-metric">
          <span class="plant-metric-label">Valve</span>
          <span class="plant-metric-value ${isWatering ? 'value-on' : 'value-off'}">${isWatering ? 'OPEN' : 'CLOSED'}</span>
        </div>
        <div class="plant-metric">
          <span class="plant-metric-label">Sensor</span>
          <span class="plant-metric-value">${plant.sensorHealth ?? '--'}</span>
        </div>
        <div class="plant-metric">
          <span class="plant-metric-label">Raw</span>
          <span class="plant-metric-value">${plant.raw ?? '--'}</span>
        </div>
        <div class="plant-metric">
          <span class="plant-metric-label">Changed</span>
          <span class="plant-metric-value">${formatSeconds(plant.lastChangeSec)}</span>
        </div>
      </div>

      ${hasWaterIssue ? `<div class="plant-alert">Water supply issue - check tank or pipe</div>` : ''}
      ${plant.fault && !hasWaterIssue ? `<div class="plant-alert">Sensor fault detected</div>` : ''}
    </div>
  `;
}

function renderPlants(plants) {
  const grid = document.getElementById('plantsGrid');
  if (!grid) return;
  if (!plants || plants.length === 0) {
    grid.innerHTML = '<div class="plant-card-placeholder">No plant data available.</div>';
    return;
  }
  grid.innerHTML = plants.map((p, i) => buildPlantCard(p, i)).join('');
}

function renderOfflinePlants() {
  const grid = document.getElementById('plantsGrid');
  if (grid) grid.innerHTML = '<div class="plant-card-placeholder">System offline. Waiting for device...</div>';
}

function buildRemoteSignature(data) {
  const plants = data.plants ?? [];
  return [data.updatedAtMs, plants.map(p => `${p.moisture}|${p.watering}|${p.status}`).join(',')].join('|');
}

function getRemoteAgeMs(data) {
  const updatedAtMs = Number(data.updatedAtMs);
  const nowMs = Date.now();
  const valid = !Number.isNaN(updatedAtMs) && updatedAtMs >= MIN_EPOCH_MS && updatedAtMs <= nowMs + MAX_FUTURE_DRIFT_MS;
  return valid ? Math.max(0, nowMs - updatedAtMs) : NaN;
}

function getRemoteDataState(data) {
  const ageMsFromHeartbeat = getRemoteAgeMs(data);
  if (!Number.isNaN(ageMsFromHeartbeat)) {
    hasConfirmedLiveRemote = true;
    return { isStale: ageMsFromHeartbeat > getRemoteStaleMs(), ageMs: ageMsFromHeartbeat };
  }
  const nowMs = Date.now();
  const signature = buildRemoteSignature(data);
  if (!lastRemoteSignature) {
    lastRemoteSignature = signature;
    lastRemoteChangeAtMs = nowMs;
    return { isStale: !hasConfirmedLiveRemote, ageMs: NaN };
  }
  if (signature !== lastRemoteSignature) {
    lastRemoteSignature = signature;
    lastRemoteChangeAtMs = nowMs;
    hasConfirmedLiveRemote = true;
    return { isStale: false, ageMs: 0 };
  }
  if (!hasConfirmedLiveRemote) return { isStale: true, ageMs: NaN };
  const ageMsFromSignature = nowMs - lastRemoteChangeAtMs;
  return { isStale: ageMsFromSignature > getRemoteStaleMs(), ageMs: ageMsFromSignature };
}

async function refreshWeather() {
  try {
    const response = await fetch(buildWeatherUrl(), { cache: 'no-store' });
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    const data = await response.json();
    const curr = data?.current;
    if (!curr) throw new Error('No weather data');
    const temp = Number(curr.temperature_2m);
    setText('outsideTemp', Number.isNaN(temp) ? '--C' : `${Math.round(temp)}C`);
    setText('weatherSummary', getWeatherDescription(curr.weather_code));
    setText('weatherUpdated', `Updated: ${new Date().toLocaleTimeString()}`);
  } catch (e) {
    setText('outsideTemp', '--C');
    setText('weatherSummary', `Weather unavailable: ${e.message}`);
    setText('weatherUpdated', 'Weather update failed');
  }
}

async function refreshRemoteStatus() {
  try {
    const shouldShowVerifying = !hasConfirmedLiveRemote && !lastRemoteSignature;
    setVerifyingState(shouldShowVerifying);

    const response = await fetch(buildRemoteUrl(), { cache: 'no-store' });
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    const data = await response.json();
    if (!data) throw new Error('No device data yet');

    const remoteState = getRemoteDataState(data);
    const stale = remoteState.isStale;
    const ageSec = Math.floor(remoteState.ageMs / 1000);

    if (stale) {
      setText('systemPower', 'OFF');
      setText('systemHeartbeat', Number.isFinite(ageSec) ? `Last heartbeat ${ageSec}s ago` : 'Waiting for live update');
      setText('connection', 'System offline');
      setSystemCardState(false);
      renderOfflinePlants();
      setVerifyingState(false);
      return;
    }

    // Device info
    setText('ip', data.ip ?? '--');
    setText('uptime', formatMinutes(data.uptimeMin));
    setText('totalPlants', data.totalPlants ?? (data.plants?.length ?? '--'));
    setText('lastUpdated', new Date().toLocaleTimeString());
    setText('systemPower', 'ON');
    setText('systemHeartbeat', `Last heartbeat ${ageSec}s ago`);
    setSystemCardState(true);
    setVerifyingState(false);

    const connection = document.getElementById('connection');
    if (connection) {
      connection.textContent = `Updated: ${new Date().toLocaleTimeString()}`;
      connection.className = '';
    }

    // Render per-plant cards
    renderPlants(data.plants ?? []);

  } catch (e) {
    setVerifyingState(false);
    setText('systemPower', 'OFF');
    setText('systemHeartbeat', 'No heartbeat from device');
    setSystemCardState(false);
    renderOfflinePlants();
    const connection = document.getElementById('connection');
    if (connection) {
      connection.textContent = `Connection issue: ${e.message}`;
      connection.className = 'error';
    }
  }
}

refreshWeather();
refreshRemoteStatus();
setInterval(refreshWeather, 600000);
setInterval(refreshRemoteStatus, 3000);
