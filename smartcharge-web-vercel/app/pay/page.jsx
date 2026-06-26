import { Suspense } from 'react';
import PayClient from './PayClient';

export default function PayPage() {
  return (
    <Suspense fallback={<main className="wrap"><section className="card">Memuat tiket...</section></main>}>
      <PayClient />
    </Suspense>
  );
}
