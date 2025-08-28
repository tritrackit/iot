// --- helpers
function $(id) { return document.getElementById(id); }
function setMsg(el, text) {
  el.textContent = text;
  setTimeout(() => (el.textContent = ""), 1500);
}

async function apiGet(url) {
  const r = await fetch(url, { credentials: "include" });
  if (!r.ok) throw new Error(r.statusText);
  return r.json();
}
async function apiPost(url, data) {
  const r = await fetch(url, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    credentials: "include",
    body: JSON.stringify(data),
  });
  if (!r.ok) throw new Error(await r.text());
  return r.json();
}

async function logout() {
  await fetch("/api/logout", { method: "POST", credentials: "include" });
  location.href = "/login";
}
$("btnLogout")?.addEventListener("click", logout);

// --- modal
const modal = $("modal");
let modalOnYes = null;
function openConfirm(title, text, onYes) {
  $("modalTitle").textContent = title;
  $("modalText").textContent = text;
  modalOnYes = onYes;
  modal.classList.remove("hidden");
}
$("modalYes").addEventListener("click", async () => {
  try { if (modalOnYes) await modalOnYes(); }
  finally { modal.classList.add("hidden"); }
});
$("modalNo").addEventListener("click", () => modal.classList.add("hidden"));

// --- password toggles
function wirePwdToggle(inputId, btnId, eyeOpenId, eyeOffId) {
  const input = $(inputId), btn = $(btnId), eyeOpen = $(eyeOpenId), eyeOff = $(eyeOffId);
  btn?.addEventListener("click", () => {
    const show = input.type === "password";
    input.type = show ? "text" : "password";
    btn.setAttribute("aria-label", show ? "Hide password" : "Show password");
    btn.title = show ? "Hide password" : "Show password";
    eyeOpen.style.display = show ? "none" : "inline";
    eyeOff.style.display = show ? "inline" : "none";
  });
}
wirePwdToggle("authPass", "toggleAuthPwd", "authEyeOpen", "authEyeOff");
wirePwdToggle("wifiPass", "toggleWifiPwd", "wifiEyeOpen", "wifiEyeOff");

// --- guard + initial load
(async () => {
  try {
    await apiGet("/api/me");        // session guard
  } catch {
    location.href = "/login";
    return;
  }

  // Pull everything from /api/config (which loads from config.csv on the ESP32)
  try {
    const c = await apiGet("/api/config");
    // Auth
    $("authUser").value = c.user ?? "";
    $("authPass").value = ""; // don’t echo passwords
    // API
    $("apiUrl").value = c.apiUrl ?? "";
    $("intervalMs").value = c.intervalMs ?? 5000;
    // Wi-Fi
    $("wifiSsid").value = c.ssid ?? "";
    $("wifiPass").value = c.password ?? "";
  } catch {
    // Fallback: leave blank/defaults
  }

  // Also restore last-used convenience (local only; does not override fetched values if present)
  if (!$("apiUrl").value) $("apiUrl").value = localStorage.getItem("apiUrl") || "";
  if (!$("intervalMs").value) $("intervalMs").value = Number(localStorage.getItem("intervalMs")) || 5000;

  // --- Save/Cancel: Auth (writes to config.csv via /api/config)
  $("authSave").addEventListener("click", () => {
    openConfirm("Save Auth", "Update username/password in config.csv?", async () => {
      const user = $("authUser").value.trim();
      const pass = $("authPass").value; // blank to keep? We’ll send it; backend may accept blank to keep or to set blank.
      await apiPost("/api/config", { user, pass });
      localStorage.setItem("authUser", user);
      $("authPass").value = ""; // clear visible field
      setMsg($("authStatus"), "Saved!");
    });
  });
  $("authCancel").addEventListener("click", async () => {
    try {
      const c = await apiGet("/api/config");
      $("authUser").value = c.user ?? "";
      $("authPass").value = "";
    } catch {}
    setMsg($("authStatus"), "Cancelled");
  });

  // --- Save/Cancel: API (writes to config.csv)
  $("apiSave").addEventListener("click", () => {
    openConfirm("Save API", "Save interval and API URL to config.csv?", async () => {
      const apiUrl = $("apiUrl").value.trim();
      const intervalMs = Number($("intervalMs").value);
      await apiPost("/api/config", { apiUrl, intervalMs });
      localStorage.setItem("apiUrl", apiUrl);
      localStorage.setItem("intervalMs", String(intervalMs));
      setMsg($("apiStatus"), "Saved!");
    });
  });
  $("apiCancel").addEventListener("click", async () => {
    try {
      const c = await apiGet("/api/config");
      $("apiUrl").value = c.apiUrl ?? "";
      $("intervalMs").value = c.intervalMs ?? 5000;
    } catch {}
    setMsg($("apiStatus"), "Cancelled");
  });

  // --- Wi-Fi panel: dirty detection (for UX only)
  const form = $("wifiForm");
  form.querySelectorAll("input, textarea, select").forEach((el) => { el.dataset.original = el.value || ""; });
  function checkDirty() {
    let dirty = false;
    form.querySelectorAll("input, textarea, select").forEach((el) => { if (el.value !== el.dataset.original) dirty = true; });
    $("wifiSave").disabled = !dirty;
    $("wifiConnect").disabled = dirty;
  }
  form.addEventListener("input", checkDirty);
  form.addEventListener("change", checkDirty);

  // Save Wi-Fi to config.csv without connecting
  $("wifiSave").addEventListener("click", () => {
    openConfirm("Save Wi-Fi", "Save SSID/password to config.csv?", async () => {
      const ssid = $("wifiSsid").value.trim();
      const password = $("wifiPass").value;
      await apiPost("/api/config", { ssid, password });
      // remember for UX only
      localStorage.setItem("wifiSsid", ssid);
      localStorage.setItem("wifiPass", password);
      // reset dirty baseline
      form.querySelectorAll("input, textarea, select").forEach((el) => { el.dataset.original = el.value || ""; });
      $("wifiSave").disabled = true;
      $("wifiConnect").disabled = false;
      setMsg($("wifiStatus"), "Saved!");
    });
  });
  $("wifiCancel").addEventListener("click", async () => {
    try {
      const c = await apiGet("/api/config");
      $("wifiSsid").value = c.ssid ?? "";
      $("wifiPass").value = c.password ?? "";
      // reset baseline
      form.querySelectorAll("input, textarea, select").forEach((el) => { el.dataset.original = el.value || ""; });
      $("wifiSave").disabled = true;
      $("wifiConnect").disabled = false;
    } catch {}
    setMsg($("wifiStatus"), "Cancelled");
  });

  // Connect (also persists on backend per your firmware)
  $("wifiConnect").addEventListener("click", async () => {
    $("wifiConnect").disabled = true;
    const ssid = $("wifiSsid").value.trim();
    const password = $("wifiPass").value;
    if (!ssid) { setMsg($("wifiStatus"), "SSID required"); $("wifiConnect").disabled = true; return; }
    $("wifiStatus").textContent = "Connecting…";
    try {
      await apiPost("/api/wifi/connect", { ssid, password });
    } catch {
      $("wifiStatus").textContent = "Connect failed";
      $("wifiConnect").disabled = false;
      return;
    }
    $("wifiStatus").textContent = "Connecting… (polling status)";
  });

  // live status poll
  async function pollWifi() {
    try {
      const st = await apiGet("/api/wifi/status");
      const c = st.sta?.connected;
      $("wifiLive").textContent =
        `AP IP: ${st.ap?.ip || "-"} | STA: ${
          c ? "Connected" : st.sta?.connecting ? "Connecting…" : "Disconnected"
        } ` + (c ? `SSID: ${st.sta?.ssid} IP: ${st.sta?.ip} RSSI: ${st.sta?.rssi}` : "");
      $("wifiStatus").textContent = st.sta?.connected ? "" : st.sta?.connecting ? "Connecting… (polling status)" : "";
    } catch {
      $("wifiConnect").disabled = false;
    }
    setTimeout(pollWifi, 1500);
  }
  pollWifi();
})();
