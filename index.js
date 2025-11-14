const express = require("express");
const fs = require("fs");
const path = require("path");
const { Mutex } = require("async-mutex");  // npm install async-mutex

const app = express();
const PORT = process.env.PORT || 3000;
const DATA_FILE = path.join(__dirname, "data.json");
const mutex = new Mutex();  // ← Ngăn race condition

// Tạo file
if (!fs.existsSync(DATA_FILE)) fs.writeFileSync(DATA_FILE, "[]", "utf8");

function readData() {
  try { return JSON.parse(fs.readFileSync(DATA_FILE, "utf8") || "[]"); }
  catch (err) { console.error("Read error:", err); return []; }
}

async function writeData(arr) {
  const release = await mutex.acquire();
  try {
    const tmp = DATA_FILE + ".tmp";
    fs.writeFileSync(tmp, JSON.stringify(arr, null, 2));
    fs.renameSync(tmp, DATA_FILE);
  } finally { release(); }
}

app.use(express.json({ limit: "1mb" }));

// Health
app.get("/healthz", (req, res) => res.status(200).send("ok"));

// Long-poll + Trigger
const pendingRequests = new Map();  // device_key → res
const clientTimeouts = new Map();   // device_key → timeout ID

app.get("/wait", (req, res) => {
  const device_key = req.query.device_key;
  if (!device_key) return res.status(400).json({ error: "Thiếu device_key" });

  // Xóa cũ nếu có
  if (pendingRequests.has(device_key)) {
    const oldRes = pendingRequests.get(device_key);
    if (oldRes.headersSent === false) oldRes.json({ cmd: "none" });
    clearTimeout(clientTimeouts.get(device_key));
  }

  pendingRequests.set(device_key, res);
  const timeout = setTimeout(() => {
    if (pendingRequests.get(device_key) === res) {
      res.json({ cmd: "none" });
      pendingRequests.delete(device_key);
    }
  }, 60000);
  clientTimeouts.set(device_key, timeout);

  req.on("close", () => {
    if (pendingRequests.get(device_key) === res) {
      pendingRequests.delete(device_key);
      clearTimeout(clientTimeouts.get(device_key));
      clientTimeouts.delete(device_key);
    }
  });
});

app.post("/trigger", (req, res) => {
  const device_key = req.query.device_key;
  if (!device_key) return res.status(400).send("Thiếu device_key");

  const client = pendingRequests.get(device_key);
  if (client && !client.headersSent) {
    client.json({ cmd: "send" });
    pendingRequests.delete(device_key);
    clearTimeout(clientTimeouts.get(device_key));
    clientTimeouts.delete(device_key);
    res.send(`Đã gửi lệnh đến ${device_key}`);
  } else {
    res.send(`Thiết bị không online: ${device_key}`);
  }
});

// Log
app.post("/log", async (req, res) => {
  const { device_key, device, status, timestamp, uptime, localip, resent } = req.body;
  if (!device_key || !device || !status) return res.status(400).json({ ok: false });

  const record = {
    device_key: String(device_key),
    device: String(device),
    status: String(status),
    timestamp: timestamp || new Date().toISOString(),
    uptime: uptime || null,
    localip: localip || null,
    resent: !!resent,
    createdAt: new Date().toISOString(),
  };

  try {
    const data = readData();
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
  if (!device_key || !device || !status) return res.status(400).json({ ok: false });

  const result = { device_key, device, status, timestamp: timestamp || new Date().toISOString(), uptime, localip, receivedAt: new Date().toISOString() };
  latestStatus.set(device_key, result);
  setTimeout(() => latestStatus.delete(device_key), 30000);
  res.json({ ok: true, data: result });
});

app.get("/status/latest", (req, res) => {
  const data = latestStatus.get(req.query.device_key);
  res.json(data ? { ok: true, data } : { ok: false });
});

// UI với pagination
// ==== UI: TRANG CHÍNH + BẢNG + POPUP + PAGINATION ====
app.get("/data", async (req, res) => {
  const page = Math.max(1, parseInt(req.query.page) || 1);
  const limit = 50;
  const data = readData().reverse(); // Mới nhất lên đầu
  const total = data.length;
  const pages = Math.ceil(total / limit);
  const start = (page - 1) * limit;
  const end = Math.min(start + limit, total);

  const rows = data
    .slice(start, end)
    .map((r, i) => `
      <tr>
        <td>${total - start - i}</td>
        <td><strong>${r.device_key}</strong></td>
        <td>${r.device}</td>
        <td>${r.status}</td>
        <td>${r.timestamp}</td>
        <td>${r.uptime || ""}</td>
        <td>${r.localip || ""}</td>
        <td>${new Date(r.createdAt).toLocaleString("vi-VN")}</td>
      </tr>`)
    .join("");

  const deviceKeys = [...new Set(data.map(r => r.device_key).filter(Boolean))].sort();

  // Pagination HTML
  const pagination = `
    <div style="margin:20px 0;text-align:center;font-size:1.1rem;">
      ${page > 1 ? `<a href="/data?page=${page-1}" class="btn">Trước</a>` : '<span class="btn disabled">Trước</span>'}
      <span style="margin:0 15px;">Trang <strong>${page}</strong> / <strong>${pages}</strong></span>
      ${page < pages ? `<a href="/data?page=${page+1}" class="btn">Sau</a>` : '<span class="btn disabled">Sau</span>'}
      <div style="margin-top:8px;font-size:0.9rem;color:#666;">
        Hiển thị ${start + 1}–${end} trong tổng số <strong>${total}</strong> bản ghi
      </div>
    </div>
  `;

  const html = `
<!DOCTYPE html>
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
    .actions { margin-bottom: 20px; display: flex; gap: 12px; flex-wrap: wrap; }
    .btn {
      padding: 10px 18px; border: none; border-radius: 8px; font-weight: 500; cursor: pointer;
      text-decoration: none; display: inline-block; transition: all 0.2s; font-size: 0.95rem;
    }
    .btn-primary { background: var(--primary); color: white; }
    .btn-danger { background: var(--danger); color: white; }
    .btn:hover { opacity: 0.9; transform: translateY(-1px); box-shadow: 0 4px 8px rgba(0,0,0,0.15); }
    .btn.disabled { background: #ccc; cursor: not-allowed; opacity: 0.6; pointer-events: none; }

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
    select, button { width: 100%; padding: 12px; margin: 8px 0; border: 1px solid #ddd; border-radius: 8px; font-size: 1rem; }
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
      <a href="/data/clear" class="btn btn-danger" onclick="return confirm('Xóa toàn bộ dữ liệu?')">Xóa tất cả</a>
    </div>

    ${pagination}

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

app.listen(PORT, () => {
  console.log(`Server chạy tại http://localhost:${PORT}/data`);
});