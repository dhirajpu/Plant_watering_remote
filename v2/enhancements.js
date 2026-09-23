(function(){
  const COLORS=['#0f766e','#2563eb','#d97706','#7c3aed','#dc2626'];
  const WEATHER_URL='https://api.open-meteo.com/v1/forecast?latitude=12.9716&longitude=77.5946&hourly=precipitation_probability,precipitation&forecast_days=2&timezone=Asia%2FKolkata';
  const baseRenderStatus=window.renderStatus;
  const baseGet=window.get;
  const baseRows=window.rows;
  let connectivityHistory=[];
  let telemetry=[];
  let currentStatus=null;
  let weatherBrowserValid=false;
  let weatherBrowserProbability=0;
  let weatherBrowserMm=0;

  function decoratePlants(){
    document.querySelectorAll('.plant-card').forEach((card,i)=>{
      const color=COLORS[i%COLORS.length];
      const p=currentStatus?.plants?.[i]||{};
      const low=Number(p.targetLow??0),high=Number(p.targetHigh??100);
      const moisture=card.querySelector('.moisture');
      const state=card.querySelector('.meta-grid .meta:nth-child(2) strong');
      const rawState=(state?.textContent||'').toLowerCase();
      const m=Number((moisture?.textContent||'').replace('%',''));
      card.style.borderLeft=`5px solid ${color}`;
      let icon='🌱';
      let conditionClass='plant-healthy';
      let conditionLabel='OPTIMAL';
      if(p.fault||card.querySelector('.fault')){icon='⚠️';conditionClass='plant-fault';conditionLabel='FAULT';}
      else if(rawState.includes('watering')){icon='💦';conditionClass='plant-watering';conditionLabel='WATERING';}
      else if(rawState.includes('soaking')){icon='🫧';conditionClass='plant-soaking';conditionLabel='SOAKING';}
      else if(m<=low){icon='🥀';conditionClass='plant-dry';conditionLabel='DRY';}
      else if(m>=high){icon='🌿';conditionClass='plant-wet';conditionLabel='WET';}
      card.classList.remove('plant-healthy','plant-wet','plant-dry','plant-watering','plant-soaking','plant-fault');
      card.classList.add(conditionClass);
      const name=card.querySelector('.plant-name');
      if(name&&!card.querySelector('.plant-condition-icon')){
        const iconEl=document.createElement('span');
        iconEl.className='plant-condition-icon';
        name.parentNode.insertBefore(iconEl,name);
      }
      const iconEl=card.querySelector('.plant-condition-icon');
      if(iconEl){iconEl.textContent=icon;iconEl.title=conditionLabel;}
      let labelEl=card.querySelector('.plant-condition-label');
      if(!labelEl){
        labelEl=document.createElement('div');
        labelEl.className='plant-condition-label';
        const top=card.querySelector('.plant-top');
        const moistureWrap=moisture?.parentElement;
        if(moistureWrap)moistureWrap.insertBefore(labelEl,moisture);
        else if(top)top.insertAdjacentElement('afterend',labelEl);
        else card.prepend(labelEl);
      }
      labelEl.className=`plant-condition-label ${conditionClass}`;
      labelEl.textContent=`${icon} ${conditionLabel}`;
      labelEl.title=conditionLabel==='DRY'?`Moisture is at or below Low (${low}%)`:conditionLabel==='WET'?`Moisture is at or above High (${high}%)`:conditionLabel==='OPTIMAL'?`Moisture is between Low (${low}%) and High (${high}%)`:conditionLabel;
      if(moisture)moisture.style.color=color;
      const bar=card.querySelector('.bar>div'); if(bar)bar.style.background=color;
    });
  }

  function updateWeatherCard(){
    const w=document.getElementById('weatherStatus'),h=document.getElementById('weatherHint');
    if(!w||!h)return;
    if(!weatherBrowserValid){w.textContent='Unavailable';h.textContent='Sensor-only AUTO mode';return;}
    if(weatherBrowserProbability>=60&&weatherBrowserMm>=1){w.textContent=`🌧️ Rain ${weatherBrowserProbability}%`;h.textContent=`~${weatherBrowserMm.toFixed(1)} mm / 6h · AUTO may delay`;}
    else{w.textContent=`🌤️ Rain ${weatherBrowserProbability}%`;h.textContent=`~${weatherBrowserMm.toFixed(1)} mm / 6h · AUTO allowed`;}
  }

  async function refreshBrowserWeather(){
    try{
      const r=await fetch(WEATHER_URL,{cache:'no-store'}); if(!r.ok)throw new Error(`HTTP ${r.status}`);
      const data=await r.json(); const a=data.hourly||{}; const probs=a.precipitation_probability||[],rain=a.precipitation||[];
      const now=new Date(); let hour=now.getHours(); let maxP=0,mm=0;
      for(let off=0;off<6;off++){const idx=hour+off;if(idx>=probs.length||idx>=rain.length)break;maxP=Math.max(maxP,Number(probs[idx]||0));mm+=Math.max(0,Number(rain[idx]||0));}
      weatherBrowserProbability=maxP;weatherBrowserMm=mm;weatherBrowserValid=true;updateWeatherCard();
    }catch(e){weatherBrowserValid=false;updateWeatherCard();console.warn('Bengaluru weather',e)}
  }

  window.renderStatus=function(s){
    currentStatus=s;
    baseRenderStatus(s);
    updateWeatherCard();
    decoratePlants();
  };

  window.renderChart=function(){
    const el=document.getElementById('chart');
    if(!telemetry.length){el.innerHTML='<div class="empty">No telemetry yet.</div>';return;}
    const plants=telemetry[telemetry.length-1]?.plants||[]; const W=700,H=230,pad=25; let paths='';
    plants.forEach((_,pi)=>{
      const pts=telemetry.map((r,idx)=>{const x=pad+(idx/Math.max(1,telemetry.length-1))*(W-pad*2);const m=Number(r.plants?.[pi]?.moisture||0);const y=H-pad-(m/100)*(H-pad*2);return `${x.toFixed(1)},${y.toFixed(1)}`}).join(' ');
      paths+=`<polyline points="${pts}" fill="none" stroke="${COLORS[pi%COLORS.length]}" stroke-width="3"/>`;
    });
    el.innerHTML=`<svg viewBox="0 0 ${W} ${H}" preserveAspectRatio="none"><line x1="${pad}" y1="${pad}" x2="${pad}" y2="${H-pad}" stroke="#d1d5db"/><line x1="${pad}" y1="${H-pad}" x2="${W-pad}" y2="${H-pad}" stroke="#d1d5db"/>${paths}</svg><div class="chart-legend">${plants.map((p,i)=>`<span style="color:${COLORS[i%COLORS.length]}">● ${esc(p.name||`Plant ${i+1}`)}</span>`).join('')}</div>`;
  };

  function renderConnectivity(){
    const el=document.getElementById('connectivityHistory');if(!el)return;
    if(!connectivityHistory.length){el.innerHTML='<div class="empty">No connectivity events yet.</div>';return;}
    el.innerHTML=connectivityHistory.map(x=>{const t=String(x.type||'EVENT');const icon=t==='WIFI_CONNECTED'?'🟢':t==='WIFI_DISCONNECTED'?'🔴':'🔄';const when=x.epochMs?new Date(Number(x.epochMs)).toLocaleString():'Time unavailable';return `<div class="history-item connectivity-item"><strong>${icon} ${esc(t.replaceAll('_',' '))}</strong><small>${esc(when)} · ${esc(x.reason||'')}</small></div>`}).join('');
  }

  window.loadHistory=async function(showLoader=false){
    const run=async()=>{
      try{
        const [t,h,c]=await Promise.all([baseGet('/history/telemetry'),baseGet('/history/watering'),baseGet('/history/connectivity')]);
        telemetry=baseRows(t).slice(-48);window.wateringHistory=baseRows(h).slice(-30).reverse();connectivityHistory=baseRows(c).slice(-40).reverse();
        window.renderChart();if(window.renderHistory)window.renderHistory();renderConnectivity();return true;
      }catch(e){console.warn('history',e);return false}
    };
    return showLoader&&typeof window.withLoader==='function'?window.withLoader('Loading history…',run):run();
  };

  window.refreshBrowserWeather=refreshBrowserWeather;
  const oldRefresh=window.refresh;
  window.refresh=async function(){await oldRefresh();if(currentStatus){decoratePlants();updateWeatherCard();}};
  refreshBrowserWeather();
  setInterval(refreshBrowserWeather,30*60*1000);
})();
