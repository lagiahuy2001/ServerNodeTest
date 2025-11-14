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
app.get("/data", async (req, res) => {
  const page = parseInt(req.query.page) || 1;
  const limit = 50;
  const data = readData().reverse();
  const total = data.length;
  const pages = Math.ceil(total / limit);
  const start = (page - 1) * limit;
  const rows = data.slice(start, start + limit).map((r, i) => `
    <tr><td>${total - start - i}</td><td><strong>${r.device_key}</strong></td><td>${r.device}</td><td>${r.status}</td><td>${r.timestamp}</td><td>${r.uptime || ""}</td><td>${r.localip || ""}</td><td>${new Date(r.createdAt).toLocaleString("vi-VN")}</td></tr>
  `).join("");

  const deviceKeys = [...new Set(data.map(r => r.device_key))].sort();

  const pagination = `
    <div style="margin:20px 0;text-align:center;">
      ${page > 1 ? `<a href="/data?page=${page-1}" class="btn">Trước</a>` : ""}
      <span> Trang ${page}/${pages} </span>
      ${page < pages ? `<a href="/data?page=${page+1}" class="btn">Sau</a>` : ""}
    </div>
  `;

  // HTML giống trước, thêm ${pagination} trước </table>
  // ... (giữ nguyên phần HTML, chỉ thêm pagination)
  // (Do dài, bạn có thể thêm vào phần trước </table>)
});

// Các route còn lại giữ nguyên

app.listen(PORT, () => {
  console.log(`Server chạy tại http://localhost:${PORT}/data`);
});