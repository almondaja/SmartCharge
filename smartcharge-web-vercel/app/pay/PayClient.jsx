'use client';

import { useMemo, useState } from 'react';
import { useSearchParams } from 'next/navigation';

// Dipakai berurutan. URL pertama mengikuti contoh resmi test.mosquitto.org untuk WebSocket TLS.
// Kalau sedang tidak tersedia, browser akan mencoba URL berikutnya.
const MQTT_WS_URLS = [
  'wss://test.mosquitto.org/mqtt',
  'wss://test.mosquitto.org:8081/mqtt',
  'wss://test.mosquitto.org:8081'
];
const MQTT_SCRIPT_URL = 'https://unpkg.com/mqtt/dist/mqtt.min.js';
const MQTT_TOPIC_ROOT = 'smartcharge/almondaja_iota';

const PACKAGES = [
  { minutes: 30, price: 3000, label: '30 menit' },
  { minutes: 60, price: 5000, label: '60 menit' },
  { minutes: 120, price: 8000, label: '120 menit' }
];

let mqttScriptPromise = null;

function rupiah(value) {
  return new Intl.NumberFormat('id-ID', {
    style: 'currency',
    currency: 'IDR',
    maximumFractionDigits: 0
  }).format(value);
}

function cleanParam(value, fallback = '') {
  return String(value || fallback)
    .replace(/[^a-zA-Z0-9_.-]/g, '')
    .slice(0, 96);
}

function loadMqttJs() {
  if (typeof window === 'undefined') {
    return Promise.reject(new Error('MQTT hanya bisa dijalankan di browser'));
  }

  if (window.mqtt) return Promise.resolve(window.mqtt);

  if (!mqttScriptPromise) {
    mqttScriptPromise = new Promise((resolve, reject) => {
      const existing = document.querySelector('script[data-mqttjs="true"]');
      if (existing) {
        existing.addEventListener('load', () => resolve(window.mqtt));
        existing.addEventListener('error', () => reject(new Error('Gagal memuat MQTT.js')));
        return;
      }

      const script = document.createElement('script');
      script.src = MQTT_SCRIPT_URL;
      script.async = true;
      script.dataset.mqttjs = 'true';
      script.onload = () => {
        if (window.mqtt) resolve(window.mqtt);
        else reject(new Error('MQTT.js tidak tersedia setelah script dimuat'));
      };
      script.onerror = () => reject(new Error('Gagal memuat MQTT.js dari CDN'));
      document.head.appendChild(script);
    });
  }

  return mqttScriptPromise;
}

function connectOnce(mqtt, url, device) {
  return new Promise((resolve, reject) => {
    const clientId = `web-${device}-${Math.random().toString(16).slice(2)}`;
    const client = mqtt.connect(url, {
      clientId,
      clean: true,
      reconnectPeriod: 0,
      connectTimeout: 8000,
      keepalive: 15,
      protocolVersion: 4
    });

    const timer = window.setTimeout(() => {
      try { client.end(true); } catch (_) {}
      reject(new Error(`Timeout konek ${url}`));
    }, 9000);

    client.once('connect', () => {
      window.clearTimeout(timer);
      resolve({ client, url });
    });

    client.once('error', (err) => {
      window.clearTimeout(timer);
      try { client.end(true); } catch (_) {}
      reject(new Error(`${url}: ${err.message}`));
    });
  });
}

async function connectMqttWithFallback(mqtt, device, setStatus) {
  const errors = [];
  for (const url of MQTT_WS_URLS) {
    try {
      setStatus(`Menghubungkan MQTT WebSocket: ${url}`);
      return await connectOnce(mqtt, url, device);
    } catch (err) {
      errors.push(err.message);
    }
  }
  throw new Error(`Semua MQTT WebSocket gagal. ${errors.join(' | ')}`);
}

export default function PayClient() {
  const searchParams = useSearchParams();
  const device = cleanParam(searchParams.get('device'), 'SC001');
  const ticket = cleanParam(searchParams.get('ticket'));
  const [port, setPort] = useState('1');
  const [status, setStatus] = useState('Siap. Pilih port dan paket.');
  const [lastBroker, setLastBroker] = useState('-');
  const [busy, setBusy] = useState(false);

  const topic = useMemo(() => `${MQTT_TOPIC_ROOT}/${device}/cmd`, [device]);
  const eventTopic = useMemo(() => `${MQTT_TOPIC_ROOT}/${device}/event`, [device]);
  const statusTopic = useMemo(() => `${MQTT_TOPIC_ROOT}/${device}/status`, [device]);
  const canSend = Boolean(ticket) && !busy;

  async function publishAndWait(payload, expectedTicket = '') {
    setBusy(true);

    try {
      const mqtt = await loadMqttJs();
      const { client, url } = await connectMqttWithFallback(mqtt, device, setStatus);
      setLastBroker(url);

      let finished = false;
      let replyTimer = null;

      const finish = (message, forceClose = false) => {
        if (finished) return;
        finished = true;
        if (replyTimer) window.clearTimeout(replyTimer);
        setStatus(message);
        setBusy(false);
        window.setTimeout(() => {
          try { client.end(forceClose); } catch (_) {}
        }, 300);
      };

      client.on('message', (rxTopic, buf) => {
        const message = buf.toString();
        if (rxTopic === eventTopic) {
          if (!expectedTicket || message.includes(expectedTicket) || message.startsWith('REJECT|')) {
            if (message.startsWith('STARTED|')) {
              finish(`ESP32 membalas: ${message}. Relay seharusnya aktif sekarang.`);
            } else if (message.startsWith('REJECT|')) {
              finish(`ESP32 menolak command: ${message}`);
            } else {
              finish(`Event ESP32: ${message}`);
            }
          }
        }
        if (rxTopic === statusTopic) {
          console.log('SmartCharge status:', message);
        }
      });

      client.subscribe([eventTopic, statusTopic], { qos: 0 }, (subErr) => {
        if (subErr) {
          finish(`Gagal subscribe balasan ESP32: ${subErr.message}`, true);
          return;
        }

        setStatus(`MQTT tersambung via ${url}. Mengirim: ${payload}`);

        // QoS 1 penting: callback dipanggil setelah broker memberi PUBACK, bukan cuma setelah data ditulis ke socket.
        client.publish(topic, payload, { qos: 1, retain: false }, (pubErr) => {
          if (pubErr) {
            finish(`Gagal kirim MQTT: ${pubErr.message}`, true);
            return;
          }

          setStatus(`Broker sudah menerima: ${payload}. Menunggu balasan ESP32...`);
          replyTimer = window.setTimeout(() => {
            finish(`Broker menerima command, tapi ESP32 belum membalas di ${eventTopic}. Cek Serial Monitor: apakah ada [MQTT RX]?`, false);
          }, 6000);
        });
      });
    } catch (err) {
      setBusy(false);
      setStatus(`Gagal: ${err.message}`);
    }
  }

  async function sendStart(pkg) {
    if (!ticket) {
      setStatus('Ticket kosong. Buka halaman ini dari QR printer, bukan diketik manual.');
      return;
    }
    const payload = `START|${ticket}|${port}|${pkg.minutes}|${pkg.price}`;
    await publishAndWait(payload, ticket);
  }

  async function sendRelayTest() {
    const payload = `TEST|${port}|10`;
    await publishAndWait(payload, '');
  }

  return (
    <main className="wrap">
      <section className="hero">
        <h1>SmartCharge</h1>
        <p>Demo pembayaran QR cloud.</p>
        <p className="small">Broker: test.mosquitto.org via MQTT WebSocket TLS.</p>
      </section>

      <section className="card" style={{ marginTop: 16 }}>
        <span className="badge">Ticket</span>
        <p><strong>Device:</strong> <code>{device}</code></p>
        <p><strong>Ticket:</strong> <code>{ticket || 'KOSONG'}</code></p>
        <p className="small muted">Topic command: <code>{topic}</code></p>
        <p className="small muted">Topic event: <code>{eventTopic}</code></p>
        <p className="small muted">Broker aktif: <code>{lastBroker}</code></p>
      </section>

      <section className="grid">
        <div className="card">
          <h2>Pilih port</h2>
          <select className="select" value={port} onChange={(e) => setPort(e.target.value)}>
            {Array.from({ length: 8 }, (_, i) => String(i + 1)).map((p) => (
              <option key={p} value={p}>Port {p}</option>
            ))}
          </select>
          <p className="small muted">Pastikan port belum dipakai di alat.</p>
          <button className="btn secondary" disabled={busy} onClick={sendRelayTest}>
            {busy ? 'Mengirim...' : 'Test Relay 10 detik'}
          </button>
          <p className="small muted">Tombol test butuh firmware debug yang mendukung payload <code>TEST|port|detik</code>.</p>
        </div>

        {PACKAGES.map((pkg) => (
          <div className="card" key={pkg.minutes}>
            <div className="big">{pkg.label}</div>
            <div className="price">{rupiah(pkg.price)}</div>
            <button className="btn" disabled={!canSend} onClick={() => sendStart(pkg)}>
              {busy ? 'Mengirim...' : 'Start Charge'}
            </button>
          </div>
        ))}
      </section>

      <section className="card" style={{ marginTop: 16 }}>
        <span className="badge">Log</span>
        <p>{status}</p>
        <p className="small muted">
          Versi debug ini menunggu event balik dari ESP32 supaya tidak cuma bilang “terkirim”, tapi juga tahu diterima atau ditolak.
        </p>
      </section>

      <div className="footer">SmartCharge Cloud Demo</div>
    </main>
  );
}
