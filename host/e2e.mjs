// End to end: the firmware logic (PC build) -> real MQTT broker -> MQTT IoT Panel ingest -> PostgreSQL.
// Runs the station with link cuts and message loss, then checks in the database that every reading of
// this power-up arrived exactly once, with no gaps.
//
// Needs the panel running (broker on MQTT_URL, ingest writing to DATABASE_URL). See the README.
//   node host/e2e.mjs [--duration 90] [--loss 0.05] [--cut 25:12,60:6]
import pg from 'pg';
import { runStation } from './bridge.mjs';

const arg = (k, d) => { const i = process.argv.indexOf(`--${k}`); return i > 0 ? process.argv[i + 1] : d; };
const url = process.env.MQTT_URL ?? 'mqtt://localhost:1883';
const db = process.env.DATABASE_URL ?? 'postgresql://postgres@localhost:5432/station_demo';
const id = arg('id', 'meteo-01');
const duration = Number(arg('duration', 90));
const loss = Number(arg('loss', 0.05));
const cuts = arg('cut', '25:12,60:6').split(',').filter(Boolean).map((c) => c.split(':').map(Number));

console.log(`station ${id} -> ${url}, ${duration} s, ${Math.round(loss * 100)} % loss, cuts ${JSON.stringify(cuts)}`);
const r = await runStation({ url, id, period: 500, duration, cuts, loss, drainSeconds: 10 });
await new Promise((res) => setTimeout(res, 1500));   // last ingest batch

const pool = new pg.Pool({ connectionString: db });
const { rows } = await pool.query('SELECT seq FROM readings WHERE device = $1 AND boot = $2 ORDER BY seq', [id, r.boot]);
await pool.end();
const seqs = rows.map((x) => Number(x.seq));
const sampled = r.station.seq;
const missing = [];
for (let s = 1, i = 0; s <= sampled; s++) { if (seqs[i] === s) i++; else missing.push(s); }
const dupes = seqs.length - new Set(seqs).size;

const report = {
  boot: r.boot,
  readings_sampled_on_device: sampled,
  stored_in_postgres: seqs.length,
  missing,
  duplicates: dupes,
  link_cuts: r.cuts,
  mqtt_connects: r.connects,
  sent_late_after_reconnect: r.station.late,
  lost_by_network: r.droppedByNetwork,
  resend_requests_received: r.resends,
  resent_by_device: r.station.resent,
  lost_overwritten: r.station.lost,
};
console.log(JSON.stringify(report, null, 2));
const ok = missing.length === 0 && dupes === 0 && seqs.length === sampled;
console.log(ok ? 'PASS: every reading stored exactly once' : 'FAIL');
process.exit(ok ? 0 : 1);
