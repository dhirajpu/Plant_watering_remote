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
    setText('raw', data.raw ?? '--');
    setText('ip', data.ip ?? '--');
 
    connection.textContent = `Updated: ${new Date().toLocaleTimeString()}`;
    connection.className = '';
  } catch (error) {
    connection.textContent = `Connection issue: ${error.message}`;
    connection.className = 'error';
  }
}
 
refreshRemoteStatus();
setInterval(refreshRemoteStatus, 3000);