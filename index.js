const express = require("express");
const fsSync = require("fs");
const fsp = require("fs").promises;
const path = require("path");
const { Mutex } = require("async-mutex");

const app = express();
const DATA_FILE = path.join(__dirname, "data.json");
const TMP_FILE = DATA_FILE + ".tmp";
const mutex = new Mutex();  // ← Ngăn race condition
const MAX_PENDING = 1000;

function sanitizeDeviceKey(v) {
  if (!v) return null;
  if (typeof v !== 'string') v = String(v);
  v = v.trim();
  if (v.length === 0 || v.length > 64) return null;
  if (!/^[A-Za-z0-9_\-]+$/.test(v)) return null;
  return v;
}

function escapeHtml(s) {
  if (s === null || s === undefined) return "";
  return String(s)
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;")
    .replace(/"/g, "&quot;")
    .replace(/'/g, "&#39;");
}

// Ensure data file exists and cleanup tmp on startup
try {
  if (!fsSync.existsSync(DATA_FILE)) fsSync.writeFileSync(DATA_FILE, "[]", "utf8");
} catch (e) {}
try {
  if (fsSync.existsSync(TMP_FILE)) fsSync.unlinkSync(TMP_FILE);
} catch (e) {}

// Async read/write (non-blocking)
async function readData() {
  try {
    const s = await fsp.readFile(DATA_FILE, "utf8");
    return JSON.parse(s || "[]");
  } catch (e) {
    return [];
  }
}

async function writeData(arr) {
  const release = await mutex.acquire(); // <-- use correct mutex variable
  try {
    await fsp.writeFile(TMP_FILE, JSON.stringify(arr, null, 2), "utf8");
    await fsp.rename(TMP_FILE, DATA_FILE);
  } catch (err) {
    try { if (fsSync.existsSync(TMP_FILE)) fsSync.unlinkSync(TMP_FILE); } catch (_) {}
    throw err;
  } finally {
    release();
  }
}

app.use(express.json({ limit: "1mb" }));

// Health
app.get("/healthz", (req, res) => res.status(200).send("ok"));

// Long-poll + Trigger
const pendingRequests = new Map();  // device_key → res
const clientTimeouts = new Map();   // device_key → timeout ID

app.get("/wait", (req, res) => {
  const device_key = sanitizeDeviceKey(req.query.device_key);
  if (!device_key) return res.status(400).json({ error: "Thiếu hoặc device_key không hợp lệ" });

  if (pendingRequests.size >= MAX_PENDING) {
    return res.status(503).json({ error: "Server busy, try later" });
  }

  // helper cleanup (idempotent)
  const cleanup = () => {
    const cur = pendingRequests.get(device_key);
    if (cur === res) pendingRequests.delete(device_key);
    const t = clientTimeouts.get(device_key);
    if (t) {
      clearTimeout(t);
      clientTimeouts.delete(device_key);
    }
  };

  // If old pending exists, reply none and cleanup
  if (pendingRequests.has(device_key)) {
    const oldRes = pendingRequests.get(device_key);
    try { if (!oldRes.headersSent) oldRes.json({ cmd: "none" }); } catch (e) {}
    pendingRequests.delete(device_key);
    const t = clientTimeouts.get(device_key);
    if (t) { clearTimeout(t); clientTimeouts.delete(device_key); }
  }

  // store new pending response
  pendingRequests.set(device_key, res);

  const timeoutId = setTimeout(() => {
    // only reply the same res
    const r = pendingRequests.get(device_key);
    if (r === res) {
      try { if (!res.headersSent) res.json({ cmd: "none" }); } catch (e) {}
      cleanup();
    }
  }, 60000);
  clientTimeouts.set(device_key, timeoutId);

  // ensure response won't hang forever (extra guard) - keep consistent cleanup
  res.setTimeout(61000, () => {
    try { if (!res.headersSent) res.json({ cmd: "none" }); } catch (e) {}
    cleanup();
  });

  // client closed connection
  req.on("close", () => {
    cleanup();
  });
});

app.post("/trigger", (req, res) => {
  // Accept device_key in query or body
  const rawKey = req.query.device_key || (req.body && req.body.device_key);
  const device_key = sanitizeDeviceKey(rawKey);
  if (!device_key) return res.status(400).send("Thiếu hoặc device_key không hợp lệ");

  // Atomically take client and remove from maps
  const client = pendingRequests.get(device_key);
  const timeoutId = clientTimeouts.get(device_key);

  if (client) {
    // remove entries first to avoid race
    pendingRequests.delete(device_key);
    if (timeoutId) { clearTimeout(timeoutId); clientTimeouts.delete(device_key); }

    try {
      if (!client.headersSent) client.json({ cmd: "send" });
      return res.send(`Đã gửi lệnh đến ${device_key}`);
    } catch (e) {
      // nếu gửi thất bại, trả lỗi cho caller
      return res.status(500).send("Lỗi khi gửi lệnh tới client");
    }
  } else {
    return res.send(`Thiết bị không online: ${device_key}`);
  }
});

// Log
app.post("/log", async (req, res) => {
  const { device_key, device, status, timestamp, uptime, localip, resent } = req.body;
  const sk = sanitizeDeviceKey(device_key);
  if (!sk || !device || !status) return res.status(400).json({ ok: false });

  const record = {
    device_key: sk,
    device: String(device),
    status: String(status),
    timestamp: timestamp || new Date().toISOString(),
    uptime: uptime || null,
    localip: localip || null,
    resent: !!resent,
    createdAt: new Date().toISOString(),
  };

  try {
    const data = await readData(); // <-- awaited
    data.push(record);
    await writeData(data);
    res.json({ ok: true, saved: record, total: data.length });
  } catch (err) {
    res.status(500).json({ ok: false, message: "Lỗi server" });
  }
});

// Status
const latestStatus = new Map();
app.post("/status", (req, res) => {
  const { device_key, device, status, timestamp, uptime, localip } = req.body;
  const sk = sanitizeDeviceKey(device_key);
  if (!sk || !device || !status) return res.status(400).json({ ok: false });


  const result = { device_key: sk, device, status, timestamp: timestamp || new Date().toISOString(), uptime, localip, receivedAt: new Date().toISOString() };
  latestStatus.set(sk, result);
  setTimeout(() => latestStatus.delete(sk), 30000);
  res.json({ ok: true, data: result });
});

app.get("/status/latest", (req, res) => {
  const key = sanitizeDeviceKey(req.query.device_key);
  if (!key) return res.json({ ok: false });
  const data = latestStatus.get(key);
  res.json(data ? { ok: true, data } : { ok: false });
});

// Route: hiển thị data với pagination
app.get("/data", async (req, res) => {
  const page = Math.max(1, parseInt(req.query.page) || 1);
  const limit = 50;

  // clone rồi reverse để không thay đổi mảng gốc
  const raw = await readData() || [];
  const data = raw.slice().reverse();

  const total = data.length;
  const pages = Math.max(1, Math.ceil(total / limit)); // đảm bảo >=1
  const curPage = Math.min(page, pages);
  const start = (curPage - 1) * limit;
  const end = Math.min(start + limit, total);

  // nếu không có dữ liệu, rows sẽ là chuỗi rỗng
  const rows = (total === 0)
    ? ""
    : data.slice(start, end).map((r, i) => {
      const idx = total - start - i;
      return `
      <tr>
        <td data-label="#">${idx}</td>
        <td data-label="DEVICE_KEY"><strong>${escapeHtml(r.device_key)}</strong></td>
        <td data-label="Thiết bị">${escapeHtml(r.device)}</td>
        <td data-label="Trạng thái">${escapeHtml(r.status)}</td>
        <td data-label="Thời gian">${escapeHtml(r.timestamp)}</td>
        <td data-label="Uptime">${escapeHtml(r.uptime || "")}</td>
        <td data-label="IP">${escapeHtml(r.localip || "")}</td>
        <td data-label="Nhận lúc">${escapeHtml(new Date(r.createdAt).toLocaleString("vi-VN"))}</td>
      </tr>`;
    }).join("");

  // build danh sách device keys cho select (escape)
  const deviceKeys = [...new Set(data.map(r => r.device_key).filter(Boolean))]
    .sort()
    .map(k => escapeHtml(k));

  // Pagination HTML: tính hiển thị range an toàn
  const displayFrom = total === 0 ? 0 : (start + 1);
  const displayTo = total === 0 ? 0 : end;

  const pagination = `
    <div style="margin:20px 0;text-align:center;font-size:1.1rem;">
      ${curPage > 1 ? `<a href="/data?page=${curPage-1}" class="btn">Trước</a>` : '<span class="btn disabled">Trước</span>'}
      <span style="margin:0 15px;">Trang <strong>${curPage}</strong> / <strong>${pages}</strong></span>
      ${curPage < pages ? `<a href="/data?page=${curPage+1}" class="btn">Sau</a>` : '<span class="btn disabled">Sau</span>'}
      <div style="margin-top:8px;font-size:0.9rem;color:#666;">
        Hiển thị ${displayFrom}–${displayTo} trong tổng số <strong>${total}</strong> bản ghi
      </div>
    </div>
  `;

  // HTML chính: chèn rows và deviceKeys (mình giữ style & js của bạn)
  const html = `<!DOCTYPE html>
<html lang="vi">
<head>
  <meta charset="UTF-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1.0"/>
  <title>IoT Monitor</title>
  <style>
    :root {
      --primary: #007bff;
      --danger: #dc3545;
      --gray: #6c757d;
      --light: #f8f9fa;
      --dark: #343a40;
    }
    * { box-sizing: border-box; }
    body { font-family: system-ui, -apple-system, sans-serif; margin: 0; padding: 16px; background: #f0f2f5; color: #333; }
    .container { max-width: 1200px; margin: auto; background: white; border-radius: 16px; padding: 24px; box-shadow: 0 4px 20px rgba(0,0,0,0.1); }
    h1 { margin: 0 0 20px; color: var(--dark); font-size: 1.8rem; }
    .actions{display:flex;justify-content:space-between;align-items:center;width:100%;margin-bottom:20px}
    .btn{padding:10px 18px;border:none;border-radius:8px;font-weight:500;cursor:pointer;text-decoration:none;display:inline-block;transition:.2s;font-size:.95rem;white-space:nowrap}
    .btn-primary{background:var(--primary);color:#fff}
    .btn-danger{background:var(--danger);color:#fff}
    .btn:hover{opacity:.9;transform:translateY(-1px);box-shadow:0 4px 8px rgba(0,0,0,.15)}
    .btn.disabled{background:#ccc;cursor:not-allowed;opacity:.6;pointer-events:none}

    table { width: 100%; border-collapse: collapse; margin-top: 16px; font-size: 0.95rem; }
    th, td { padding: 12px; text-align: left; border-bottom: 1px solid #eee; }
    th { background: var(--light); font-weight: 600; color: var(--dark); }
    tr:hover { background: #f5f9ff; }
    .empty { text-align: center; color: #666; padding: 40px; font-style: italic; }

    /* Popup */
    .modal { display: none; position: fixed; top: 0; left: 0; width: 100%; height: 100%; background: rgba(0,0,0,0.6); justify-content: center; align-items: center; z-index: 1000; }
    .modal.open { display: flex; }
    .modal-content { background: white; padding: 28px; border-radius: 16px; width: 90%; max-width: 520px; box-shadow: 0 15px 40px rgba(0,0,0,0.25); }
    .modal-header { display: flex; justify-content: space-between; align-items: center; margin-bottom: 20px; }
    .modal-header h2 { margin: 0; font-size: 1.5rem; color: var(--dark); }
    .close { cursor: pointer; font-size: 1.8rem; color: #aaa; font-weight: bold; }
    .close:hover { color: #000; }
    label { display: block; margin: 12px 0 6px; font-weight: 500; color: var(--dark); }
    .modal select, .modal button { width: 100%; padding: 12px; margin: 8px 0; border: 1px solid #ddd; border-radius: 8px; font-size: 1rem; }
    select { background: white; }
    button { background: var(--primary); color: white; font-weight: 500; cursor: pointer; }
    button:hover { background: #0056b3; }
    .result {
      margin-top: 20px; padding: 16px; background: #f0f8ff; border-radius: 8px;
      font-family: monospace; font-size: 0.9rem; max-height: 320px; overflow-y: auto;
      border: 1px solid #bee5eb; display: none;
    }
    .loading { color: var(--primary); font-style: italic; animation: pulse 1.5s infinite; }
    @keyframes pulse { 0%,100% { opacity: 1; } 50% { opacity: 0.5; } }

    /* Responsive */
    @media (max-width: 768px) {
      table, thead, tbody, th, td, tr { display: block; }
      thead tr { position: absolute; top: -9999px; left: -9999px; }
      tr { border: 1px solid #ddd; border-radius: 8px; margin-bottom: 12px; padding: 8px; }
      td { border: none; position: relative; padding-left: 50%; white-space: pre-wrap; }
      td:before { content: attr(data-label); position: absolute; left: 12px; width: 45%; font-weight: bold; }
    }
  </style>
</head>
<body>
  <div class="container">
    <h1>IoT Monitor - Dữ liệu từ ESP32</h1>
    <div class="actions">
      <button class="btn btn-primary" id="updateBtn">Cập nhật trạng thái ESP</button>
      <a href="/data/clear" class="btn btn-danger" onclick="return confirm('Xóa toàn bộ dữ liệu?')">
          Xóa tất cả
      </a>
    </div>

    <table>
      <thead>
        <tr>
          <th>#</th>
          <th>DEVICE_KEY</th>
          <th>Thiết bị</th>
          <th>Trạng thái</th>
          <th>Thời gian</th>
          <th>Uptime</th>
          <th>IP</th>
          <th>Nhận lúc</th>
        </tr>
      </thead>
      <tbody>
        ${rows || '<tr><td colspan="8" class="empty">Chưa có dữ liệu</td></tr>'}
      </tbody>
    </table>

    ${pagination}
  </div>

   <!-- Popup -->
  <div class="modal" id="modal">
    <div class="modal-content">
      <div class="modal-header">
        <h2>Yêu cầu cập nhật</h2>
        <span class="close" id="closeModal">×</span>
      </div>
      <form id="triggerForm">
        <label for="deviceKeySelect">Chọn thiết bị:</label>
        <select id="deviceKeySelect" required>
          <option value="">-- Chọn thiết bị --</option>
          ${deviceKeys.map(k => `<option value="${k}">${k}</option>`).join("")}
        </select>
        <button type="submit">Gửi lệnh</button>
      </form>
      <div id="result" class="result"></div>
    </div>
  </div>

  <script>
    const modal = document.getElementById('modal');
    const updateBtn = document.getElementById('updateBtn');
    const closeModal = document.getElementById('closeModal');
    const form = document.getElementById('triggerForm');
    const select = document.getElementById('deviceKeySelect');
    const resultDiv = document.getElementById('result');

    updateBtn.onclick = () => modal.classList.add('open');
    closeModal.onclick = () => modal.classList.remove('open');
    window.onclick = (e) => { if (e.target === modal) modal.classList.remove('open'); };

    form.onsubmit = async (e) => {
      e.preventDefault();
      const device_key = select.value;
      if (!device_key) return;

      resultDiv.style.display = 'block';
      resultDiv.innerHTML = '<p class="loading">Đang gửi lệnh...</p>';

      try {
        const triggerRes = await fetch(\`/trigger?device_key=\${device_key}\`, { method: 'POST' });
        const triggerText = await triggerRes.text();
        resultDiv.innerHTML = \`<p><strong>Kết quả:</strong> \${triggerText}</p>\`;

        resultDiv.innerHTML += '<p class="loading">Chờ phản hồi từ ESP (tối đa 10s)...</p>';
        let data = null;
        for (let i = 0; i < 20; i++) {
          const res = await fetch(\`/status/latest?device_key=\${device_key}\`);
          if (res.ok) {
            const json = await res.json();
            if (json.ok && json.data) { data = json.data; break; }
          }
          await new Promise(r => setTimeout(r, 500));
        }

        if (data) {
          resultDiv.innerHTML = \`
            <p><strong>Trạng thái hiện tại:</strong></p>
            <pre>\${JSON.stringify(data, null, 2)}</pre>
          \`;
        } else {
          resultDiv.innerHTML += '<p>ESP không phản hồi.</p>';
        }
      } catch (err) {
        resultDiv.innerHTML = '<p style="color:red;">Lỗi kết nối</p>';
      }
    };

    // Tự reload bảng mỗi 10s
    setInterval(() => location.reload(), 10000);
  </script>
</body>
</html>
`;

  res.setHeader("Content-Type", "text/html; charset=utf-8");
  res.send(html);
});

// Clear data (async)
app.get("/data/clear", async (req, res) => {
  try {
    await writeData([]);
    res.redirect("/data");
  } catch (e) {
    res.status(500).send("Xóa dữ liệu thất bại");
  }
});



// Endpoint: Trang chủ - Hello World
app.get("/", (req, res) => {
  res.setHeader("Content-Type", "text/html; charset=utf-8");
  res.send(`
<!DOCTYPE html>
<html lang="vi">
<head>
  <meta charset="UTF-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1.0"/>
  <title>HLC IoT Server</title>
  <style>
    body {
      font-family: system-ui, sans-serif;
      display: flex;
      justify-content: center;
      align-items: center;
      height: 100vh;
      margin: 0;
      background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
      color: white;
      text-align: center;
    }
    .container {
      padding: 40px;
      background: rgba(255,255,255,0.1);
      border-radius: 16px;
      backdrop-filter: blur(10px);
      box-shadow: 0 8px 32px rgba(0,0,0,0.2);
    }
    h1 { margin: 0 0 16px; font-size: 2.5rem; }
    p { margin: 8px 0; font-size: 1.1rem; }
    a {
      color: #fff;
      text-decoration: underline;
      font-weight: 500;
    }
    a:hover { color: #a0e7ff; }
  </style>
</head>
<body>
  <div class="container">
    <h1>Hello!</h1>
    <p>Chào mừng đến với <strong>HLC IoT Monitor Server</strong></p>
    <p>Xem dữ liệu tại: <a href="/data">/data</a></p>
  </div>
</body>
</html>
  `);
});

function gracefulExit(code = 0) {
  try {
    pendingRequests.forEach((r, k) => {
      try { if (!r.headersSent) r.json({ cmd: "none" }); } catch (e) {}
    });
    pendingRequests.clear();

    clientTimeouts.forEach(t => clearTimeout(t));
    clientTimeouts.clear();

    // cleanup tmp file if exists
    try { if (fsSync.existsSync(TMP_FILE)) fsSync.unlinkSync(TMP_FILE); } catch (e) {}
  } finally {
    process.exit(code);
  }
}
process.on('SIGINT', () => gracefulExit(0));
process.on('SIGTERM', () => gracefulExit(0));

const port = 3000;
app.listen(port, '0.0.0.0', () => {
  console.log(`Server listening on port ${port} (host: 0.0.0.0)`);
});