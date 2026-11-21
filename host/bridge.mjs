// Connects the PC build of the firmware (station_host) to a real MQTT broker, doing the job Wi-Fi +
// PubSubClient do on the board, and injects the faults the firmware is built to survive:
//   --cut 30:15     at t = 30 s drop the TCP link abruptly for 15 s (no DISCONNECT: the broker fires the will)
//   --loss 0.05     5 % of telemetry messages vanish after leaving the device (what QoS 0 allows)
//
//   node host/bridge.mjs --url mqtt://localhost:1883 --id meteo-01 --period 500 --duration 90 --cut 30:15 --loss 0.05
import { spawn } from 'node:child_process';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import readline from 'node:readline';
import mqtt from 'mqtt';

const here = path.dirname(fileURLToPath(import.meta.url));
export const HOST_EXE = path.join(here, '..', 'build', process.platform === 'win32' ? 'station_host.exe' : 'station_host');

function lcg(seed) {
  let s = seed >>> 0;
  return () => ((s = (Math.imul(s, 1664525) + 1013904223) >>> 0) / 4294967296);
}

export async function runStation(o) {
  const opts = { url: 'mqtt://localhost:1883', id: 'meteo-01', period: 500, duration: 60, cuts: [], loss: 0, seed: 7, log: true, ...o };
  const boot = Date.now();
  const rnd = lcg(opts.seed);
  const log = (...a) => opts.log && console.log(`[${((Date.now() - boot) / 1000).toFixed(1).padStart(5)} s]`, ...a);
  const host = spawn(HOST_EXE, [opts.id, String(boot)], { stdio: ['pipe', 'pipe', 'inherit'] });
  const lines = readline.createInterface({ input: host.stdout });
  const counters = { published: 0, droppedByNetwork: 0, dropped: [], resends: 0, connects: 0, cuts: 0 };

  let client = null;
  let blockedUntil = 0;

  // The host answers every command, in order, with its PUB lines and then OK: a FIFO of resolvers is enough.
  const waiting = [];
  let current = {};
  lines.on('line', (l) => {
    if (l === 'OK') { waiting.shift()?.(current); current = {}; return; }
    if (l.startsWith('STATS ')) { current.stats = JSON.parse(l.slice(6)); return; }
    if (!l.startsWith('PUB ')) return;
    const [, retain, topic] = l.split(' ', 3);
    const payload = l.slice(4 + retain.length + 1 + topic.length + 1);
    if (topic.endsWith('/telemetry') && rnd() < opts.loss) {
      counters.droppedByNetwork++;
      counters.dropped.push(JSON.parse(payload).seq);
      return;
    }
    client?.publish(topic, payload, { qos: 0, retain: retain === '1' });
    counters.published++;
  });
  const cmd = (c) => new Promise((resolve) => { waiting.push(resolve); host.stdin.write(`${c}\n`); });

  function connect() {
    const status = `plant/${opts.id}/status`;
    client = mqtt.connect(opts.url, {
      clientId: `${opts.id}-host`, keepalive: 15, reconnectPeriod: 0, clean: true,
      will: { topic: status, payload: 'offline', qos: 1, retain: true },
    });
    client.on('connect', async () => {
      counters.connects++;
      await client.subscribeAsync(`plant/${opts.id}/resend`, { qos: 1 });
      log('MQTT up');
      cmd('UP');
    });
    client.on('message', (topic, payload) => {
      counters.resends++;
      cmd(`MSG ${topic} ${payload.toString()}`);
    });
    client.on('close', () => {
      cmd('DOWN');
      const c = client;
      c.removeAllListeners();
      if (client === c) client = null;
      if (!stopping) setTimeout(retry, 1000);
    });
    client.on('error', () => {});
  }
  function retry() {
    if (stopping || client) return;
    if (Date.now() < blockedUntil) { setTimeout(retry, 250); return; }
    connect();
  }

  let stopping = false;
  connect();
  const sampleTimer = setInterval(() => cmd(`SAMPLE ${Date.now()}`), opts.period);
  const loopTimer = setInterval(() => cmd('LOOP'), 100);
  const cutTimers = opts.cuts.map(([at, len]) => setTimeout(() => {
    counters.cuts++;
    blockedUntil = Date.now() + len * 1000;
    log(`link cut for ${len} s (abrupt, no DISCONNECT)`);
    client?.stream.destroy();
  }, at * 1000));

  await new Promise((r) => setTimeout(r, opts.duration * 1000));
  clearInterval(sampleTimer);
  // Keep the link and the loop running a little so the server's resend requests (every 2 s) are served.
  await new Promise((r) => setTimeout(r, (opts.drainSeconds ?? 8) * 1000));
  clearInterval(loopTimer);
  cutTimers.forEach(clearTimeout);
  const final = (await cmd('STATS')).stats;
  stopping = true;
  await client?.endAsync();
  host.stdin.end();
  return { boot, ...counters, station: final };
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  const arg = (k, d) => { const i = process.argv.indexOf(`--${k}`); return i > 0 ? process.argv[i + 1] : d; };
  const cuts = (arg('cut', '') || '').split(',').filter(Boolean).map((c) => c.split(':').map(Number));
  const r = await runStation({ url: arg('url'), id: arg('id'), period: Number(arg('period', 500)), duration: Number(arg('duration', 60)),
    cuts, loss: Number(arg('loss', 0)), seed: Number(arg('seed', 7)) });
  console.log(JSON.stringify({ ...r, dropped: r.dropped.length }, null, 2));
}
