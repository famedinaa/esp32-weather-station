// Starts the MQTT IoT Panel (broker :1883, ingest + panel :8080, its simulated plant) on its own database,
// so the station's test runs don't mix with the panel demo's data.
//   PANEL_DIR=... node host/run-panel.mjs
import { spawn } from 'node:child_process';

const dir = process.env.PANEL_DIR ?? 'D:/Informática (trabajo, hobbie, etc)/Proyectos/Upwork/Electrónica y embebidos/MQTT IoT Panel';
const env = { ...process.env, DATABASE_URL: process.env.DATABASE_URL ?? 'postgresql://postgres@localhost:5432/station_demo' };
const child = spawn('pnpm', ['dev'], { cwd: dir, env, stdio: 'inherit', shell: true });
child.on('exit', (code) => process.exit(code ?? 0));
for (const s of ['SIGINT', 'SIGTERM']) process.on(s, () => child.kill(s));
