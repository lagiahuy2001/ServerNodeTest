const express = require("express");
const fs = require("fs");
const path = require("path");

const app = express();
const PORT = process.env.PORT || 3000;

// ==== CẤU HÌNH FILE DỮ LIỆU ====
const DATA_FILE = path.join(__dirname, "data.json");

// Tạo file nếu chưa có
function ensureDataFile() {
  if (!fs.existsSync(DATA_FILE)) {
    fs.writeFileSync(DATA_FILE, "[]", "utf8");
  }
}
ensureDataFile();

// Đọc dữ liệu
function readData() {
  try {
    const raw = fs.readFileSync(DATA_FILE, "utf8");
    return JSON.parse(raw || "[]");
  } catch (err) {
    console.error("Lỗi đọc file:", err);
    return [];
  }
}

// Ghi dữ liệu (an toàn)
function writeData(arr) {
  const tmp = DATA_FILE + ".tmp";
  fs.writeFileSync(tmp, JSON.stringify(arr, null, 2), "utf8");
  fs.renameSync(tmp, DATA_FILE);
}

app.use(express.json());

// Healthcheck
app.get("/healthz", (req, res) => res.status(200).send("ok"));

// ==== LONG-POLL & TRIGGER ====
const pendingRequests = new Map();  // device_key → res

app.get("/wait", (req, res) => {
  const device_key = req.query.device_key;
  if (!device_key) return res.status(400).json({ error: "Thiếu device_key" });

  pendingRequests.set(device_key, res);
  req.on("close", () => pendingRequests.delete(device_key));

  setTimeout(() => {
    if (pendingRequests.get(device_key) === res) {
      res.json({ cmd: "none" });
      pendingRequests.delete(device_key);
    }
  }, 60000);
});

app.post("/trigger", (req, res) => {
  const device_key = req.query.device_key;
  if (!device_key) return res.status(400).send("Thiếu device_key");

  const client = pendingRequests.get(device_key);
  if (client) {
    client.json({ cmd: "send" });
    pendingRequests.delete(device_key);
    res.send(`Đã gửi lệnh đến ${device_key}`);
  } else {
    res.send(`Không tìm thấy thiết bị: ${device_key}`);
  }
});

// ==== API: LƯU DỮ LIỆU TỰ ĐỘNG (BẬT/TẮT) ====
app.post("/log", (req, res) => {
  const { device_key, device, status, timestamp, uptime, localip, resent } = req.body;

  if (!device_key || !device || !status) {
    return res.status(400).json({ ok: false, message: "Thiếu device_key/device/status" });
  }

  const record = {
    device_key: String(device_key),
    device: String(device),
    status: String(status),
    timestamp: timestamp ? String(timestamp) : new Date().toISOString(),
    uptime: uptime ? String(uptime) : null,
    localip: localip ? String(localip) : null,
    resent: !!resent,
    createdAt: new Date().toISOString(),
  };

  try {
    const data = readData();
    data.push(record);
    writeData(data);
    res.json({ ok: true, saved: record, total: data.length });
  } catch (err) {
    res.status(500).json({ ok: false, message: "Lỗi ghi file" });
  }
});

// ==== API: TRẢ VỀ TRẠNG THÁI THEO YÊU CẦU (CHỈ POPUP) ====
const latestStatus = new Map();  // device_key → data (tạm 30s)

app.post("/status", (req, res) => {
  const { device_key, device, status, timestamp, uptime, localip } = req.body;

  if (!device_key || !device || !status) {
    return res.status(400).json({ ok: false });
  }

  const result = {
    device_key,
    device,
    status,
    timestamp: timestamp || new Date().toISOString(),
    uptime: uptime || null,
    localip: localip || null,
    receivedAt: new Date().toISOString(),
  };

  latestStatus.set(device_key, result);
  setTimeout(() => latestStatus.delete(device_key), 30000);

  res.json({ ok: true, data: result });
});

app.get("/status/latest", (req, res) => {
  const data = latestStatus.get(req.query.device_key);
  res.json(data ? { ok: true, data } : { ok: false });
});

// ==== UI: TRANG CHÍNH + BẢNG + POPUP ====
app.get("/data", (req, res) => {
  const data = readData();
  const deviceKeys = [...new Set(data.map(r => r.device_key).filter(Boolean))].sort();

  const rows = data
    .map((r, i) => `
      <tr>
        <td>${i + 1}</td>
        <td><strong>${r.device_key}</strong></td>
        <td>${r.device}</td>
        <td>${r.status}</td>
        <td>${r.timestamp}</td>
        <td>${r.uptime || ""}</td>
        <td>${r.localip || ""}</td>
        <td>${new Date(r.createdAt).toLocaleString("vi-VN")}</td>
      </tr>`)
    .join("");

  const html = `
<!DOCTYPE html>
<html lang="vi">
<head>
  <meta charset="UTF-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1.0"/>
  <title>IoT Monitor</title>
  <style>
    body { font-family: system-ui, sans-serif; margin: 0; padding: 16px; background: #f9f9fb; }
    .container { max-width: 1200px; margin: auto; background: white; border-radius: 12px; padding: 20px; box-shadow: 0 2px 10px rgba(0,0,0,0.1); }
    h1 { margin: 0 0 16px; color: #1a1a1a; }
    .actions { margin-bottom: 16px; display: flex; gap: 12px; }
    .btn { padding: 10px 16px; border: none; border-radius: 8px; font-weight: 500; cursor: pointer; transition: 0.2s; }
    .btn-primary { background: #007bff; color: white; }
    .btn-danger { background: #dc3545; color: white; }
    .btn:hover { opacity: 0.9; transform: translateY(-1px); }
    table { width: 100%; border-collapse: collapse; margin-top: 16px; }
    th, td { padding: 10px; text-align: left; border-bottom: 1px solid #eee; }
    th { background: #f8f9fa; font-weight: 600; }
    tr:hover { background: #f5f5f5; }
    .empty { text-align: center; color: #666; padding: 32px; }

    /* Popup */
    .modal { display: none; position: fixed; top: 0; left: 0; width: 100%; height: 100%; background: rgba(0,0,0,0.5); justify-content: center; align-items: center; z-index: 1000; }
    .modal.open { display: flex; }
    .modal-content { background: white; padding: 24px; border-radius: 12px; width: 90%; max-width: 500px; box-shadow: 0 10px 30px rgba(0,0,0,0.2); }
    .modal-header { display: flex; justify-content: space-between; align-items: center; margin-bottom: 16px; }
    .modal-header h2 { margin: 0; font-size: 1.4rem; }
    .close { cursor: pointer; font-size: 1.5rem; color: #aaa; }
    .close:hover { color: #000; }
    select, button { width: 100%; padding: 10px; margin: 8px 0; border: 1px solid #ddd; border-radius: 8px; font-size: 1rem; }
    .result { margin-top: 16px; padding: 12px; background: #f0f8ff; border-radius: 8px; font-family: monospace; font-size: 0.9rem; max-height: 300px; overflow-y: auto; display: none; }
    .loading { color: #007bff; font-style: italic; }
  </style>
</head>
<body>
  <div class="container">
    <h1>IoT Monitor - Dữ liệu từ ESP</h1>
    <div class="actions">
      <button class="btn btn-primary" id="updateBtn">Cập nhật trạng thái ESP</button>
      <a href="/data/clear" class="btn btn-danger" onclick="return confirm('Xóa toàn bộ dữ liệu?')">Xóa tất cả</a>
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
        ${rows || '<tr><td colspan="8" class="empty">(Chưa có dữ liệu)</td></tr>'}
      </tbody>
    </table>
  </div>

  <!-- Popup -->
  <div class="modal" id="modal">
    <div class="modal-content">
      <div class="modal-header">
        <h2>Yêu cầu cập nhật</h2>
        <span class="close" id="closeModal">×</span>
      </div>
      <form id="triggerForm">
        <label>Chọn thiết bị:</label>
        <select id="deviceKeySelect" required>
          <option value="">-- Chọn thiết bị --</option>
          ${deviceKeys.map(k => `<option value="${k}">${k}</option>`).join("")}
        </select>
        <button type="submit" class="btn btn-primary">Gửi lệnh</button>
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
        // Gửi trigger
        const triggerRes = await fetch(\`/trigger?device_key=\${device_key}\`, { method: 'POST' });
        const triggerText = await triggerRes.text();
        resultDiv.innerHTML = \`<p><strong>Kết quả:</strong> \${triggerText}</p>\`;

        // Poll /status/latest
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

    // Tự reload bảng
    setInterval(() => location.reload(), 10000);
  </script>
</body>
</html>
  `;

  res.setHeader("Content-Type", "text/html; charset=utf-8");
  res.send(html);
});

// API: Raw JSON
app.get("/data.json", (req, res) => res.json(readData()));

// XÓA DỮ LIỆU
app.get("/data/clear", (req, res) => {
  writeData([]);
  res.redirect("/data");
});

app.listen(PORT, () => {
  console.log(`Server chạy tại http://localhost:${PORT}`);
  console.log(`UI: http://localhost:${PORT}/data`);
});