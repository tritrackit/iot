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
document.getElementById("btnLogout")?.addEventListener("click", logout);

(async () => {
  try {
    await apiGet("/api/me");
  } catch {
    location.href = "/login";
    return;
  }

  const startBtn = document.getElementById("btnStartUpload");
  const statusText = document.getElementById("uploadStatus");
  const stopBtn = document.getElementById('btnStopUpload');
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
    if (!confirm('Start automatic upload?')) return;
    try{ await apiPost('/api/upload/start', {}); await refreshUploadState(); alert('Upload started'); }
    catch(e){ alert('Failed to start: '+e); }
  });
  stopBtn?.addEventListener('click', async()=>{
    if (!confirm('Stop automatic upload?')) return;
    try{ await apiPost('/api/upload/stop', {}); await refreshUploadState(); }
    catch(e){ alert('Failed to stop: '+e); }
  });
  refreshUploadState();
  setInterval(refreshUploadState, 5000);

  const body = document.getElementById("logsBody");
  body.innerHTML = `<tr><td colspan="3">Loading…</td></tr>`;

  try {
    const rows = await apiGet("/api/logs");
    if (!Array.isArray(rows)) throw new Error("Bad data");
    body.innerHTML = "";
    for (const r of rows) {
      const tr = document.createElement("tr");
      const status = (r.sent ? 'Sent' : (r.message || 'Pending'));
      tr.innerHTML = `
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
