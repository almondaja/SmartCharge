# SmartCharge Web Vercel Demo

Project minimal untuk memperbaiki 404 Vercel dan menyediakan route QR:

- `/` untuk halaman status
- `/pay?device=SC001&ticket=SC001-XXXXXXXX` untuk halaman pilih port/paket

Demo ini memakai broker publik `test.mosquitto.org`:

- ESP32: `test.mosquitto.org:1883`
- Browser/Vercel page: `wss://test.mosquitto.org:8081/mqtt`

Jangan pakai broker publik ini untuk transaksi asli.

## Jalankan lokal

```bash
npm install
npm run dev
```

Buka:

```text
http://localhost:3000/pay?device=SC001&ticket=SC001-DEMO1234
```

## Deploy

```bash
git add .
git commit -m "Add SmartCharge web"
git push
```

Vercel akan deploy otomatis kalau repo sudah tersambung.
