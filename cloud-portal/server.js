const express = require('express');
const mqtt = require('mqtt');

const PORT = Number(process.env.PORT || 3000);
const MQTT_URL = process.env.MQTT_URL || 'mqtt://192.168.0.115:1883';
const MQTT_USER = process.env.MQTT_USER || 'ukwms';
const MQTT_PASSWORD = process.env.MQTT_PASSWORD || 'ukwms';
const TICKET_TTL_MS = 20 * 60 * 1000;

const packages = [
  { minutes: 30, price: 3000, title: 'Basic', note: 'Cocok untuk top-up cepat' },
  { minutes: 60, price: 5000, title: 'Standard', note: 'Pilihan paling seimbang' },
  { minutes: 120, price: 8000, title: 'Long Charge', note: 'Untuk charging lebih lama' },
];

const tickets = new Map();
const paidTickets = new Set();
const devices = new Map();
const events = [];
let mqttOnline = false;

const mqttOptions = {
  reconnectPeriod: 3000,
  connectTimeout: 5000,
};
if (MQTT_USER) {
  mqttOptions.username = MQTT_USER;
  mqttOptions.password = MQTT_PASSWORD;
}

const mqttClient = mqtt.connect(MQTT_URL, mqttOptions);

mqttClient.on('connect', () => {
  mqttOnline = true;
  console.log(`[MQTT] Online: ${MQTT_URL}`);
  mqttClient.subscribe('smartcharge/+/ticket');
  mqttClient.subscribe('smartcharge/+/status');
  mqttClient.subscribe('smartcharge/+/event');
  mqttClient.subscribe('smartcharge/+/availability');
});

mqttClient.on('close', () => {
  if (mqttOnline) console.log('[MQTT] Offline');
  mqttOnline = false;
});

mqttClient.on('error', (err) => {
  if (!mqttOnline) console.log(`[MQTT] ${err.message}`);
});

mqttClient.on('message', (topic, payloadBuffer) => {
  const payload = payloadBuffer.toString();
  const parts = topic.split('/');
  const device = parts[1];
  const channel = parts[2];
  if (!device || !channel) return;

  if (channel === 'ticket') {
    const p = payload.split('|');
    if (p[0] === 'TICKET' && p[1]) {
      tickets.set(ticketKey(device, p[1]), {
        device,
        ticket: p[1],
        url: p[2] || '',
        used: paidTickets.has(ticketKey(device, p[1])),
        createdAt: Date.now(),
      });
      pushEvent(device, `Ticket dibuat: ${p[1]}`);
    }
    return;
  }

  if (channel === 'status') {
    const current = devices.get(device) || { device };
    devices.set(device, {
      ...current,
      device,
      online: current.online !== false,
      status: parseStatusPayload(payload),
      rawStatus: payload,
      updatedAt: Date.now(),
    });
    return;
  }

  if (channel === 'availability') {
    const current = devices.get(device) || { device };
    devices.set(device, {
      ...current,
      device,
      online: payload.trim() === 'online',
      availability: payload.trim(),
      updatedAt: Date.now(),
    });
    return;
  }

  if (channel === 'event') {
    pushEvent(device, payload);
  }
});

function ticketKey(device, ticket) {
  return `${device}:${ticket}`;
}

function cleanupTickets() {
  const now = Date.now();
  for (const [key, value] of tickets.entries()) {
    if (!value.used && now - value.createdAt > TICKET_TTL_MS) tickets.delete(key);
  }
}

function getTicket(device, ticket) {
  cleanupTickets();
  return tickets.get(ticketKey(device, ticket));
}

function pushEvent(device, message) {
  events.unshift({ device, message, at: Date.now() });
  if (events.length > 25) events.pop();
}

function rupiah(n) {
  return 'Rp ' + Number(n || 0).toLocaleString('id-ID');
}

function escapeHtml(value) {
  return String(value ?? '')
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;')
    .replace(/'/g, '&#39;');
}

function parseStatusPayload(payload) {
  const result = { ports: [] };
  String(payload || '').split('|').forEach((part) => {
    const [key, ...rest] = part.split('=');
    if (!rest.length) return;
    const value = rest.join('=');
    if (key === 'ports') result.ports = value.split(',').map((x) => x === '1');
    else result[key] = value;
  });
  return result;
}

function timeAgo(ts) {
  if (!ts) return 'belum ada data';
  const sec = Math.max(1, Math.floor((Date.now() - ts) / 1000));
  if (sec < 60) return `${sec} detik lalu`;
  const min = Math.floor(sec / 60);
  if (min < 60) return `${min} menit lalu`;
  return `${Math.floor(min / 60)} jam lalu`;
}

function devicePortBusy(device, port) {
  const d = devices.get(device);
  return Boolean(d?.status?.ports?.[port - 1]);
}

function deviceSummary(device) {
  const d = devices.get(device);
  if (!d) return { online: false, active: 0, ports: Array(8).fill(false), updatedAt: 0 };
  const ports = d.status?.ports?.length ? d.status.ports : Array(8).fill(false);
  return {
    online: d.online !== false,
    active: ports.filter(Boolean).length,
    ports,
    updatedAt: d.updatedAt,
    sta: d.status?.sta || '-',
  };
}

function layout(title, body, opts = {}) {
  const subtitle = opts.subtitle || 'Smart charging trainer portal';
  return `<!doctype html>
<html lang="id">
<head>
  <meta charset="utf-8" />
  <meta name="viewport" content="width=device-width,initial-scale=1" />
  <meta name="theme-color" content="#0f172a" />
  <title>${escapeHtml(title)}</title>
  <style>
    :root{--bg:#f4f7fb;--panel:#ffffff;--text:#0f172a;--muted:#64748b;--line:#e2e8f0;--brand:#2563eb;--brand2:#14b8a6;--danger:#ef4444;--ok:#16a34a;--warn:#f59e0b;--shadow:0 20px 50px rgba(15,23,42,.10);font-family:Inter,ui-sans-serif,system-ui,-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;color:var(--text);background:radial-gradient(circle at top left,#dbeafe 0,#f4f7fb 38%,#f8fafc 100%)}
    *{box-sizing:border-box}body{margin:0;min-height:100vh}.shell{width:min(1040px,100%);margin:0 auto;padding:22px}.topbar{display:flex;justify-content:space-between;align-items:center;gap:16px;margin-bottom:18px}.brand{display:flex;align-items:center;gap:12px}.logo{width:46px;height:46px;border-radius:16px;background:linear-gradient(135deg,var(--brand),var(--brand2));box-shadow:var(--shadow);display:grid;place-items:center;color:white;font-weight:950}.brand h1{font-size:20px;margin:0}.brand p{margin:2px 0 0;color:var(--muted);font-size:13px}.pill{display:inline-flex;align-items:center;gap:7px;border:1px solid var(--line);background:rgba(255,255,255,.72);border-radius:999px;padding:8px 12px;color:var(--muted);font-size:13px;font-weight:800}.dot{width:9px;height:9px;border-radius:999px;background:var(--danger)}.dot.ok{background:var(--ok)}.hero{background:linear-gradient(135deg,#0f172a,#1d4ed8 58%,#0d9488);color:white;border-radius:30px;padding:28px;box-shadow:var(--shadow);overflow:hidden;position:relative}.hero:after{content:"";position:absolute;width:180px;height:180px;border-radius:50%;background:rgba(255,255,255,.10);right:-56px;top:-64px}.hero h2{font-size:32px;line-height:1.08;margin:0 0 10px}.hero p{margin:0;color:#dbeafe}.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:14px}.grid.small{grid-template-columns:repeat(auto-fit,minmax(135px,1fr))}.card{background:rgba(255,255,255,.92);border:1px solid rgba(226,232,240,.9);border-radius:24px;padding:18px;box-shadow:0 12px 30px rgba(15,23,42,.06);backdrop-filter:blur(10px)}.section{margin-top:18px}.label{display:block;margin:0 0 9px;font-size:13px;color:var(--muted);font-weight:900;text-transform:uppercase;letter-spacing:.06em}.title{font-size:22px;font-weight:950;margin:0 0 8px}.muted{color:var(--muted)}.price{font-size:34px;font-weight:950;letter-spacing:-.04em}.btn{appearance:none;border:0;border-radius:16px;padding:14px 16px;min-height:48px;display:inline-flex;align-items:center;justify-content:center;text-decoration:none;background:var(--brand);color:white;font-weight:950;cursor:pointer;width:100%;box-shadow:0 12px 22px rgba(37,99,235,.22)}.btn.secondary{background:#eef2ff;color:#1d4ed8;box-shadow:none}.btn.ghost{background:white;color:var(--text);box-shadow:none;border:1px solid var(--line)}.btn.danger{background:var(--danger);box-shadow:none}.status{display:inline-flex;border-radius:999px;padding:6px 10px;font-size:12px;font-weight:950}.status.ok{background:#dcfce7;color:#166534}.status.off{background:#fee2e2;color:#991b1b}.status.idle{background:#f1f5f9;color:#475569}.status.warn{background:#fef3c7;color:#92400e}.choices{display:grid;grid-template-columns:repeat(auto-fit,minmax(145px,1fr));gap:12px}.choice input{position:absolute;opacity:0}.choice span{display:block;border:2px solid var(--line);border-radius:20px;padding:16px;background:white;cursor:pointer;min-height:116px}.choice input:checked+span{border-color:var(--brand);box-shadow:0 0 0 5px rgba(37,99,235,.10)}.choice input:disabled+span{opacity:.48;cursor:not-allowed;background:#f8fafc}.choice b{font-size:20px}.choice small{display:block;color:var(--muted);margin-top:6px}.steps{display:grid;grid-template-columns:repeat(3,1fr);gap:10px;margin:16px 0}.step{background:rgba(255,255,255,.16);border:1px solid rgba(255,255,255,.20);border-radius:16px;padding:12px}.step b{display:block}.table{width:100%;border-collapse:collapse}.table td,.table th{border-bottom:1px solid var(--line);padding:12px;text-align:left}.table th{font-size:12px;color:var(--muted);text-transform:uppercase;letter-spacing:.06em}.mono{font-family:ui-monospace,SFMono-Regular,Menlo,monospace;font-size:12px}.footer{padding:20px 0;color:var(--muted);font-size:12px;text-align:center}@media(max-width:640px){.shell{padding:14px}.topbar{align-items:flex-start;flex-direction:column}.hero{border-radius:24px;padding:22px}.hero h2{font-size:28px}.steps{grid-template-columns:1fr}.price{font-size:30px}}
  </style>
</head>
<body>
  <main class="shell">
    <header class="topbar">
      <div class="brand"><div class="logo">SC</div><div><h1>SmartCharge</h1><p>${escapeHtml(subtitle)}</p></div></div>
      <div class="pill"><span class="dot ${mqttOnline ? 'ok' : ''}"></span> MQTT ${mqttOnline ? 'Online' : 'Offline'}</div>
    </header>
    ${body}
    <div class="footer">SmartCharge Trainer · Mosquitto ${escapeHtml(MQTT_URL)} · Port ${PORT}</div>
  </main>
</body>
</html>`;
}

function invalidPage(title, message, status = 400) {
  return {
    status,
    html: layout(title, `
      <section class="hero"><h2>${escapeHtml(title)}</h2><p>${escapeHtml(message)}</p></section>
      <section class="section card"><a class="btn secondary" href="/">Kembali ke dashboard</a></section>
    `),
  };
}

const app = express();
app.use(express.urlencoded({ extended: false }));
app.use(express.json());

app.get('/', (req, res) => {
  cleanupTickets();
  const deviceCards = Array.from(devices.values()).map((d) => {
    const summary = deviceSummary(d.device);
    const portBadges = summary.ports.map((active, index) => `<span class="status ${active ? 'ok' : 'idle'}">P${index + 1} ${active ? 'ON' : 'OFF'}</span>`).join(' ');
    return `<div class="card">
      <div class="label">Device</div>
      <div class="title">${escapeHtml(d.device)}</div>
      <p><span class="status ${summary.online ? 'ok' : 'off'}">${summary.online ? 'Online' : 'Offline'}</span> <span class="muted">${timeAgo(summary.updatedAt)}</span></p>
      <p class="muted">STA IP: ${escapeHtml(summary.sta)}</p>
      <div style="display:flex;flex-wrap:wrap;gap:6px">${portBadges}</div>
    </div>`;
  }).join('') || '<div class="card"><div class="title">Belum ada device</div><p class="muted">Nyalakan ESP32 sampai MQTT Online, lalu tekan tombol print.</p></div>';

  const recentTickets = Array.from(tickets.values()).slice(-8).reverse().map((t) => `<tr><td class="mono">${escapeHtml(t.ticket)}</td><td>${escapeHtml(t.device)}</td><td>${t.used ? '<span class="status ok">used</span>' : '<span class="status warn">ready</span>'}</td><td>${timeAgo(t.createdAt)}</td></tr>`).join('') || '<tr><td colspan="4" class="muted">Belum ada ticket.</td></tr>';
  const recentEvents = events.slice(0, 6).map((e) => `<tr><td>${escapeHtml(e.device)}</td><td>${escapeHtml(e.message)}</td><td>${timeAgo(e.at)}</td></tr>`).join('') || '<tr><td colspan="3" class="muted">Belum ada event.</td></tr>';

  res.send(layout('SmartCharge Dashboard', `
    <section class="hero">
      <h2>Dashboard SmartCharge</h2>
      <p>Monitor alat, ticket QR thermal, dan status port charger dari MQTT.</p>
      <div class="steps">
        <div class="step"><b>1. Print QR</b><span>Tekan tombol S0</span></div>
        <div class="step"><b>2. User scan</b><span>Buka halaman payment</span></div>
        <div class="step"><b>3. Start charge</b><span>Relay aktif via MQTT</span></div>
      </div>
    </section>

    <section class="section grid">${deviceCards}</section>

    <section class="section card">
      <div class="label">Ticket terakhir</div>
      <table class="table"><thead><tr><th>Ticket</th><th>Device</th><th>Status</th><th>Waktu</th></tr></thead><tbody>${recentTickets}</tbody></table>
    </section>

    <section class="section card">
      <div class="label">Event MQTT</div>
      <table class="table"><thead><tr><th>Device</th><th>Event</th><th>Waktu</th></tr></thead><tbody>${recentEvents}</tbody></table>
    </section>
  `, { subtitle: 'Dashboard operator' }));
});

app.get('/pay', (req, res) => {
  const device = String(req.query.device || '').trim();
  const ticket = String(req.query.ticket || '').trim();
  const key = ticketKey(device, ticket);
  const t = getTicket(device, ticket);

  if (!device || !ticket) {
    const page = invalidPage('QR tidak valid', 'QR tidak membawa data device atau ticket. Cetak ticket baru dari alat.');
    res.status(page.status).send(page.html);
    return;
  }

  if (paidTickets.has(key) || t?.used) {
    const page = invalidPage('Ticket sudah digunakan', 'Silakan cetak ticket baru dari alat SmartCharge.');
    res.status(409).send(page.html);
    return;
  }

  const status = deviceSummary(device);
  const portChoices = Array.from({ length: 8 }, (_, i) => {
    const port = i + 1;
    const busy = devicePortBusy(device, port);
    return `<label class="choice"><input type="radio" name="port" value="${port}" ${busy ? 'disabled' : ''} ${!busy && port === 1 ? 'checked' : ''}><span><b>Port ${port}</b><small>${busy ? 'Sedang digunakan' : 'Tersedia untuk charging'}</small></span></label>`;
  }).join('');

  const packageChoices = packages.map((p, i) => `<label class="choice"><input type="radio" name="pkg" value="${i}" ${i === 1 ? 'checked' : ''}><span><b>${p.title}</b><small>${p.minutes} menit · ${rupiah(p.price)}</small><small>${p.note}</small></span></label>`).join('');
  const ticketNote = t ? `<span class="status ok">Ticket terverifikasi</span>` : `<span class="status warn">Mode trainer</span> <span class="muted">Ticket belum masuk ke cache web, tapi tetap bisa dipakai jika ESP32 mengenal ticket ini.</span>`;

  res.send(layout('SmartCharge Payment', `
    <section class="hero">
      <h2>Mulai Charging</h2>
      <p>Pilih port yang sesuai dengan colokan alat, lalu pilih paket charging.</p>
      <div class="steps">
        <div class="step"><b>Ticket</b><span class="mono">${escapeHtml(ticket)}</span></div>
        <div class="step"><b>Device</b><span>${escapeHtml(device)}</span></div>
        <div class="step"><b>Status</b><span>${ticketNote}</span></div>
      </div>
    </section>

    <form method="POST" action="/confirm" class="section card">
      <input type="hidden" name="device" value="${escapeHtml(device)}">
      <input type="hidden" name="ticket" value="${escapeHtml(ticket)}">
      <div class="label">Pilih port charger</div>
      <div class="choices">${portChoices}</div>
      <div style="height:18px"></div>
      <div class="label">Pilih paket</div>
      <div class="choices">${packageChoices}</div>
      <div style="height:18px"></div>
      <button class="btn" type="submit">Lanjutkan pembayaran</button>
    </form>
  `, { subtitle: 'Payment custom trainer' }));
});

app.post('/confirm', (req, res) => {
  const device = String(req.body.device || '').trim();
  const ticket = String(req.body.ticket || '').trim();
  const port = Number(req.body.port || 0);
  const pkgIndex = Number(req.body.pkg || 0);
  const pkg = packages[pkgIndex];

  if (!device || !ticket || port < 1 || port > 8 || !pkg) {
    const page = invalidPage('Data tidak lengkap', 'Ulangi scan QR dan pilih port serta paket dengan benar.');
    res.status(page.status).send(page.html);
    return;
  }

  if (paidTickets.has(ticketKey(device, ticket))) {
    const page = invalidPage('Ticket sudah digunakan', 'Cetak ticket baru dari alat SmartCharge.');
    res.status(409).send(page.html);
    return;
  }

  res.send(layout('Konfirmasi Pembayaran', `
    <section class="hero">
      <h2>Konfirmasi Pembayaran</h2>
      <p>Periksa pilihan sebelum memulai charging.</p>
    </section>
    <section class="section grid">
      <div class="card"><div class="label">Port</div><div class="title">Port ${port}</div><p class="muted">Pastikan kabel masuk ke port ini.</p></div>
      <div class="card"><div class="label">Durasi</div><div class="title">${pkg.minutes} menit</div><p class="muted">Timer berjalan setelah tombol bayar ditekan.</p></div>
      <div class="card"><div class="label">Nominal</div><div class="price">${rupiah(pkg.price)}</div><p class="muted">Custom payment untuk trainer.</p></div>
    </section>
    <section class="section card">
      <div class="label">Mode pembayaran</div>
      <p class="muted">Belum memakai payment gateway asli. Tombol di bawah dianggap sebagai pembayaran berhasil, lalu web mengirim command MQTT ke ESP32.</p>
      <form method="POST" action="/paid">
        <input type="hidden" name="device" value="${escapeHtml(device)}">
        <input type="hidden" name="ticket" value="${escapeHtml(ticket)}">
        <input type="hidden" name="port" value="${port}">
        <input type="hidden" name="minutes" value="${pkg.minutes}">
        <input type="hidden" name="price" value="${pkg.price}">
        <button class="btn" type="submit">Saya sudah bayar, mulai charge</button>
      </form>
      <p><a class="btn secondary" href="/pay?device=${encodeURIComponent(device)}&ticket=${encodeURIComponent(ticket)}">Ubah pilihan</a></p>
    </section>
  `, { subtitle: 'Konfirmasi transaksi' }));
});

app.post('/paid', (req, res) => {
  const device = String(req.body.device || '').trim();
  const ticket = String(req.body.ticket || '').trim();
  const port = Number(req.body.port || 0);
  const minutes = Number(req.body.minutes || 0);
  const price = Number(req.body.price || 0);
  const key = ticketKey(device, ticket);

  if (!device || !ticket || port < 1 || port > 8 || minutes <= 0) {
    const page = invalidPage('Data tidak valid', 'Ulangi scan QR dari ticket yang baru dicetak.');
    res.status(page.status).send(page.html);
    return;
  }

  if (paidTickets.has(key)) {
    const page = invalidPage('Ticket sudah digunakan', 'Ticket ini sudah memulai transaksi. Cetak ticket baru untuk transaksi berikutnya.');
    res.status(409).send(page.html);
    return;
  }

  if (!mqttOnline) {
    const page = invalidPage('MQTT belum online', 'Server web belum tersambung ke Mosquitto. Periksa broker lalu coba lagi.', 503);
    res.status(page.status).send(page.html);
    return;
  }

  const topic = `smartcharge/${device}/cmd`;
  const cmd = `START|${ticket}|${port}|${minutes}|${price}`;
  mqttClient.publish(topic, cmd, { qos: 0, retain: false });
  paidTickets.add(key);
  const t = getTicket(device, ticket);
  if (t) t.used = true;
  pushEvent(device, `START dikirim: Port ${port}, ${minutes} menit`);

  res.send(layout('Charging Dimulai', `
    <section class="hero">
      <h2>Charging dimulai</h2>
      <p>Perintah sudah dikirim ke alat. Silakan gunakan port yang dipilih.</p>
    </section>
    <section class="section card">
      <div class="grid small">
        <div><div class="label">Port</div><div class="title">Port ${port}</div></div>
        <div><div class="label">Durasi</div><div class="title">${minutes} menit</div></div>
        <div><div class="label">Nominal</div><div class="title">${rupiah(price)}</div></div>
      </div>
      <p class="muted mono">Ticket: ${escapeHtml(ticket)}</p>
      <a class="btn secondary" href="/">Selesai</a>
    </section>
  `, { subtitle: 'Transaksi berhasil' }));
});

app.get('/api/status', (req, res) => {
  cleanupTickets();
  res.json({
    mqttOnline,
    mqttUrl: MQTT_URL,
    devices: Array.from(devices.values()),
    tickets: Array.from(tickets.values()),
    events,
  });
});

app.listen(PORT, () => {
  console.log(`\n=== SmartCharge Cloud Portal ===`);
  console.log(`[WEB]  http://0.0.0.0:${PORT}`);
  console.log(`[MQTT] ${MQTT_URL}`);
  console.log(`[USER] ${MQTT_USER || '(anonymous)'}`);
});
