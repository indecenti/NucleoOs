// Dev-only reverse proxy: forwards localhost:5602 -> the REAL Cardputer at 192.168.0.166:80,
// so the preview browser (pinned to a launch.json server) actually shows the DEVICE's web app.
// Relative /api/* calls from the served app hit this proxy and are forwarded to the device.
// Handles WebSocket (/ws) upgrades too, so the shell's event bus works. Not a build artifact.
import http from 'node:http';
import net from 'node:net';

const TARGET_HOST = process.env.NUCLEO_IP || '192.168.0.166';
const TARGET_PORT = 80;
const LISTEN = Number(process.env.PROXY_PORT || 5602);

const server = http.createServer((creq, cres) => {
  const headers = { ...creq.headers, host: `${TARGET_HOST}` };
  const preq = http.request(
    { host: TARGET_HOST, port: TARGET_PORT, path: creq.url, method: creq.method, headers, timeout: 20000 },
    (pres) => { cres.writeHead(pres.statusCode || 502, pres.headers); pres.pipe(cres); }
  );
  preq.on('error', (e) => { if (!cres.headersSent) cres.writeHead(502, { 'content-type': 'text/plain' }); cres.end('proxy error: ' + e.message); });
  preq.on('timeout', () => preq.destroy(new Error('upstream timeout')));
  creq.pipe(preq);
});

// WebSocket / Upgrade passthrough (raw TCP splice) so /ws works.
server.on('upgrade', (creq, csock, head) => {
  const psock = net.connect(TARGET_PORT, TARGET_HOST, () => {
    const reqLine = `${creq.method} ${creq.url} HTTP/1.1\r\n`;
    const hdrs = Object.entries({ ...creq.headers, host: TARGET_HOST }).map(([k, v]) => `${k}: ${v}`).join('\r\n');
    psock.write(reqLine + hdrs + '\r\n\r\n');
    if (head && head.length) psock.write(head);
    psock.pipe(csock);
    csock.pipe(psock);
  });
  psock.on('error', () => csock.destroy());
  csock.on('error', () => psock.destroy());
});

server.listen(LISTEN, () => console.log(`device-proxy on http://localhost:${LISTEN} -> http://${TARGET_HOST}:${TARGET_PORT}`));
