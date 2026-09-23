(() => {
  const KEY="plantV3SuperAdminSession";
  let session=null, devices=[], customers=[], supportTimer=null, supportSessions={};
  const $=id=>document.getElementById(id);
  const esc=v=>String(v??"").replace(/[&<>'"]/g,c=>({"&":"&amp;","<":"&lt;",">":"&gt;","'":"&#39;","\"":"&quot;"}[c]));
  const api=async(path,opts={})=>{
    const r=await fetch("/api"+path,{...opts,headers:{"Content-Type":"application/json",...(session?.token?{Authorization:"Bearer "+session.token}:{}),...(opts.headers||{})},cache:"no-store"});
    const j=await r.json().catch(()=>({})); if(!r.ok)throw new Error(j.error||"Request failed."); return j;
  };
  const msg=(id,text,err=false)=>{const e=$(id);if(!e)return;e.textContent=text||"";e.className=err?"auth-error":"auth-status";e.hidden=!text};
  const toast=t=>{const e=$("adminToast");e.textContent=t;e.hidden=false;clearTimeout(toast.t);toast.t=setTimeout(()=>e.hidden=true,3000)};
  const save=()=>sessionStorage.setItem(KEY,JSON.stringify(session));
  const load=()=>{try{session=JSON.parse(sessionStorage.getItem(KEY)||"null")}catch{session=null}};
  const clear=()=>{session=null;sessionStorage.removeItem(KEY)};
  const dt=v=>v?new Date(Number(v)).toLocaleString():"Never";
  const pill=(text,kind="")=>`<span class="status-pill ${kind}">${esc(text)}</span>`;
  function showApp(){ $("loginShell").hidden=true; $("adminShell").hidden=false; $("adminUser").textContent=session.admin.email; loadAll(); }
  async function login(){
    msg("loginError","");msg("loginStatus","Signing in…");
    try{session=await api("/superadmin/login",{method:"POST",body:JSON.stringify({email:$("adminEmail").value.trim(),password:$("adminPassword").value})});save();showApp()}
    catch(e){msg("loginStatus","");msg("loginError",e.message,true)}
  }
  function renderStats(s){$("stats").innerHTML=[["Devices",s.devices],["Claimed",s.claimed],["Customers",s.customers],["Active Support",s.activeSupport]].map(x=>`<div class="stat"><span>${esc(x[0])}</span><strong>${x[1]}</strong></div>`).join("")}
  async function loadStats(){renderStats(await api("/superadmin/stats"))}
  async function loadDevices(){
    devices=await api("/superadmin/devices");
    $("deviceTable").innerHTML=devices.length?`<table><thead><tr><th>Device</th><th>Owner</th><th>Firmware / Last Seen</th><th>State</th><th>Actions</th></tr></thead><tbody>${devices.map(d=>`<tr><td><strong>${esc(d.deviceId)}</strong><small>Created ${dt(d.createdAt)}</small></td><td>${esc(d.ownerEmail||"Unclaimed")}</td><td>${esc(d.firmware||"Unknown")}<small>${dt(d.lastSeen)}</small></td><td>${d.disabled?pill("Disabled","bad"):d.maintenanceExpires&&Number(d.maintenanceExpires)>Date.now()?pill("Maintenance","warn"):pill("Active")}</td><td><div class="row-actions"><button onclick="window.adminSupport('${d.deviceId}')">Support</button><button class="secondary" onclick="window.adminMaintenance('${d.deviceId}',${d.disabled?0:1})">${d.disabled?"Enable":"Disable"}</button><button class="secondary" onclick="window.adminOwner('${d.deviceId}')">Owner</button></div></td></tr>`).join("")}</tbody></table>`:"<div class='empty'>No devices registered.</div>";
    const unclaimed=devices.filter(d=>!d.ownerId&&!d.disabled);$("qrDevice").innerHTML="<option value=''>Select an unclaimed device</option>"+unclaimed.map(d=>`<option value="${esc(d.deviceId)}">${esc(d.deviceId)}</option>`).join("");
  }
  async function loadCustomers(){
    customers=await api("/superadmin/customers");
    $("customerTable").innerHTML=customers.length?`<table><thead><tr><th>Customer</th><th>Email</th><th>Devices</th><th>Status</th><th>Actions</th></tr></thead><tbody>${customers.map(c=>`<tr><td><strong>${esc(c.name)}</strong></td><td>${esc(c.email)}</td><td>${c.deviceCount}</td><td>${c.disabled?pill("Disabled","bad"):pill("Active")}</td><td><button class="secondary" onclick="window.adminCustomerDevices('${esc(c.email)}')">View Ownership</button></td></tr>`).join("")}</tbody></table>`:"<div class='empty'>No customers.</div>";
  }
  async function loadMaintenance(){
    const active=devices.length?devices:await api("/superadmin/devices");
    $("maintenanceTable").innerHTML=`<table><thead><tr><th>Device</th><th>Maintenance Until</th><th>Controller</th><th>Action</th></tr></thead><tbody>${active.map(d=>`<tr><td>${esc(d.deviceId)}</td><td>${d.maintenanceExpires?dt(d.maintenanceExpires):"Not scheduled"}</td><td>${d.disabled?pill("Disabled","bad"):pill("Enabled")}</td><td><button class="secondary" onclick="window.adminSetMaintenance('${d.deviceId}')">Set Window</button></td></tr>`).join("")}</tbody></table>`;
  }
  async function loadAudit(){
    const rows=await api("/superadmin/audit");
    $("auditTable").innerHTML=rows.length?`<table><thead><tr><th>Time</th><th>Action</th><th>Device</th><th>Customer</th><th>Details</th></tr></thead><tbody>${rows.map(x=>`<tr><td>${dt(x.createdAt)}</td><td><strong>${esc(x.action)}</strong></td><td>${esc(x.deviceId||"—")}</td><td>${esc(x.customerEmail||"—")}</td><td><code>${esc(JSON.stringify(x.details||{}))}</code></td></tr>`).join("")}</tbody></table>`:"<div class='empty'>No audit events.</div>";
  }
  async function loadSupport(){
    $("supportPanel").innerHTML=devices.length?devices.map(d=>`<article class="support-card"><h3>${esc(d.deviceId)}</h3><p>Owner: ${esc(d.ownerEmail||"Unclaimed")} · Last seen: ${dt(d.lastSeen)}</p><pre id="support-${esc(d.deviceId)}">${esc(JSON.stringify(d.status||{},null,2))}</pre><div class="row-actions"><button onclick="window.adminStartSupport('${d.deviceId}')">Start 30-min Support</button><button class="secondary" onclick="window.adminSupportCommand('${d.deviceId}','emergency_stop')">Emergency Stop</button><button class="secondary" onclick="window.adminSupportCommand('${d.deviceId}','resume')">Resume</button></div></article>`).join(""):"<div class='empty'>No devices.</div>";
  }
  async function loadAll(){try{await Promise.all([loadStats(),loadDevices(),loadCustomers(),loadAudit()]);loadMaintenance();loadSupport()}catch(e){if(e.message.includes("authentication")){clear();$("adminShell").hidden=true;$("loginShell").hidden=false}else toast(e.message)}}
  window.adminMaintenance=async(deviceId,disabled)=>{try{await api("/superadmin/device/update",{method:"POST",body:JSON.stringify({deviceId,disabled:!!disabled})});toast(disabled?"Device disabled.":"Device enabled.");loadAll()}catch(e){toast(e.message)}};
  window.adminSetMaintenance=async deviceId=>{const mins=prompt("Maintenance window in minutes. Enter 0 to clear.","60");if(mins===null)return;const n=Math.max(0,Number(mins)||0);const expires=n?Date.now()+n*60000:null;try{await api("/superadmin/device/update",{method:"POST",body:JSON.stringify({deviceId,maintenanceExpiresAt:expires})});toast("Maintenance window updated.");loadAll()}catch(e){toast(e.message)}};
  window.adminOwner=async deviceId=>{const email=prompt("Customer email. Leave blank to remove ownership.");if(email===null)return;try{await api("/superadmin/device/owner",{method:"POST",body:JSON.stringify({deviceId,email})});toast("Ownership updated.");loadAll()}catch(e){toast(e.message)}};
  window.adminCustomerDevices=email=>{const list=devices.filter(d=>d.ownerEmail===email).map(d=>d.deviceId);alert(email+"\n\nDevices:\n"+(list.join("\n")||"None"))};
  window.adminSupport=async deviceId=>{document.querySelector('[data-view="support"]').click();setTimeout(()=>document.getElementById("support-"+deviceId)?.scrollIntoView({behavior:"smooth"}),50)};
  window.adminStartSupport=async deviceId=>{try{const s=await api("/superadmin/support/start",{method:"POST",body:JSON.stringify({deviceId})});supportSessions[deviceId]=s.id;toast("Support session started for 30 minutes.");if(s.expiresAt){clearInterval(supportTimer);supportTimer=setInterval(()=>{if(Date.now()>=s.expiresAt){clearInterval(supportTimer);toast("Support session expired.")}},1000)}}catch(e){toast(e.message)}};
  window.adminSupportCommand=async(deviceId,action)=>{if(!confirm(action==="emergency_stop"?"Send Emergency Stop to this controller?":"Send "+action+" to this controller?"))return;try{await api("/superadmin/device/command",{method:"POST",body:JSON.stringify({deviceId,action,supportSessionId:supportSessions[deviceId],extra:{}})});toast("Command sent to controller.");loadAudit()}catch(e){toast(e.message)}};
  $("registerDeviceBtn").onclick=async()=>{msg("registerStatus","Preparing device…");try{const id=$("regDeviceId").value.trim().toUpperCase();if(!id||!/^ESPBOARD-[A-F0-9]{6}$/.test(id)){msg("registerStatus","Enter a valid Device ID such as ESPBOARD-A1B2C3.",true);return}const j=await api("/superadmin/device/register",{method:"POST",body:JSON.stringify({deviceId:id})});await loadDevices();$("qrDevice").value=id;msg("registerStatus",j.created?"Device prepared successfully. Generate the product activation QR now.":"Device is already prepared. You can generate the product activation QR.")}catch(e){msg("registerStatus",e.message,true)}};
  $("generateQrBtn").onclick=async()=>{msg("qrStatus","Generating product activation QR…");$("qrResult").hidden=true;try{const j=await api("/superadmin/enrollment/create",{method:"POST",body:JSON.stringify({deviceId:$("qrDevice").value})});const url=new URL("/index.html",location.origin);url.hash=new URLSearchParams({device:j.deviceId,enroll:j.token}).toString();$("qr").innerHTML="";new QRCode($("qr"),{text:url.href,width:280,height:280});$("qrText").textContent=j.deviceId;$("qrResult").hidden=false;$("qrCountdown").textContent="Permanent activation QR — valid until the product is activated.";msg("qrStatus","Product activation QR generated successfully.");loadDevices()}catch(e){msg("qrStatus",e.message,true)}};
  $("printQrBtn").onclick=()=>{
    const qr=$("qr");
    if(!qr||!qr.innerHTML.trim()){msg("qrStatus","Generate the activation QR before printing.",true);return}
    const printWindow=window.open("","_blank","width=720,height=900");
    if(!printWindow){msg("qrStatus","Please allow pop-ups for printing the activation QR.",true);return}
    const deviceId=esc($("qrText").textContent||"");
    const qrMarkup=qr.innerHTML;
    printWindow.document.write(`<!DOCTYPE html><html><head><meta charset="UTF-8"><title>Plant Life Care - Product Activation</title><style>
      *{box-sizing:border-box}body{font-family:Arial,sans-serif;margin:0;padding:32px;color:#111;text-align:center}
      .sheet{max-width:620px;margin:0 auto;border:2px solid #222;border-radius:16px;padding:28px}
      h1{font-size:24px;margin:0 0 6px}h2{font-size:17px;margin:24px 0 10px;text-align:left}
      .subtitle{margin:0 0 18px;color:#555;font-size:13px}.device{font-size:16px;font-weight:700;margin:14px 0}
      .qr{display:flex;justify-content:center;margin:18px 0}.qr img,.qr canvas{width:280px!important;height:280px!important}
      ol{text-align:left;margin:8px 0 0;padding-left:24px;font-size:14px;line-height:1.7}
      .note{text-align:left;margin-top:18px;padding:12px;background:#f3f3f3;border-radius:8px;font-size:12px;line-height:1.5}
      @media print{body{padding:0}.sheet{border:0;margin:0 auto}}
    </style></head><body><div class="sheet">
      <h1>Plant Life Care</h1>
      <p class="subtitle">Product Activation</p>
      <div class="device">Device ID: ${deviceId}</div>
      <div class="qr">${qrMarkup}</div>
      <h2>Activation Steps</h2>
      <ol>
        <li>Open the Plant Life Care app.</li>
        <li>Scan this Product Activation QR code.</li>
        <li>Register or sign in to your customer account.</li>
        <li>Confirm activation for the displayed Device ID.</li>
        <li>After activation, this QR code cannot be used again.</li>
      </ol>
      <div class="note"><strong>Important:</strong> Keep this QR code with the product until the customer completes activation. It is single-use and remains valid until it is activated.</div>
    </div></body></html>`);
    printWindow.document.close();
    printWindow.focus();
    setTimeout(()=>{printWindow.print();printWindow.close()},250);
  };
  document.querySelectorAll(".admin-nav button").forEach(b=>b.onclick=async()=>{document.querySelectorAll(".admin-nav button").forEach(x=>x.classList.toggle("active",x===b));document.querySelectorAll(".admin-view").forEach(x=>x.classList.toggle("active",x.id===b.dataset.view));if(b.dataset.view==="devices")await loadDevices();if(b.dataset.view==="customers")await loadCustomers();if(b.dataset.view==="maintenance")await loadMaintenance();if(b.dataset.view==="support")await loadSupport();if(b.dataset.view==="audit")await loadAudit()});
  $("logoutBtn").onclick=()=>{clear();location.reload()};$("refreshDevices").onclick=loadDevices;$("refreshCustomers").onclick=loadCustomers;$("refreshMaintenance").onclick=loadMaintenance;$("refreshAudit").onclick=loadAudit;$("loginBtn").onclick=login;
  async function boot(){load();if(!session){$("loginShell").hidden=false;return}try{const m=await api("/superadmin/me");session.admin=m.admin;save();showApp()}catch{clear();$("loginShell").hidden=false}}
  document.addEventListener("DOMContentLoaded",()=>{const s=document.createElement("script");s.src="https://cdn.jsdelivr.net/npm/qrcodejs@1.0.0/qrcode.min.js";document.head.appendChild(s);boot()});
})();