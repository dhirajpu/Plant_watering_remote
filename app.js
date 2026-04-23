function buildRemoteUrl() {
  const config = window.PLANT_APP_CONFIG;
  let url = `${config.firebaseBaseUrl}${config.devicePath}.json`;
 
  if (config.authToken) {
    url += `?auth=${encodeURIComponent(config.authToken)}`;
  }
 
  return url;
}
 
function setText(id, value) {
  document.getElementById(id).textContent = value;
}
 
function formatSeconds(seconds) {
  if (seconds == null || Number.isNaN(Number(seconds))) {
    return '--';
  }
 
  const totalSeconds = Number(seconds);
  if (totalSeconds < 60) {
    return `${totalSeconds}s ago`;
  }
 
  const minutes = Math.floor(totalSeconds / 60);
  const remainingSeconds = totalSeconds % 60;
  return `${minutes}m ${remainingSeconds}s ago`;
}
 
function formatMinutes(minutes) {
  if (minutes == null || Number.isNaN(Number(minutes))) {
    return '--';
  }
 
  const totalMinutes = Number(minutes);
  if (totalMinutes < 60) {
    return `${totalMinutes} min`;
  }
 
  const hours = Math.floor(totalMinutes / 60);
  const remainingMinutes = totalMinutes % 60;
  return `${hours}h ${remainingMinutes}m`;
}
 
async function refreshRemoteStatus() {
  const connection = document.getElementById('connection');
 
  try {
    const response = await fetch(buildRemoteUrl(), { cache: 'no-store' });
    if (!response.ok) {
      throw new Error(`HTTP ${response.status}`);
    }
 
    const data = await response.json();
    if (!data) {
      throw new Error('No device data yet');
    }
 
    setText('moisture', `${data.moisture ?? '--'}%`);
    setText('status', data.status ?? '--');
    setText('pump', data.pump ? 'ON' : 'OFF');
    setText('fault', data.fault ? 'YES' : 'NO');
    setText('sensorHealth', data.sensorHealth ?? '--');
    setText('raw', data.raw ?? '--');
    setText('ip', data.ip ?? '--');
    setText('targetRange', data.targetRange ?? '--');
    setText('lastChange', formatSeconds(data.lastChangeSec));
    setText('uptime', formatMinutes(data.uptimeMin));
 
    connection.textContent = `Updated: ${new Date().toLocaleTimeString()}`;
    connection.className = '';
  } catch (error) {
    connection.textContent = `Connection issue: ${error.message}`;
    connection.className = 'error';
  }
}
 
refreshRemoteStatus();
setInterval(refreshRemoteStatus, 3000);
