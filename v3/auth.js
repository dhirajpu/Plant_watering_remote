(function(){
  const C=window.PLANT_V3_CONFIG, SESSION_KEY="plantV3CustomerSession", state={session:null,loading:false};
  const el=id=>document.getElementById(id);
  const esc=v=>String(v??"").replace(/[&<>'"]/g,c=>({"&":"&amp;","<":"&lt;",">":"&gt;","'":"&#39;","\"":"&quot;"}[c]));
  const query=new URLSearchParams(location.hash.replace(/^#/,"")), claimDevice=()=>query.get("device")||"", claimToken=()=>query.get("enroll")||"";
  const save=()=>localStorage.setItem(SESSION_KEY,JSON.stringify(state.session));
  const load=()=>{try{state.session=JSON.parse(localStorage.getItem(SESSION_KEY)||"null")}catch{state.session=null}};
  const clear=()=>{state.session=null;localStorage.removeItem(SESSION_KEY)};
  const api=(path,opts={})=>fetch(C.apiBaseUrl+path,{...opts,headers:{"Content-Type":"application/json",...(opts.headers||{}),...(state.session?.token?{Authorization:"Bearer "+state.session.token}:{})},cache:"no-store"});
  const setupKey=id=>`plantV3SetupDone:${id}`;
  const setupDataKey=id=>`plantV3SetupData:${id}`;
  function showSetup(id){
    setDevice(id); el("authShell").hidden=true; el("customerApp").hidden=true; el("setupShell").hidden=false;
    el("setupStatus").textContent=""; showError(""); el("sensorStep").hidden=false; el("plantStep").hidden=true; el("sensorCount").value="";
    el("setupHint").textContent=`Device ${id} is activated. Tell us how many moisture sensors you installed.`;
  }
  function renderPlantSetup(count){
    const wrap=el("plantSetupList"); wrap.innerHTML="";
    for(let i=0;i<count;i++) wrap.insertAdjacentHTML("beforeend",`<div class="setup-plant"><h3>Sensor ${i+1}</h3><label>Plant name<input id="setupName-${i}" placeholder="Plant ${i+1}" autocomplete="off"></label><label>Moisture percentage (%)<input id="setupMoisture-${i}" type="number" min="1" max="100" inputmode="decimal" placeholder="e.g. 50"></label></div>`);
  }
  async function saveSetup(id,count,valveCount){
    const plants=[];
    for(let i=0;i<count;i++){
      const name=(el(`setupName-${i}`).value||`Plant ${i+1}`).trim();
      const target=Number(el(`setupMoisture-${i}`).value);
      if(target<1||target>100)throw new Error(`Enter a moisture percentage from 1 to 100 for Sensor ${i+1}.`);
      plants.push({name,targetLow:target,targetHigh:Math.min(100,target+10),mode:"AUTO",sensorIndex:i,valveIndex:i});
    }
    const r=await api(`/device/${encodeURIComponent(id)}/config/setup`,{method:"PUT",body:JSON.stringify({sensorCount:count,valveCount,plants})});
    if(!r.ok)throw new Error((await r.json().catch(()=>({}))).error||"Could not save the hardware setup.");
    localStorage.setItem(setupKey(id),"1");
    localStorage.setItem(setupDataKey(id),JSON.stringify({sensorCount:count,valveCount}));
  }
  async function completeSetup(id,count,valveCount){
    el("setupStatus").textContent="Saving plant setup…"; el("setupError").hidden=true; el("finishSetupBtn").disabled=true;
    try{await saveSetup(id,count,valveCount); appView(id)}catch(e){el("setupError").textContent=e.message;el("setupError").hidden=false;el("setupStatus").textContent=""}finally{el("finishSetupBtn").disabled=false}
  }
  function bindSetup(){
    el("sensorNextBtn").onclick=()=>{
      const n=Number(el("sensorCount").value),v=Number(el("valveCount").value);
      if(!Number.isInteger(n)||n<1){el("setupError").textContent="Enter a valid sensor count greater than 0.";el("setupError").hidden=false;return}
      if(!Number.isInteger(v)||v<1){el("setupError").textContent="Enter a valid valve count greater than 0.";el("setupError").hidden=false;return}
      if(n!==v){el("setupError").textContent=`Sensor count (${n}) must match valve count (${v}) before automatic watering can be enabled.`;el("setupError").hidden=false;return}
      el("setupError").hidden=true;renderPlantSetup(n);el("sensorStep").hidden=true;el("plantStep").hidden=false;
    };
    el("plantBackBtn").onclick=()=>{el("plantStep").hidden=true;el("sensorStep").hidden=false};
    el("finishSetupBtn").onclick=()=>{const n=Number(el("sensorCount").value),v=Number(el("valveCount").value);completeSetup(C.deviceId,n,v)};
  }
  const showError=msg=>{const e=el("authError");if(e){e.textContent=msg||"";e.hidden=!msg}}, showStatus=msg=>{const e=el("authStatus");if(e)e.textContent=msg||""};
  async function request(path,body,method="POST"){const r=await api(path,{method,body:body===undefined?undefined:JSON.stringify(body)}),j=await r.json().catch(()=>({}));if(!r.ok)throw new Error(j.error||"Request failed.");return j}
  function setDevice(id){if(!/^ESPBOARD-[A-F0-9]{6}$/i.test(id))throw new Error("Invalid Device ID. Expected ESPBOARD-XXXXXX.");C.deviceId=id.toUpperCase();sessionStorage.setItem("plantV3DeviceId",C.deviceId)}
  async function devices(){return (await request("/devices",undefined,"GET"))||[]}
  async function claim(id,token){if(!token)throw new Error("This QR has no secure enrollment token. Please request a new QR.");const j=await request("/enrollment/claim",{deviceId:id,token});setDevice(j.deviceId);return j.deviceId}
  function authView(){el("setupShell").hidden=true;const claim=claimDevice();el("customerApp").hidden=true;el("authShell").hidden=false;el("claimHint").innerHTML=claim?"<strong>Device found:</strong> "+esc(claim.toUpperCase())+"<br>Sign in or register to securely claim this controller.":"Sign in to access your Plant Life Care devices.";el("claimDevice").value=claim}
  function appView(id){setDevice(id);if(!localStorage.getItem(setupKey(id))){showSetup(id);return}C.customerToken=state.session?.token||"";el("authShell").hidden=true;el("customerApp").hidden=false;el("customerUser").textContent=state.session?.customer?.email||"Customer";el("customerDevice").textContent=id;if(!document.getElementById("customerLogout")){const b=document.createElement("button");b.id="customerLogout";b.className="secondary";b.textContent="Sign out";b.onclick=()=>{clear();location.href=location.pathname};el("customerActions").appendChild(b)}const s=document.createElement("script");s.src="./app.js?v="+Date.now();s.onload=()=>{const e=document.createElement("script");e.src="./enhancements.js?v="+Date.now();document.body.appendChild(e)};document.body.appendChild(s)}
  async function continueToDevice(){showError("");showStatus("Checking device ownership…");const id=(el("claimDevice").value||"").trim().toUpperCase();if(!id)return showError("Enter or scan a Device ID first.");try{const list=await devices();if(list.some(d=>d.deviceId===id)){appView(id);return}await claim(id,claimToken());showStatus("Device claimed securely.");appView(id)}catch(e){showError(e.message||"Unable to claim device.");showStatus("")}}
  async function submit(mode){if(state.loading)return;state.loading=true;showError("");showStatus(mode==="register"?"Creating your customer account…":"Signing you in…");const email=(el(mode==="register"?"customerEmail":"loginEmail").value||"").trim().toLowerCase(),password=el(mode==="register"?"customerPassword":"loginPassword").value||"",name=(el("customerName").value||"").trim();try{if(!email||!password)throw new Error("Email and password are required.");if(mode==="register"&&(name.length<2||password.length<8))throw new Error("Enter your name and use at least 8 characters.");const j=await request(mode==="register"?"/auth/register":"/auth/login",{email,password,name});state.session=j;save();const id=claimDevice(),list=await devices();const owned=id&&list.some(d=>d.deviceId===id)?id:(list[0]?.deviceId||null);if(id&&!owned)await claim(id,claimToken());const finalId=id||owned;if(!finalId)throw new Error("Account ready. Scan your device QR to connect your first controller.");appView(finalId)}catch(e){showError(e.message||"Authentication failed.");showStatus("")}finally{state.loading=false}}
  function bind(){bindSetup();el("registerBtn").onclick=()=>submit("register");el("loginBtn").onclick=()=>submit("login");el("claimBtn").onclick=continueToDevice;el("logoutLink").onclick=()=>{clear();location.href=location.pathname};el("showLogin").onclick=()=>{el("registerForm").hidden=true;el("loginForm").hidden=false;showError("");showStatus("")};el("showRegister").onclick=()=>{el("registerForm").hidden=false;el("loginForm").hidden=true;showError("");showStatus("")}}
  async function boot(){bind();load();if(!state.session){authView();return}try{const me=await request("/auth/me",undefined,"GET");state.session.customer=me.customer;save();const claim=claimDevice(),list=await devices(),chosen=claim&&list.some(d=>d.deviceId===claim)?claim:(list[0]?.deviceId||null);if(chosen){appView(chosen);return}if(claim){el("claimDevice").value=claim;return}}catch(e){clear()}authView()}
  window.addEventListener("DOMContentLoaded",boot);
})();