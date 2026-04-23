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



function resolveTargetRange(targetLow, targetHigh, targetRange) {

  const minTarget = Number(targetLow);

  const maxTarget = Number(targetHigh);



  if (!Number.isNaN(minTarget) && !Number.isNaN(maxTarget)) {

    return { minTarget, maxTarget };

  }



  if (typeof targetRange === 'string') {

    const match = targetRange.match(/(\d+)\s*-\s*(\d+)/);

    if (match) {

      return {

        minTarget: Number(match[1]),

        maxTarget: Number(match[2])

      };

    }

  }



  return {

    minTarget: NaN,

    maxTarget: NaN

  };

}



function updateMoistureBar(moisture, targetLow, targetHigh, targetRange) {

  const fill = document.getElementById('moistureFill');

  const marker = document.getElementById('moistureMarker');

  const dryLabel = document.getElementById('labelDry');

  const idealLabel = document.getElementById('labelIdeal');

  const wetLabel = document.getElementById('labelWet');



  if (!fill || !marker || !dryLabel || !idealLabel || !wetLabel) {

    return;

  }



  const currentMoisture = Number(moisture);

  const { minTarget, maxTarget } = resolveTargetRange(targetLow, targetHigh, targetRange);



  const safePercent = Number.isNaN(currentMoisture)

    ? 0

    : Math.min(100, Math.max(0, currentMoisture));



  fill.style.width = `${safePercent}%`;

  marker.style.left = `${safePercent}%`;



  dryLabel.classList.remove('active');

  idealLabel.classList.remove('active');

  wetLabel.classList.remove('active');



  if (Number.isNaN(currentMoisture) || Number.isNaN(minTarget) || Number.isNaN(maxTarget)) {

    return;

  }



  if (currentMoisture < minTarget) {

    dryLabel.classList.add('active');

  } else if (currentMoisture > maxTarget) {

    wetLabel.classList.add('active');

  } else {

    idealLabel.classList.add('active');

  }

}



function getCareAdvice(moisture, targetLow, targetHigh, targetRange) {

  const currentMoisture = Number(moisture);

  const { minTarget, maxTarget } = resolveTargetRange(targetLow, targetHigh, targetRange);



  if ([currentMoisture, minTarget, maxTarget].some((value) => Number.isNaN(value))) {

    return {

      title: 'Waiting for data...',

      hint: 'Live care guidance will appear when the device sends valid values.'

    };

  }



  if (currentMoisture < minTarget) {

    return {

      title: 'Needs watering',

      hint: `Moisture is below the ideal ${minTarget}-${maxTarget}% range.`

    };

  }



  if (currentMoisture > maxTarget) {

    return {

      title: 'Too wet now',

      hint: `Moisture is above the ideal ${minTarget}-${maxTarget}% range. Let soil dry a bit.`

    };

  }



  return {

    title: 'Plant is in ideal range',

    hint: `Current moisture is healthy for the ${minTarget}-${maxTarget}% target band.`

  };

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

    setText('requiredMoisture', data.targetRange ?? '--');

    setText('lastChange', formatSeconds(data.lastChangeSec));

    setText('uptime', formatMinutes(data.uptimeMin));

    updateMoistureBar(data.moisture, data.targetLow, data.targetHigh, data.targetRange);



    const advice = getCareAdvice(data.moisture, data.targetLow, data.targetHigh, data.targetRange);

    setText('careAdvice', advice.title);

    setText('careHint', advice.hint);



    connection.textContent = `Updated: ${new Date().toLocaleTimeString()}`;

    connection.className = '';

  } catch (error) {

    connection.textContent = `Connection issue: ${error.message}`;

    connection.className = 'error';

  }

}



refreshRemoteStatus();

setInterval(refreshRemoteStatus, 3000);

