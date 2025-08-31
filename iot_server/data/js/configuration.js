
function $(id){ return document.getElementById(id); }
function setMsg(el, text){ if(!el) return; el.textContent = text; setTimeout(()=>{ if(el) el.textContent=""; },1500); }

async function apiGet(url){ const r = await fetch(url,{credentials:"include"}); if(!r.ok) throw new Error(await r.text().catch(()=>r.statusText)); return r.json(); }
async function apiPost(url,data){ const r = await fetch(url,{method:"POST",headers:{"Content-Type":"application/json"},credentials:"include",body:JSON.stringify(data)}); if(!r.ok) throw new Error(await r.text().catch(()=>r.statusText)); try{ return await r.json(); }catch{return {};}}

async function logout(){ await fetch("/api/logout",{method:"POST",credentials:"include"}); location.href="/login"; }
$("btnLogout")?.addEventListener("click", logout);

const modal=$("modal"); let modalOnYes=null;
function openConfirm(title,text,onYes){
  const t=$("modalTitle"), m=$("modalText");
  if(modal && t && m){
    t.textContent=title; m.textContent=text; modalOnYes=onYes; modal.classList.remove("hidden");
  } else {
    (async()=>{ try{ if(onYes) await onYes(); }catch(e){ console.error(e); } })();
  }
}
$("modalYes")?.addEventListener("click", async()=>{ try{ if(modalOnYes) await modalOnYes(); } finally { modal?.classList.add("hidden"); } });
$("modalNo")?.addEventListener("click", ()=> modal?.classList.add("hidden"));

function wirePwdToggle(inputId, btnId, eyeOpenId, eyeOffId){
  const input=$(inputId), btn=$(btnId), eyeOpen=$(eyeOpenId), eyeOff=$(eyeOffId);
  btn?.addEventListener("click", ()=>{
    if(!input || !eyeOpen || !eyeOff) return;
    const show = input.type === "password"; input.type = show?"text":"password";
    btn.setAttribute?.("aria-label", show?"Hide password":"Show password"); btn.title = show?"Hide password":"Show password";
    eyeOpen.style.display = show?"none":"inline"; eyeOff.style.display = show?"inline":"none";
  });
}
document.addEventListener("DOMContentLoaded", ()=>{
  wirePwdToggle("authPass","toggleAuthPwd","authEyeOpen","authEyeOff");
  wirePwdToggle("wifiPass","toggleWifiPwd","wifiEyeOpen","wifiEyeOff");
  wirePwdToggle("apPass","toggleApPwd","apEyeOpen","apEyeOff");
});

async function loadConfig(){
  return await apiGet("/api/config");
}

async function init(){
  try { await apiGet("/api/me"); } catch { location.href="/login"; return; }

  try{
    const c = await loadConfig();
    if($("apSsid")) $("apSsid").value = c.wifi_ap_ssid ?? c.apSsid ?? "";
    if($("apPass")) $("apPass").value = c.wifi_ap_password ?? c.apPass ?? "";

    if($("wifiSsid")) $("wifiSsid").value = c.wifi_sta_ssid ?? c.ssid ?? "";
    if($("wifiPass")) $("wifiPass").value = c.wifi_sta_password ?? c.password ?? "";
    
    if($("authUser")) $("authUser").value = c.auth_user ?? c.user ?? "";
    if($("authPass")) $("authPass").value = "";
    
    if($("authPass")) $("apiUrl").value = c.api_url ?? c.apiUrl ?? "";
    if($("intervalMs")) $("intervalMs").value = (c.upload_interval ?? c.intervalMs ?? 0);
    setMsg($("wifiStatus"), "Config loaded");
  }catch{
    setMsg($("wifiStatus"), "Failed to load config");
  }

  const form=$("wifiForm");
  if(form){
    form.querySelectorAll("input, textarea, select").forEach(el=>{ el.dataset.original = el.value || ""; });
    function checkDirty(){ let d=false; form.querySelectorAll("input, textarea, select").forEach(el=>{ if(el.value!==el.dataset.original) d=true; }); $("wifiSave").disabled=!d; $("wifiConnect").disabled=d; }
    form.addEventListener("input",checkDirty); form.addEventListener("change",checkDirty);

    $("wifiSave") && ($("wifiSave").disabled = true);
    $("wifiConnect") && ($("wifiConnect").disabled = false);
  }

  
  $("authSave")?.addEventListener("click",()=>{
    openConfirm("Save Auth","Update credentials?", async()=>{
      const auth_user = $("authUser").value.trim();
      const auth_password = $("authPass").value;
      await apiPost("/api/config", {type:"auth", auth_user, auth_password});
      $("authPass").value=""; setMsg($("authStatus"),"Saved!");
    });
  });
  $("authCancel")?.addEventListener("click", async()=>{
    try{ const c=await apiGet("/api/config"); $("authUser").value=c.auth_user??""; $("authPass").value=""; }catch{}
    setMsg($("authStatus"),"Cancelled");
  });

  $("apiSave")?.addEventListener("click",()=>{
    openConfirm("Save API","Save URL/interval?", async()=>{
      const api_url = $("apiUrl").value.trim(); const upload_interval = Number($("intervalMs").value||0);
      await apiPost("/api/config", {type:"api", api_url, upload_interval}); setMsg($("apiStatus"),"Saved!");
    });
  });
  $("apiCancel")?.addEventListener("click", async()=>{
    try{ const c=await apiGet("/api/config"); $("apiUrl").value=c.api_url??""; $("intervalMs").value=c.upload_interval??0; }catch{}
    setMsg($("apiStatus"),"Cancelled");
  });

  $("wifiSave")?.addEventListener("click",()=>{
    openConfirm("Save WiFi","Save STA SSID/password?", async()=>{
      const wifi_sta_ssid=$("wifiSsid").value.trim(); const wifi_sta_password=$("wifiPass").value;
      await apiPost("/api/config", {type:"sta", wifi_sta_ssid, wifi_sta_password});
      if(form){ form.querySelectorAll("input, textarea, select").forEach(el=>{ el.dataset.original = el.value || ""; }); }
      $("wifiSave").disabled=true; $("wifiConnect").disabled=false; setMsg($("wifiStatus"),"Saved!");
    });
  });
  $("wifiCancel")?.addEventListener("click", async()=>{
    try{ const c=await apiGet("/api/config"); $("wifiSsid").value=c.wifi_sta_ssid??""; $("wifiPass").value=c.wifi_sta_password??""; if(form){ form.querySelectorAll("input, textarea, select").forEach(el=>{ el.dataset.original = el.value || ""; }); $("wifiSave").disabled=true; $("wifiConnect").disabled=false; } }catch{}
    setMsg($("wifiStatus"),"Cancelled");
  });

  $("wifiConnect")?.addEventListener("click", async()=>{
    $("wifiConnect").disabled=true;
    $("wifiConnect").innerText="Connecting...";
    const ssid=$("wifiSsid").value.trim(); const password=$("wifiPass").value;
    if(!ssid){ setMsg($("wifiStatus"),"SSID required"); $("wifiConnect").disabled=true; return; }
    $("wifiStatus").textContent="Connecting...";
    try { await apiPost("/api/wifi/connect", {ssid, password});
    $("wifiConnect").innerText="Connect"; }
    catch { 
    $("wifiConnect").innerText="Connect";
    $("wifiStatus").textContent="Connect failed"; $("wifiConnect").disabled=false; return; }
    $("wifiStatus").textContent="Connecting... (polling status)";
  });

  $("apSave")?.addEventListener("click", async()=>{
    const wifi_ap_ssid=$("apSsid").value.trim(); const wifi_ap_password=$("apPass").value;
    try{
      const res = await apiPost("/api/config", {type:"ap", wifi_ap_ssid, wifi_ap_password});
      if(res && res.ap_change_pending){ setMsg($("apStatus"),"Saved. AP changes apply after reboot."); }
      else { setMsg($("apStatus"),"Saved!"); }
    } catch{ setMsg($("apStatus"),"Save failed"); }
  });

  $("btnReboot")?.addEventListener("click", ()=>{
    openConfirm("Reboot Device","Are you sure you want to reboot now?", async()=>{
      try{
        await fetch('/api/reboot',{method:'POST', credentials:'include'});
      }catch{}
      $("rebootStatus").textContent='Rebooting...';
      setTimeout(()=>{ try{ location.href = "/" }catch{} }, 200);
    });
  });

  async function pollWifi(){
    try{
      const st=await apiGet("/api/wifi/status"); const c=st.sta?.connected;
      $("wifiLive").textContent = `AP IP: ${st.ap?.ip||"-"} | STA: ${ c?"Connected": (st.sta?.connecting?"Connecting...":"Disconnected") }` + (c?` SSID: ${st.sta?.ssid} IP: ${st.sta?.ip} RSSI: ${st.sta?.rssi}`:"");
      $("wifiStatus").textContent = c?"":(st.sta?.connecting?"Connecting... (polling status)":"");
      $("wifiConnect").innerText="Connect";
    }catch{ $("wifiConnect").disabled=false; }
    setTimeout(pollWifi,1500);
  }
  pollWifi();
}

document.addEventListener("DOMContentLoaded", ()=>{ init().catch(e=>console.error(e)); });
