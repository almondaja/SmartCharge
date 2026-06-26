export default function HomePage() {
  return (
    <main className="wrap">
      <section className="hero">
        <h1>SmartCharge</h1>
        <p>Web cloud sudah online.</p>
        <p className="small">Scan QR dari printer untuk membuka halaman pembayaran tiket.</p>
      </section>

      <section className="card" style={{ marginTop: 16 }}>
        <span className="badge">Status</span>
        <h2>Domain Vercel aktif</h2>
        <p className="muted">
          QR ESP32 akan membuka route <code>/pay?device=SC001&amp;ticket=...</code>.
        </p>
        <p className="small muted">
          Halaman demo ini memakai broker publik test.mosquitto.org. Jangan dipakai untuk transaksi asli.
        </p>
      </section>
    </main>
  );
}
