const express = require("express");
const fs = require("fs");
const path = require("path");

const app = express();
const PORT = process.env.PORT || 3000;

// ==== Cấu hình file dữ liệu ====
const DATA_FILE = path.join(__dirname, "data.json");

// Tạo file nếu chưa tồn tại
function ensureDataFile() {
  try {
    if (!fs.existsSync(DATA_FILE)) {
      fs.writeFileSync(DATA_FILE, "[]", "utf8");
    }
  } catch (err) {
    console.error("Failed to ensure data file:", err);
  }
}
ensureDataFile();

// Đọc dữ liệu (trả về mảng)
function readData() {
  try {
    const raw = fs.readFileSync(DATA_FILE, "utf8");
    return JSON.parse(raw || "[]");
  } catch (err) {
    console.error("Failed to read data:", err);
    return [];
  }
}

// Ghi dữ liệu (ghi đè toàn bộ mảng)
function writeData(arr) {
  try {
    // Ghi tạm rồi rename để hạn chế rủi ro ghi hỏng
    const tmp = DATA_FILE + ".tmp";
    fs.writeFileSync(tmp, JSON.stringify(arr, null, 2), "utf8");
    fs.renameSync(tmp, DATA_FILE);
  } catch (err) {
    console.error("Failed to write data:", err);
    throw err;
  }
}

app.use(express.json());

// Healthcheck (hữu ích khi deploy Render)
app.get("/healthz", (req, res) => {
  res.status(200).send("ok");
});

// Trang chủ
app.get("/", (req, res) => {
  res.send("Hello, World!");
});

/**
 * 1) Ghi data vào file .json (GET /log)
 * Params: device, status, timestamp, uptime, localip
 * Ví dụ: /log?device=esp32-01&status=online&timestamp=1730640000&uptime=3600&localip=192.168.1.10
 */
app.post("/log", (req, res) => {
  const body = req.body; 
  const { device, status, timestamp, uptime, localip } = body;

  // Kiểm tra tối thiểu
  if (!device || !status) {
    return res.status(400).json({
      ok: false,
      message: "Thiếu tham số bắt buộc: device, status",
    });
  }

  const nowIso = new Date().toISOString();
  const record = {
    device: String(device),
    status: String(status),
    timestamp: timestamp ? String(timestamp) : nowIso, // nếu không có, dùng thời điểm server nhận
    uptime: uptime ? Number(uptime) : null,
    localip: localip ? String(localip) : null,
    createdAt: nowIso, // server-side timestamp
  };

  try {
    const data = readData();
    data.push(record);
    writeData(data);
    return res.json({ ok: true, saved: record, total: data.length });
  } catch (err) {
    return res.status(500).json({ ok: false, message: "Ghi dữ liệu thất bại" });
  }
});

/**
 * 2) Đọc data từ file .json, hiển thị dạng bảng (GET /data)
 */
app.get("/data", (req, res) => {
  const data = readData();

  // Render HTML table đơn giản
  const rows = data
    .map(
      (r, i) => `
        <tr>
          <td>${i + 1}</td>
          <td>${r.device ?? ""}</td>
          <td>${r.status ?? ""}</td>
          <td>${r.timestamp ?? ""}</td>
          <td>${r.uptime ?? ""}</td>
          <td>${r.localip ?? ""}</td>
          <td>${r.createdAt ?? ""}</td>
        </tr>`
    )
    .join("");

  const html = `
    <!DOCTYPE html>
    <html lang="vi">
    <head>
      <meta charset="UTF-8" />
      <meta name="viewport" content="width=device-width, initial-scale=1.0"/>
      <title>Data Table</title>
      <style>
        body { font-family: system-ui, -apple-system, Roboto, sans-serif; padding: 16px; }
        table { border-collapse: collapse; width: 100%; }
        th, td { border: 1px solid #ddd; padding: 8px; }
        th { background: #f6f6f6; text-align: left; }
        tr:nth-child(even) { background: #fafafa; }
        .actions { margin-bottom: 12px; }
        .btn { display:inline-block; padding:8px 12px; border:1px solid #ddd; border-radius:8px; text-decoration:none; }
      </style>
    </head>
    <body>
      <h1>Dữ liệu ghi từ /log</h1>
      <div class="actions">
        <a class="btn" href="/data/clear" onclick="return confirm('Xóa hết dữ liệu?')">Xóa tất cả</a>
      </div>
      <table>
        <thead>
          <tr>
            <th>#</th>
            <th>device</th>
            <th>status</th>
            <th>timestamp</th>
            <th>uptime</th>
            <th>localip</th>
            <th>createdAt (server)</th>
          </tr>
        </thead>
        <tbody>${rows || '<tr><td colspan="7" style="text-align:center;">(Chưa có dữ liệu)</td></tr>'}</tbody>
      </table>
    </body>
    </html>
  `;

  res.setHeader("Content-Type", "text/html; charset=utf-8");
  res.send(html);
});

/**
 * 3) Xóa tất cả data trong file .json (GET /data/clear)
 * (Trong production nên dùng DELETE/POST + xác thực)
 */
app.get("/data/clear", (req, res) => {
  try {
    writeData([]);
    res.redirect("/data");
  } catch (err) {
    res.status(500).json({ ok: false, message: "Xóa dữ liệu thất bại" });
  }
});

let pendingRequest = null;          // lưu request đang chờ

app.get('/wait', (req, res) => {
  pendingRequest = res;             // giữ kết nối
  req.on('close', () => pendingRequest = null);
  // timeout 60s để tránh treo vĩnh viễn
  setTimeout(() => {
    if (pendingRequest === res) {
      res.json({cmd: 'none'});
      pendingRequest = null;
    }
  }, 60000);
});

app.post('/trigger', (req, res) => {
  if (pendingRequest) {
    pendingRequest.json({cmd: 'send'});
    pendingRequest = null;
    res.send('triggered');
  } else {
    res.send('no client waiting');
  }
});

app.listen(PORT, () => {
  console.log(`Server listening on port ${PORT}`);
});
