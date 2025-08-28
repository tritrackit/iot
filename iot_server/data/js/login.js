async function postJSON(url, data) {
  const res = await fetch(url, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    credentials: "include",              // ✅ store cookie from /api/login
    body: JSON.stringify(data)
  });
  return res;
}

document.getElementById("loginForm").addEventListener("submit", async (e) => {
  e.preventDefault();
  const username = document.getElementById("username").value.trim();
  const password = document.getElementById("password").value;
  const msg = document.getElementById("msg");
  msg.textContent = "Signing in…";
  try {
    const res = await postJSON("/api/login", { username, password });
    if (res.ok) {
      // tiny delay helps some browsers commit the cookie before nav
      setTimeout(() => { location.replace("/"); }, 50);
    } else {
      const j = await res.json().catch(() => ({}));
      msg.textContent = j.error || "Login failed";
    }
  } catch {
    msg.textContent = "Network error";
  }
});

/* === Show/Hide password toggle === */
const pwd = document.getElementById("password");
const toggle = document.getElementById("togglePwd");
toggle?.addEventListener("click", () => {
  const show = pwd.type === "password";
  pwd.type = show ? "text" : "password";
  toggle.setAttribute("aria-label", show ? "Hide password" : "Show password");
  toggle.title = show ? "Hide password" : "Show password";
  document.getElementById("eyeOpen").style.display = show ? "none" : "inline";
  document.getElementById("eyeOff").style.display  = show ? "inline" : "none";
});
