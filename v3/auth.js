/* V3 Customer Authentication + Device Claiming
 * Uses Firebase Authentication REST API for email/password accounts.
 * The Firebase Web API key identifies the Firebase project; it is not a database secret.
 */
(function(){
  const C=window.PLANT_V3_CONFIG;
  const AUTH_BASE='https://identitytoolkit.googleapis.com/v1/accounts:';
  const REFRESH_URL='https://securetoken.googleapis.com/v1/token';
  const SESSION_KEY='plantV3CustomerSession';
  const state={session:null,loading:false};

  const el=id=>document.getElementById(id);
  const esc=v=>String(v??'').replace(/[&<>'"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;',"'":'&#39;','"':'&quot;'}[c]));
  const claimDevice=()=>new URLSearchParams(location.search).get('device')||'';
  const save=()=>localStorage.setItem(SESSION_KEY,JSON.stringify(state.session));
  const load=()=>{try{state.session=JSON.parse(localStorage.getItem(SESSION_KEY)||'null')}catch{state.session=null}};
  const clear=()=>{state.session=null;localStorage.removeItem(SESSION_KEY)};
  const showError=msg=>{const e=el('authError');if(e){e.textContent=msg;e.hidden=!msg}};
  const showStatus=msg=>{const e=el('authStatus');if(e)e.textContent=msg||''};

  function apiUrl(endpoint){return AUTH_BASE+endpoint+'?key='+encodeURIComponent(C.firebaseApiKey||'')}
  function authMessage(code){
    return ({
      EMAIL_EXISTS:'Email is already registered.',
      INVALID_EMAIL:'Please enter a valid email address.',
      INVALID_PASSWORD:'Incorrect email or password.',
      EMAIL_NOT_FOUND:'Incorrect email or password.',
      USER_DISABLED:'This account has been disabled.',
      WEAK_PASSWORD:'Use a stronger password.',
      OPERATION_NOT_ALLOWED:'Email/password authentication is not enabled in Firebase yet.',
      TOO_MANY_ATTEMPTS_TRY_LATER:'Too many attempts. Please wait and try again.'
    })[code]||String(code).replaceAll('_',' ').toLowerCase();
  }
  async function authRequest(endpoint,body){
    if(!C.firebaseApiKey||C.firebaseApiKey.includes('REPLACE_'))throw new Error('Add the Firebase Web API key to v3/config.js first.');
    const r=await fetch(apiUrl(endpoint),{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({...body,returnSecureToken:true})});
    const j=await r.json().catch(()=>({}));
    if(!r.ok)throw new Error(authMessage(j?.error?.message||'Authentication failed.'));
    return j;
  }
  async function db(path,method='GET',body=null){
    if(!state.session?.idToken)throw new Error('Please sign in again.');
    const url=C.firebaseBaseUrl.replace(/\/$/,'')+path+'.json?auth='+encodeURIComponent(state.session.idToken);
    const r=await fetch(url,{method,headers:body?{'Content-Type':'application/json'}:undefined,body:body?JSON.stringify(body):undefined,cache:'no-store'});
    if(r.status===401){clear();throw new Error('Your session expired. Please sign in again.')}
    if(!r.ok)throw new Error('Database request failed (HTTP '+r.status+').');
    return r.json();
  }
  async function refreshSession(){
    if(!state.session?.refreshToken)return false;
    try{
      const r=await fetch(REFRESH_URL+'?key='+encodeURIComponent(C.firebaseApiKey),{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'grant_type=refresh_token&refresh_token='+encodeURIComponent(state.session.refreshToken)});
      const j=await r.json();if(!r.ok)throw new Error();
      state.session.idToken=j.id_token;state.session.refreshToken=j.refresh_token||state.session.refreshToken;
      state.session.expiresAt=Date.now()+Number(j.expires_in||3600)*1000-60000;save();return true;
    }catch{return false}
  }
  async function ensureSession(){load();if(!state.session)return false;if(Date.now()<Number(state.session.expiresAt||0))return true;return refreshSession()}

  function setDevice(deviceId){
    if(!/^PLANT-[A-Fa-f0-9]{6}$/.test(deviceId))throw new Error('Invalid Device ID. Expected PLANT-XXXXXX.');
    C.deviceRoot='/plantMonitor/v3/devices/'+deviceId;C.authToken=state.session.idToken;C.deviceId=deviceId;
    sessionStorage.setItem('plantV3DeviceId',deviceId);
  }
  async function getUserDevices(){return (await db('/plantMonitor/v3/users/'+state.session.localId+'/devices'))||{}}
  async function claim(deviceId){
    if(!/^PLANT-[A-Fa-f0-9]{6}$/.test(deviceId))throw new Error('Invalid Device ID.');
    const ownerPath='/plantMonitor/v3/deviceOwners/'+deviceId;
    const current=await db(ownerPath);
    if(current&&current!==state.session.localId)throw new Error('This device is already claimed by another customer.');
    if(!current)await db(ownerPath,'PUT',state.session.localId);
    await db('/plantMonitor/v3/users/'+state.session.localId+'/devices/'+deviceId,'PUT',{deviceId,claimedAt:new Date().toISOString(),source:'qr'});
    setDevice(deviceId);return deviceId;
  }

  function authView(){
    const claim=claimDevice();el('customerApp').hidden=true;el('authShell').hidden=false;
    el('claimHint').innerHTML=claim?'<strong>Device found:</strong> '+esc(claim)+'<br>Sign in or register to claim this controller.':'Sign in to access your Plant Life Care devices.';
    el('claimDevice').value=claim;
  }
  function appView(deviceId){
    setDevice(deviceId);el('authShell').hidden=true;el('customerApp').hidden=false;
    el('customerUser').textContent=state.session.email||'Customer';el('customerDevice').textContent=deviceId;
    if(!document.getElementById('customerLogout')){const b=document.createElement('button');b.id='customerLogout';b.className='secondary';b.textContent='Sign out';b.onclick=()=>{clear();location.href=location.pathname};el('customerActions').appendChild(b)}
    const s=document.createElement('script');s.src='./app.js?v='+Date.now();document.body.appendChild(s);
  }
  async function continueToDevice(){
    showError('');showStatus('Checking device ownership…');
    const id=(el('claimDevice').value||'').trim().toUpperCase();if(!id)return showError('Enter or scan a Device ID first.');
    try{const devices=await getUserDevices();if(devices[id])return appView(id);await claim(id);showStatus('Device claimed successfully.');appView(id)}
    catch(e){showError(e.message||'Unable to claim device.')}
  }
  async function submit(mode){
    if(state.loading)return;state.loading=true;showError('');showStatus(mode==='register'?'Creating your customer account…':'Signing you in…');
    const email=(el(mode==='register'?'customerEmail':'loginEmail').value||'').trim().toLowerCase();
    const password=el(mode==='register'?'customerPassword':'loginPassword').value||'';
    const name=(el('customerName').value||'').trim();
    try{
      if(!email||!password)throw new Error('Email and password are required.');
      if(mode==='register'){if(name.length<2)throw new Error('Please enter your name.');if(password.length<8)throw new Error('Use at least 8 characters for your customer password.')}
      const j=await authRequest(mode==='register'?'signUp':'signInWithPassword',{email,password});
      state.session={idToken:j.idToken,refreshToken:j.refreshToken,localId:j.localId,email:j.email,expiresAt:Date.now()+Number(j.expiresIn||3600)*1000-60000};save();
      if(mode==='register')await db('/plantMonitor/v3/users/'+j.localId,'PATCH',{email:j.email,name,createdAt:new Date().toISOString()});
      else await db('/plantMonitor/v3/users/'+j.localId,'PATCH',{email:j.email,name});
      const id=claimDevice();const devices=await getUserDevices();
      if(id&&!devices[id])await claim(id);
      const chosen=id||(Object.keys(devices)[0]||null);
      if(!chosen)throw new Error('Account ready. Scan your device QR to connect your first controller.');
      appView(chosen);
    }catch(e){showError(e.message||'Authentication failed.');showStatus('')}
    finally{state.loading=false}
  }
  function bind(){
    el('registerBtn').onclick=()=>submit('register');el('loginBtn').onclick=()=>submit('login');el('claimBtn').onclick=continueToDevice;
    el('logoutLink').onclick=()=>{clear();location.href=location.pathname};
    el('showLogin').onclick=()=>{el('registerForm').hidden=true;el('loginForm').hidden=false;showError('');showStatus('')};
    el('showRegister').onclick=()=>{el('registerForm').hidden=false;el('loginForm').hidden=true;showError('');showStatus('')};
  }
  async function boot(){
    bind();if(!await ensureSession()){authView();return}
    const claim=claimDevice();
    try{const devices=await getUserDevices();const chosen=claim&&devices[claim]?claim:Object.keys(devices)[0];if(chosen){appView(chosen);return}
      if(claim){el('authShell').hidden=false;el('customerApp').hidden=true;el('claimDevice').value=claim;el('claimHint').innerHTML='<strong>Device found:</strong> '+esc(claim)+'<br>Click Claim Device to add it to your account.';return}
    }catch(e){clear();showError(e.message||'Unable to load your account.')}
    authView();
  }
  window.addEventListener('DOMContentLoaded',boot);
})();