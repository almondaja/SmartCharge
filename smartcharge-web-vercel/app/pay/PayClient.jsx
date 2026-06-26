'use client';

import { useMemo, useState } from 'react';
import { useSearchParams } from 'next/navigation';

const MQTT_WS_URL = 'wss://test.mosquitto.org:8081/mqtt';
const MQTT_SCRIPT_URL = 'https://unpkg.com/mqtt/dist/mqtt.min.js';

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

export default function PayClient() {
  const searchParams = useSearchParams();
  const device = cleanParam(searchParams.get('device'), 'SC001');
  const ticket = cleanParam(searchParams.get('ticket'));
  const [port, setPort] = useState('1');
  const [status, setStatus] = useState('Siap. Pilih port dan paket.');
  const [busy, setBusy] = useState(false);

  const topic = useMemo(() => `smartcharge/${device}/cmd`, [device]);
  const canSend = Boolean(ticket) && !busy;

  async function sendStart(pkg) {
    if (!ticket) {
      setStatus('Ticket kosong. Buka halaman ini dari QR printer, bukan diketik manual.');
      return;
    }

    setBusy(true);
    setStatus('Menghubungkan ke MQTT WebSocket...');

    try {
      const mqtt = await loadMqttJs();
      const clientId = `web-${device}-${Math.random().toString(16).slice(2)}`;
      const client = mqtt.connect(MQTT_WS_URL, {
        clientId,
        clean: true,
        reconnectPeriod: 0,
        connectTimeout: 10000
      });

      let finished = false;
      const finish = (message) => {
        if (finished) return;
        finished = true;
        setStatus(message);
        setBusy(false);
        try { client.end(true); } catch (_) {}
      };

      const timer = window.setTimeout(() => {
        finish('Timeout konek MQTT. test.mosquitto.org kadang sibuk, coba lagi.');
      }, 12000);

      client.on('connect', () => {
        window.clearTimeout(timer);
        const payload = `START|${ticket}|${port}|${pkg.minutes}|${pkg.price}`;

        client.publish(topic, payload, { qos: 0, retain: false }, (err) => {
          if (err) finish(`Gagal kirim MQTT: ${err.message}`);
          else finish(`Terkirim ke ${topic}: ${payload}`);
        });
      });

      client.on('error', (err) => {
        window.clearTimeout(timer);
        finish(`MQTT error: ${err.message}`);
      });
    } catch (err) {
      setBusy(false);
      setStatus(`Gagal: ${err.message}`);
    }
  }

  return (
    <main className="wrap">
      <section className="hero">
        <h1>SmartCharge</h1>
        <p>Demo pembayaran QR cloud.</p>
        <p className="small">Broker: test.mosquitto.org via WebSocket TLS.</p>
      </section>

      <section className="card" style={{ marginTop: 16 }}>
        <span className="badge">Ticket</span>
        <p><strong>Device:</strong> <code>{device}</code></p>
        <p><strong>Ticket:</strong> <code>{ticket || 'KOSONG'}</code></p>
        <p className="small muted">Topic command: <code>{topic}</code></p>
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
          Ini masih demo. Tombol langsung mengirim command MQTT, belum ada payment gateway asli.
        </p>
      </section>

      <div className="footer">SmartCharge Cloud Demo</div>
    </main>
  );
}
