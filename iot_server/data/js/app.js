async function apiGet(url) {
  const res = await fetch(url, { credentials: "include" });
  if (!res.ok) throw new Error(res.statusText);
  return res.json();
}

async function logout() {
  await fetch("/api/logout", { method: "POST", credentials: "include" });
  location.href = "/login";
}
document.getElementById("btnLogout")?.addEventListener("click", logout);

(async () => {
  try {
    await apiGet("/api/me"); // guard
  } catch {
    location.href = "/login";
    return;
  }

  const body = document.getElementById("logsBody");
  body.innerHTML = `<tr><td colspan="3">Loading…</td></tr>`;

  try {
    const rows = await apiGet("/api/logs");
    if (!Array.isArray(rows)) throw new Error("Bad data");
    body.innerHTML = "";
    for (const r of rows) {
      const tr = document.createElement("tr");
      tr.innerHTML = `
        <td>${r.rfid ?? ""}</td>
        <td>${r.scanner_id ?? ""}</td>
        <td>${r.timestamp ?? ""}</td>
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
