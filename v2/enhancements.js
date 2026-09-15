(function(){
  const COLORS=['#0f766e','#2563eb','#d97706','#7c3aed','#dc2626'];
  const baseRenderStatus=window.renderStatus;
  const baseGet=window.get;
  const baseRows=window.rows;
  let connectivityHistory=[];
  let telemetry=[];
  let currentStatus=null;

  function decoratePlants(){
    document.querySelectorAll('.plant-card').forEach((card,i)=>{
      const color=COLORS[i%COLORS.length];
      card.style.borderLeft=`5px solid ${color}`;
      const name=card.querySelector('.plant-name');
      if(name&&!card.querySelector('.plant-condition-icon')){
        const icon=document.createElement('span');
        icon.className='plant-condition-icon';
        icon.textContent='🌱';
        name.parentNode.insertBefore(icon,name);
      }
      const state=card.querySelector('.meta-grid .meta:nth-child(2) strong');
      const moisture=card.querySelector('.moisture');
      const rawState=(state?.textContent||'').toLowerCase();
      const m=Number((moisture?.textContent||'').replace('%',''));
      let icon='🌱';
      if(card.querySelector('.fault'))icon='⚠️';
      else if(rawState.includes('watering'))icon='💦';
      else if(rawState.includes('soaking'))icon='🫧';
      else if(rawState==='idle')icon='🌱';
      else if(m<30)icon='🥀';
      const ci=card.querySelector('.plant-condition-icon'); if(ci)ci.textContent=icon;
      if(moisture)moisture.style.color=color;
      const bar=card.querySelector('.bar>div'); if(bar)bar.style.background=color;
    });
  }

  function updateWeather(s){
    const w=document.getElementById('weatherStatus'),h=document.getElementById('weatherHint');
    if(!w||!h)return;
    if(!s.weatherValid){w.textContent='Unavailable';h.textContent='Sensor-only AUTO mode';return;}
    const p=Number(s.weatherRainProbability||0),mm=Number(s.weatherRainMm6h||0);
    if(s.weatherDelayAuto){w.textContent=`🌧️ Rain ${p}%`;h.textContent=`~${mm.toFixed(1)} mm / 6h · AUTO delayed`;}
    else{w.textContent=`☀️ Rain ${p}%`;h.textContent=`~${mm.toFixed(1)} mm / 6h · AUTO allowed`;}
  }

  window.renderStatus=function(s){
    currentStatus=s;
    baseRenderStatus(s);
    updateWeather(s);
    decoratePlants();
  };

  window.renderChart=function(){
    const el=document.getElementById('chart');
    if(!telemetry.length){el.innerHTML='<div class="empty">No telemetry yet.</div>';return;}
    const plants=telemetry[telemetry.length-1]?.plants||[];
    const W=700,H=230,pad=25;
    let paths='';
    plants.forEach((_,pi)=>{
      const pts=telemetry.map((r,idx)=>{
        const x=pad+(idx/Math.max(1,telemetry.length-1))*(W-pad*2);
        const m=Number(r.plants?.[pi]?.moisture||0);
        const y=H-pad-(m/100)*(H-pad*2);
        return `${x.toFixed(1)},${y.toFixed(1)}`;
      }).join(' ');
      paths+=`<polyline points="${pts}" fill="none" stroke="${COLORS[pi%COLORS.length]}" stroke-width="3"/>`;
    });
    el.innerHTML=`<svg viewBox="0 0 ${W} ${H}" preserveAspectRatio="none"><line x1="${pad}" y1="${pad}" x2="${pad}" y2="${H-pad}" stroke="#d1d5db"/><line x1="${pad}" y1="${H-pad}" x2="${W-pad}" y2="${H-pad}" stroke="#d1d5db"/>${paths}</svg><div class="chart-legend">${plants.map((p,i)=>`<span style="color:${COLORS[i%COLORS.length]}">● ${esc(p.name||`Plant ${i+1}`)}</span>`).join('')}</div>`;
  };

  function renderConnectivity(){
    const el=document.getElementById('connectivityHistory'); if(!el)return;
    if(!connectivityHistory.length){el.innerHTML='<div class="empty">No connectivity events yet.</div>';return;}
    el.innerHTML=connectivityHistory.map(x=>{
      const t=String(x.type||'EVENT');
      const icon=t==='WIFI_CONNECTED'?'🟢':t==='WIFI_DISCONNECTED'?'🔴':'🔄';
      const when=x.epochMs?new Date(Number(x.epochMs)).toLocaleString():'Time unavailable';
      return `<div class="history-item connectivity-item"><strong>${icon} ${esc(t.replaceAll('_',' '))}</strong><small>${esc(when)} · ${esc(x.reason||'')}</small></div>`;
    }).join('');
  }

  window.loadHistory=async function(){
    try{
      const [t,h,c]=await Promise.all([baseGet('/history/telemetry'),baseGet('/history/watering'),baseGet('/history/connectivity')]);
      telemetry=baseRows(t).slice(-48);
      window.wateringHistory=baseRows(h).slice(-30).reverse();
      connectivityHistory=baseRows(c).slice(-40).reverse();
      window.renderChart();
      if(window.renderHistory)window.renderHistory();
      renderConnectivity();
    }catch(e){console.warn('history',e)}
  };

  const oldRefresh=window.refresh;
  window.refresh=async function(){await oldRefresh();if(currentStatus)updateWeather(currentStatus);decoratePlants();};

  setTimeout(()=>{window.refresh();window.loadHistory();},250);
  setInterval(()=>{window.refresh();},5000);
  setInterval(()=>{window.loadHistory();},60000);
})();
