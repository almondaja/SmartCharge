import './globals.css';

export const metadata = {
  title: 'SmartCharge',
  description: 'SmartCharge QR payment demo'
};

export default function RootLayout({ children }) {
  return (
    <html lang="id">
      <body>{children}</body>
    </html>
  );
}
