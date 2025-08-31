
function $(id){ return document.getElementById(id); }
function setMsg(el, text){ if(!el) return; el.textContent = text; setTimeout(()=>{ if(el) el.textContent=""; },1500); }


async function apiGet(url) {
  const res = await fetch(url, { credentials: "include" });
  if (!res.ok) throw new Error(res.statusText);
  return res.json();
}
async function apiPost(url, data){
  const res = await fetch(url, { method:"POST", headers:{"Content-Type":"application/json"}, credentials:"include", body: JSON.stringify(data||{}) });
  if (!res.ok) throw new Error(await res.text().catch(()=>res.statusText));
  return res.json().catch(()=>({}));
}

async function logout() {
  await fetch("/api/logout", { method: "POST", credentials: "include" });
  location.href = "/login";
}
$("btnLogout")?.addEventListener("click", logout);

const modal=$("modal"); let modalOnYes=null;
function openConfirm(title,text,labelYes,labelNo,onYes){
  const t=$("modalTitle"), m=$("modalText"), lY=$("modalYes"), lN=$("modalNo");
  if(modal && t && m){
    t.textContent=title; 
    m.textContent=text;
    lY.textContent=labelYes && labelYes !== "" || "Yes";
    lN.textContent=labelNo && labelNo !== "" || "Cancel";
    modalOnYes=onYes; 
    modal.classList.remove("hidden");
  } else {
    (async()=>{ try{ if(onYes) await onYes(); }catch(e){ console.error(e); } })();
  }
}
$("modalYes")?.addEventListener("click", async()=>{ try{ if(modalOnYes) await modalOnYes(); } finally { modal?.classList.add("hidden"); } });
$("modalNo")?.addEventListener("click", ()=> modal?.classList.add("hidden"));


(async () => {
  try {
    await apiGet("/api/me");
  } catch {
    location.href = "/login";
    return;
  }

  const startBtn = $("btnStartUpload");
  const statusText = $("uploadStatus");
  const stopBtn = $('btnStopUpload');
  async function refreshUploadState(){
    try{
      const st = await apiGet('/api/upload/status');
      const valid = !!st.valid;
      if (startBtn) startBtn.disabled = !valid || !!st.enabled;
      if (stopBtn) stopBtn.disabled = !st.enabled;
      if (statusText) statusText.textContent = st.enabled ? 'Uploading: ON' : (valid ? 'Ready to start' : (st.reason||'Invalid config'));
    }catch{
      if (startBtn) startBtn.disabled = true;
      if (stopBtn) stopBtn.disabled = true;
      if (statusText) statusText.textContent = 'Status unavailable';
    }
  }
  startBtn?.addEventListener('click', async()=>{
    openConfirm("Start automatic upload?", "Turn On Auto Upload", null, null, async()=>{
      try{ await apiPost('/api/upload/start', {}); await refreshUploadState(); alert('Upload started'); }
      catch(e){ alert('Failed to start: '+e); }
    });
  });
  stopBtn?.addEventListener('click', async()=>{
    openConfirm("Stop automatic upload?", "", null, null, async()=>{
      try{ await apiPost('/api/upload/stop', {}); await refreshUploadState(); }
      catch(e){ alert('Failed to stop: '+e); }
    });
  });
  refreshUploadState();
  setInterval(refreshUploadState, 5000);

  const body = $("logsBody");
  body.innerHTML = `<tr><td colspan="3">Loading…</td></tr>`;

  try {
    const rows = await apiGet("/api/logs");
    if (!Array.isArray(rows)) throw new Error("Bad data");
    body.innerHTML = "";
    for (const r of rows) {
      const tr = document.createElement("tr");
      const status = (r.sent ? 'Sent' : (r.message || 'Pending'));
      tr.innerHTML = `
        <td>${r.scanner_id ?? ""}</td>
        <td>${r.rfid ?? ""}</td>
        <td>${r.timestamp ?? ""}</td>
        <td>${status}</td>
      `;
      body.appendChild(tr);
    }
    if (!rows.length) {
      body.innerHTML = `<tr><td colspan="3">No data</td></tr>`;
    }
  } catch {
    body.innerHTML = `<tr><td colspan="3">Failed to load logs</td></tr>`;
  }
})();
