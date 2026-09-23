const CFG=window.PLANT_V2_CONFIG;let controlSessionToken=sessionStorage.getItem('plantV2ControlToken')||'';let controlSessionExpires=Number(sessionStorage.getItem('plantV2ControlExpires')||0);let securityBusy=false;
async function sha256Text(value){const data=new TextEncoder().encode(value);const digest=await crypto.subtle.digest('SHA-256',data);return Array.from(new Uint8Array(digest)).map(b=>b.toString(16).padStart(2,'0')).join('')}
function controlUnlocked(){return !!controlSessionToken&&Date.now()<controlSessionExpires}
function lockControls(){controlSessionToken='';controlSessionExpires=0;sessionStorage.removeItem('plantV2ControlToken');sessionStorage.removeItem('plantV2ControlExpires');document.body.classList.remove('controls-unlocked');toast('Protected controls locked.')}
async function pollNode(path,match,attempts=24,delayMs=250){for(let n=0;n<attempts;n++){const v=await get(path);if(v&&match(v))return v;await new Promise(r=>setTimeout(r,delayMs))}throw new Error('Controller authentication timed out.')}
async function requireControlAuth(reason='Protected action'){
  if(controlUnlocked())return true;
  if(securityBusy)return false;
  securityBusy=true;
  try{
    const password=prompt('🔐 '+reason+' requires the control password.');
    if(password===null)return false;
    if(!password.length){toast('Password is required.');return false}
    const meta=await get('/security/meta');
    if(!meta?.enabled||!meta.salt)throw new Error('Controller security is not initialized.');
    const id=commandId(),clientNonce=await sha256Text(id+'|'+Date.now()+'|'+Math.random());
    await put('/security/request',{id,clientNonce});
    const challenge=await pollNode('/security/challenge',v=>v.id===id&&v.serverNonce);
    const passwordHash=await sha256Text(password+meta.salt);
    const proof=await sha256Text(passwordHash+challenge.serverNonce+clientNonce);
    await put('/security/request',{id,clientNonce,proof});
    const response=await pollNode('/security/response',v=>v.id===id);
    if(!response.ok||!response.token)throw new Error('Invalid password.');
    controlSessionToken=response.token;
    controlSessionExpires=Number(response.expiresAtMs||Date.now()+Number(meta.sessionSec||300)*1000);
    sessionStorage.setItem('plantV2ControlToken',controlSessionToken);
    sessionStorage.setItem('plantV2ControlExpires',String(controlSessionExpires));
    document.body.classList.add('controls-unlocked');
    toast('Controls unlocked for '+Math.max(1,Math.round((controlSessionExpires-Date.now())/60000))+' min.');
    return true;
  }catch(e){toast(e.message||'Authentication failed.');return false}
  finally{securityBusy=false}
}
function scheduleSecurityExpiry(){setInterval(()=>{if(controlSessionToken&&Date.now()>=controlSessionExpires)lockControls()},1000)}
window.lockControls=lockControls;
async function changeControlPassword(){
  if(!(await requireControlAuth('Change Control Password')))return;
  const next=prompt('Enter the new control password (8+ characters):');
  if(next===null)return;
  if(next.length<8){toast('Use at least 8 characters.');return}
  const confirmPassword=prompt('Confirm the new control password:');
  if(confirmPassword!==next){toast('Passwords do not match.');return}
  try{
    const bytes=new Uint8Array(16);crypto.getRandomValues(bytes);
    const newSalt=Array.from(bytes).map(b=>b.toString(16).padStart(2,'0')).join('');
    const newHash=await sha256Text(next+newSalt);
    const id=await sendCommand('change_password',{newSalt,newHash},'Change Password');
    if(id)finishCommand('Control password changed successfully.');
  }catch(e){finishCommand('Failed: '+e.message)}
}
function addSecurityButtons(){
  const hero=document.querySelector('.hero-actions');
  if(hero&&!document.getElementById('lockControlsBtn')){
    const b=document.createElement('button');b.id='lockControlsBtn';b.className='secondary';b.textContent='🔒 Lock Controls';b.onclick=lockControls;hero.appendChild(b);
  }
  const panel=document.getElementById('diagnosticsView');
  if(panel&&!document.getElementById('securityPanel')){
    const section=document.createElement('section');section.className='section';section.id='securityPanel';
    section.innerHTML='<div class="section-head"><div><h2>Control Security</h2><p>Monitoring remains view-only. Control changes require authentication and automatically lock after 5 minutes.</p></div><div class="actions"><button class="secondary" id="changePasswordBtn">Change Control Password</button></div></div>';
    panel.appendChild(section);
    document.getElementById('changePasswordBtn').onclick=changeControlPassword;
  }
}
let lastStatus=null;let settingsEditing=false;let lastLiveAt=0;const DEVICE_STALE_SEC=15;let telemetry=[];let wateringHistory=[];let commandBusy=false;let commandTimer=null;let commandButtons=[];
function url(path){let u=`${CFG.firebaseBaseUrl}${CFG.deviceRoot}${path}.json`;if(CFG.authToken)u+=`?auth=${encodeURIComponent(CFG.authToken)}`;return u}
async function get(path){const controller=new AbortController();const timer=setTimeout(()=>controller.abort(),7000);try{const r=await fetch(url(path),{cache:'no-store',signal:controller.signal});if(!r.ok)throw new Error(`HTTP ${r.status}`);return r.json()}catch(e){if(e.name==='AbortError')throw new Error(`Request timed out: ${path}`);throw e}finally{clearTimeout(timer)}}
async function put(path,data){const r=await fetch(url(path),{method:'PUT',headers:{'Content-Type':'application/json'},body:JSON.stringify(data)});if(!r.ok)throw new Error(`HTTP ${r.status}`);return r.json()}
function esc(v){return String(v??'').replace(/[&<>'\"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;',"'":'&#39;','"':'&quot;'}[c]) )}
function fmtMin(v){const n=Number(v||0);return n<60?`${n} min`:`${Math.floor(n/60)}h ${n%60}m`}
function fmtHistoryDate(v){if(!v)return '';const d=new Date(v);return Number.isNaN(d.getTime())?'':d.toLocaleString(undefined,{year:'numeric',month:'short',day:'2-digit',hour:'2-digit',minute:'2-digit',second:'2-digit'})}
function toast(msg){const e=document.getElementById('toast');e.textContent=msg;e.hidden=false;clearTimeout(toast.t);toast.t=setTimeout(()=>e.hidden=true,3000)}
function commandId(){return `${Date.now()}-${Math.random().toString(16).slice(2)}`}
function setCommandBusy(active,label='Processing…'){commandBusy=active;document.querySelectorAll('.actions button,#emergencyBtn,#resumeBtn').forEach(b=>{if(active){if(!b.disabled){b.dataset.originalText=b.textContent;b.disabled=true}}else if(b.dataset.originalText!==undefined){b.textContent=b.dataset.originalText;delete b.dataset.originalText;b.disabled=false}});if(active){commandButtons=Array.from(document.querySelectorAll('.actions button,#emergencyBtn,#resumeBtn')).filter(b=>b.disabled);commandButtons.forEach(b=>{b.dataset.originalText=b.textContent;b.textContent=label})}}
function stopCommandTimer(){if(commandTimer){clearInterval(commandTimer);commandTimer=null}}
function startCommandCountdown(seconds=8){stopCommandTimer();let remaining=seconds;setCommandBusy(true,`Processing… ${remaining}s`);commandTimer=setInterval(()=>{remaining--;if(remaining<=0){stopCommandTimer();setCommandBusy(false);toast('Command timeout — controller did not confirm it. Please try again.');return}document.querySelectorAll('.actions button,#emergencyBtn,#resumeBtn').forEach(b=>{if(b.dataset.originalText!==undefined)b.textContent=`Processing… ${remaining}s`})},1000)}
function finishCommand(message){stopCommandTimer();setCommandBusy(false);toast(message)}
async function sendCommand(action,extra={},label='Command'){if(commandBusy){toast('Please wait — previous command is still executing.');return null}if(!(await requireControlAuth(label)))return null;const id=commandId();try{startCommandCountdown(8);await put('/command',{id,action,...extra,authToken:controlSessionToken,issuedAtEpochSec:Math.floor(Date.now()/1000)});toast(`${label} sent. Waiting for controller…`);return id}catch(e){finishCommand(`Failed: ${e.message}`);throw e}}
function plantCard(p,i){const m=Math.max(0,Math.min(100,Number(p.moisture)||0));const fault=p.fault?'fault':'good';const watering=p.watering?'Watering':p.soaking?'Soaking':'Idle';const faultReason=String(p.faultReason??'').trim();return `<article class="plant-card" data-i="${i}">
<div class="plant-top"><div><div class="plant-name">${esc(p.name||`Plant ${i+1}`)}</div><small class="${fault}">${p.fault?'Fault detected':'Sensor healthy'}</small>${p.fault&&faultReason?`<div class="fault-reason"><strong>Reason:</strong> ${esc(faultReason)}</div>`:''}</div><span class="mode">${esc(p.mode||'AUTO')}</span></div>
<div class="moisture">${m}%</div><div class="bar"><div style="width:${m}%"></div></div>
<div class="meta-grid"><div class="meta"><span>Target</span><strong>${esc(p.targetRange||`${p.targetLow}-${p.targetHigh}%`)}</strong></div><div class="meta"><span>State</span><strong>${watering}</strong></div><div class="meta"><span>Raw</span><strong>${esc(p.raw)}</strong></div><div class="meta"><span>Last stop</span><strong>${esc(p.lastStopReason||'None')}</strong></div><div class="meta"><span>Today pump</span><strong>${Math.round((p.dailyPumpMs||0)/1000)} sec</strong></div></div>
<div class="actions"><button onclick="waterNow(${i})" ${p.mode==='DISABLED'?'disabled':''}>Water Now</button><button class="secondary" onclick="setMode(${i},'AUTO')">AUTO</button><button class="secondary" onclick="setMode(${i},'MANUAL')">MANUAL</button><button class="secondary" onclick="setMode(${i},'DISABLED')">Disable</button><button class="secondary" onclick="toggleSettings(${i})">Settings</button>${p.fault?`<button class="warning" onclick="clearFault(${i})">Clear Fault</button>`:''}</div>
<div class="settings" id="settings-${i}"><div class="form-grid"><div class="field"><label>Plant name</label><input id="name-${i}" value="${esc(p.name)}"></div><div class="field"><label>Low %</label><input id="low-${i}" type="number" min="0" max="95" value="${p.targetLow}"></div><div class="field"><label>High %</label><input id="high-${i}" type="number" min="1" max="100" value="${p.targetHigh}"></div><div class="field"><label>Burst seconds</label><input id="burst-${i}" type="number" min="1" max="15" value="${Math.round((p.burstMs||3000)/1000)}"></div><div class="field"><label>Soak seconds</label><input id="soak-${i}" type="number" min="10" max="1800" value="${p.soakSec||120}"></div><div class="field"><label>Min interval minutes</label><input id="interval-${i}" type="number" min="1" value="${p.minIntervalMin||60}"></div><div class="field"><label>Mode</label><select id="mode-${i}"><option ${p.mode==='AUTO'?'selected':''}>AUTO</option><option ${p.mode==='MANUAL'?'selected':''}>MANUAL</option><option ${p.mode==='DISABLED'?'selected':''}>DISABLED</option></select></div></div><div class="actions"><button onclick="saveSettings(${i})">Save Settings</button><button class="secondary" onclick="calibrate(${i},'dry')">Record Dry</button><button class="secondary" onclick="calibrate(${i},'wet')">Record Wet</button></div></div>
</article>`}
function renderStatus(s){lastStatus=s;if(settingsEditing)return;const hb=Number(s.heartbeatEpochSec||0);if(hb>0){const age=Math.floor(Date.now()/1000)-hb;if(age>DEVICE_STALE_SEC){renderOffline();return}}lastLiveAt=Date.now();document.getElementById('systemBadge').textContent=s.emergencyStop?'STOPPED':'ONLINE';document.getElementById('systemBadge').className=`badge ${s.emergencyStop?'offline':'online'}`;document.getElementById('systemStatus').textContent=s.emergencyStop?'Emergency Stop':s.systemReady?'Ready':'Starting';document.getElementById('heartbeat').textContent=`Last seen ${new Date((Number(s.heartbeatEpochSec||0)||Math.floor(Date.now()/1000))*1000).toLocaleTimeString()}`;const active=Number(s.activePlantIndex);document.getElementById('activePlant').textContent=active>=0?(s.plants?.[active]?.name||`Plant ${active+1}`):'Pump OFF';document.getElementById('wateringAllowed').textContent=s.wateringAllowed?'Watering allowed':'Safety lock active';document.getElementById('wifiRssi').textContent=`${s.wifiRssi||0} dBm`;document.getElementById('deviceIp').textContent=s.ip||'--';document.getElementById('resetReason').textContent=s.resetReason||'--';document.getElementById('uptime').textContent=`Uptime ${fmtMin(s.uptimeMin)}`;document.getElementById('emergencyBtn').hidden=!!s.emergencyStop;document.getElementById('resumeBtn').hidden=!s.emergencyStop;document.getElementById('plantsGrid').innerHTML=(s.plants||[]).map(plantCard).join('')||'<div class="empty">No plants found.</div>';renderDiagnostics(s);renderOta(s)}
function renderDiagnostics(s){const items=[['Firmware',s.firmware],['Reset reason',s.resetReason],['System ready',s.systemReady?'YES':'NO'],['Watering allowed',s.wateringAllowed?'YES':'NO'],['Emergency stop',s.emergencyStop?'ACTIVE':'No'],['Wi-Fi RSSI',`${s.wifiRssi||0} dBm`],['Device IP',s.ip],['NTP time',s.timeSynced?'Synchronized':'Not synchronized'],['Uptime',fmtMin(s.uptimeMin)],['Active plant',s.activePlantIndex]];document.getElementById('diagnosticsGrid').innerHTML=items.map(x=>`<div class="diag"><span>${esc(x[0])}</span><strong>${esc(x[1]??'--')}</strong></div>`).join('')}
function renderOta(s){const el=document.getElementById('otaPanel');if(!el)return;const ip=String(s.ip||'').trim();if(!ip||ip==='offline'||!s.ota){el.innerHTML='<strong>Local OTA unavailable</strong><p>Use the USB fallback firmware when the controller is not online.</p>';return}const link=`http://${ip}/update`;el.innerHTML=`<strong>${esc(s.firmware||'V2 firmware')}</strong><p>Controller: ${esc(ip)} · Authenticated local OTA is enabled.</p><a class="button-link" href="${link}" target="_blank" rel="noopener" onclick="return openProtectedOta(event,'${ip}')">Open Firmware Update</a><p class="ota-note">Your computer/phone must be on the same Wi-Fi network as the ESP32. Upload the compiled ESP32 <code>.bin</code> file. The controller forces pump and valves OFF during the update and reboots when complete.</p>`}
window.openProtectedOta=async(e,ip)=>{e.preventDefault();if(!(await requireControlAuth('Firmware Update')))return false;window.open(`http://${ip}/update`,'_blank','noopener');return false};
function renderOffline(){document.getElementById('systemBadge').textContent='OFFLINE';document.getElementById('systemBadge').className='badge offline';document.getElementById('systemStatus').textContent='Offline';document.getElementById('heartbeat').textContent='Waiting for V2 firmware heartbeat';renderOta({ip:'offline',ota:false})}
window.waterNow=async i=>{if(commandBusy)return toast('Please wait — previous command is still executing.');const s=prompt('Water for how many seconds?','5');if(s===null)return;const sec=Math.max(1,Math.min(45,Number(s)||5));try{await sendCommand('water_now',{plantIndex:i,durationSec:sec},'Water Now')}catch(e){}}
window.setMode=async(i,mode)=>{try{const id=await sendCommand('set_mode',{plantIndex:i,mode},mode);if(!id)return;toast(`Mode changed to ${mode}`);finishCommand(`Mode changed to ${mode}`)}catch(e){finishCommand(`Failed: ${e.message}`)}};
window.clearFault=async i=>{try{const id=await sendCommand('clear_fault',{plantIndex:i},'Clear Fault');if(id)toast('Clear Fault sent. Waiting for controller…')}catch(e){}};
window.calibrate=async(i,kind)=>{if(!confirm(`Record current sensor reading as ${kind.toUpperCase()} calibration?`))return;try{await sendCommand(`calibrate_${kind}`,{plantIndex:i},`Record ${kind.toUpperCase()}`)}catch(e){}};
window.toggleSettings=async i=>{if(commandBusy)return toast('Please wait — previous command is still executing.');const el=document.getElementById(`settings-${i}`);const opening=!el.classList.contains('open');if(opening&&!(await requireControlAuth('Settings')))return;document.querySelectorAll('.settings.open').forEach(x=>x.classList.remove('open'));el.classList.toggle('open',opening);settingsEditing=opening;if(opening)toast('Auto refresh paused while editing settings.');else{toast('Auto refresh resumed.');refresh()}};
window.saveSettings=async i=>{if(commandBusy)return toast('Please wait — previous command is still executing.');if(!(await requireControlAuth('Save Settings')))return;try{const cfg={name:document.getElementById(`name-${i}`).value.trim(),targetLow:Number(document.getElementById(`low-${i}`).value),targetHigh:Number(document.getElementById(`high-${i}`).value),burstMs:Number(document.getElementById(`burst-${i}`).value)*1000,soakSec:Number(document.getElementById(`soak-${i}`).value),minIntervalMin:Number(document.getElementById(`interval-${i}`).value),mode:document.getElementById(`mode-${i}`).value};if(cfg.targetHigh<=cfg.targetLow)throw new Error('High threshold must be greater than low threshold');const id=await sendCommand('set_config',{plantIndex:i,...cfg},'Save Settings');if(!id)return;settingsEditing=false;finishCommand('Settings saved successfully');await refresh()}catch(e){finishCommand(`Failed: ${e.message}`)}};
function rows(obj){return obj&&typeof obj==='object'?Object.values(obj):[]}
async function loadHistory(){try{const [t,h]=await Promise.all([get('/history/telemetry'),get('/history/watering')]);telemetry=rows(t).slice(-48);wateringHistory=rows(h).slice(-30).reverse();renderChart();renderHistory()}catch(e){console.warn(e)}}
function renderHistory(){const el=document.getElementById('history');if(!wateringHistory.length){el.innerHTML='<div class="empty">No watering history yet.</div>';return}el.innerHTML=wateringHistory.map(x=>{const dateTime=x.timestamp?fmtHistoryDate(x.timestamp):(x.timestampEpochMs?fmtHistoryDate(Number(x.timestampEpochMs)):'');return `<div class="history-item"><strong>${esc(x.plantName||`Plant ${Number(x.plantIndex)+1}`)}</strong><small>${dateTime?`<span class="history-time">${esc(dateTime)}</span> · `:''}${Math.round((x.durationMs||0)/1000)} sec · ${esc(x.reason||'Completed')} · ${x.moistureStart??'--'}% → ${x.moistureEnd??'--'}%</small></div>`}).join('')}
function renderChart(){const el=document.getElementById('chart');if(!telemetry.length){el.innerHTML='<div class="empty">No telemetry yet.</div>';return}const plants=telemetry[telemetry.length-1]?.plants||[];const W=700,H=230,pad=25;let paths='';plants.forEach((_,pi)=>{const pts=telemetry.map((r,idx)=>{const x=pad+(idx/Math.max(1,telemetry.length-1))*(W-pad*2);const m=Number(r.plants?.[pi]?.moisture||0);const y=H-pad-(m/100)*(H-pad*2);return `${x.toFixed(1)},${y.toFixed(1)}`}).join(' ');paths+=`<polyline points="${pts}" fill="none" stroke="hsl(${(pi*67)%360} 55% 42%)" stroke-width="3"/>`});el.innerHTML=`<svg viewBox="0 0 ${W} ${H}" preserveAspectRatio="none"><line x1="${pad}" y1="${pad}" x2="${pad}" y2="${H-pad}" stroke="#d1d5db"/><line x1="${pad}" y1="${H-pad}" x2="${W-pad}" y2="${H-pad}" stroke="#d1d5db"/>${paths}</svg><div class="chart-legend">${plants.map((p,i)=>`<span>● ${esc(p.name||`Plant ${i+1}`)}</span>`).join('')}</div>`}
async function switchView(id){if(id==='diagnosticsView'&&!(await requireControlAuth('Diagnostics')))return;document.querySelectorAll('.dashboard-view').forEach(v=>v.classList.toggle('active',v.id===id));document.querySelectorAll('.menu-btn').forEach(b=>b.classList.toggle('active',b.dataset.view===id));if(id==='historyView')loadHistory()}
document.querySelectorAll('.menu-btn').forEach(btn=>btn.addEventListener('click',()=>switchView(btn.dataset.view)));
async function refresh(){if(settingsEditing)return;try{const s=await get('/status');if(!s)throw new Error('No V2 status yet');renderStatus(s)}catch(e){if(Date.now()-lastLiveAt>(CFG.staleTimeoutSec*1000))renderOffline()}}
document.getElementById('emergencyBtn').onclick=async()=>{if(commandBusy)return toast('Please wait — previous command is still executing.');if(confirm('Stop pump and close all valves?'))try{await sendCommand('emergency_stop',{},'Emergency Stop')}catch(e){}};document.getElementById('resumeBtn').onclick=async()=>{if(commandBusy)return toast('Please wait — previous command is still executing.');if(confirm('Resume automatic watering?'))try{await sendCommand('resume',{},'Resume')}catch(e){}};
async function finishStartupLoading(){const loader=document.getElementById('startupLoader');if(!loader)return;loader.classList.add('hidden');setTimeout(()=>loader.remove(),300)}\nasync function initialDashboardLoad(){try{await Promise.all([refresh(),loadHistory()])}finally{finishStartupLoading()}}\ninitialDashboardLoad();scheduleSecurityExpiry();setTimeout(addSecurityButtons,0);setInterval(refresh,CFG.refreshMs);setInterval(loadHistory,60000);