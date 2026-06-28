// Coherent launcher icon set — bold filled silhouettes on a square grid, one ink colour + the badge
// colour behind (`bg`) for clean cut-outs. Every shape maps 1:1 to an M5GFX primitive, so the
// firmware ui_icon() is a mechanical port of this file. Drawn centred at (cx,cy) inside a 2*s box.
//
// Style rules (keep them to stay coherent):
//   · bold filled forms (bx/fc/tri/frr), NOT thin outlines; internal detail = a `bg` cut-out.
//   · features snap to ~0.2*s minimum thickness so nothing reads as a hairline on the panel.
//   · one visual weight across the set; axis-aligned where possible (clean when pixelated).

// Adapter binding a 2D context to M5GFX-shaped primitives. col = ink (default), bg = badge colour.
export function iconGfx(ctx, col, bg) {
  const round = (x, y, w, h, r, c) => {
    r = Math.min(r, w / 2, h / 2);
    ctx.beginPath();
    ctx.moveTo(x + r, y);
    ctx.arcTo(x + w, y, x + w, y + h, r); ctx.arcTo(x + w, y + h, x, y + h, r);
    ctx.arcTo(x, y + h, x, y, r);         ctx.arcTo(x, y, x + w, y, r);
    ctx.closePath(); ctx.fillStyle = c; ctx.fill();
  };
  return {
    col, bg,
    bx:  (x, y, w, h, c = col) => { ctx.fillStyle = c; ctx.fillRect(x, y, w, h); },
    fc:  (x, y, r, c = col) => { ctx.fillStyle = c; ctx.beginPath(); ctx.arc(x, y, r, 0, 7); ctx.fill(); },
    tri: (x0, y0, x1, y1, x2, y2, c = col) => { ctx.fillStyle = c; ctx.beginPath(); ctx.moveTo(x0, y0); ctx.lineTo(x1, y1); ctx.lineTo(x2, y2); ctx.closePath(); ctx.fill(); },
    frr: (x, y, w, h, r, c = col) => round(x, y, w, h, r, c),
    ln:  (x0, y0, x1, y1, w = 2, c = col) => { ctx.strokeStyle = c; ctx.lineWidth = w; ctx.lineCap = 'butt'; ctx.beginPath(); ctx.moveTo(x0, y0); ctx.lineTo(x1, y1); ctx.stroke(); },
    el:  (x, y, rx, ry, w = 2, c = col) => { ctx.strokeStyle = c; ctx.lineWidth = w; ctx.beginPath(); ctx.ellipse(x, y, rx, ry, 0, 0, 7); ctx.stroke(); },
  };
}

// Draw the icon `id` centred at (cx,cy), half-extent `s`. Returns true if a vector icon was drawn,
// false for an unknown id (the caller then draws a bold initial as the fallback).
export function drawIcon(g, id, cx, cy, s) {
  const T = Math.max(2, s * 0.22);                 // standard limb thickness (bold, never a hairline)
  switch (id) {

    case 'level':                                  // spirit level: pill vial, bubble, two gauge ticks
      g.frr(cx - s, cy - s * 0.42, 2 * s, s * 0.84, s * 0.42);
      g.frr(cx - s + T * 0.6, cy - s * 0.42 + T * 0.6, 2 * s - T * 1.2, s * 0.84 - T * 1.2, s * 0.36, g.bg);
      g.bx(cx - s * 0.34, cy - s * 0.42, T * 0.5, s * 0.84);
      g.bx(cx + s * 0.34 - T * 0.5, cy - s * 0.42, T * 0.5, s * 0.84);
      g.fc(cx, cy, T * 1.15);
      break;

    case 'Hardware':                               // DIP microchip: body + side legs + pin-1 dot
      g.frr(cx - s * 0.56, cy - s * 0.72, s * 1.12, s * 1.44, s * 0.12);
      g.frr(cx - s * 0.28, cy - s * 0.44, s * 0.56, s * 0.9, 2, g.bg);
      for (let i = -1; i <= 1; i++) {
        g.bx(cx - s, cy + i * s * 0.42 - T * 0.28, s * 0.44, T * 0.56);
        g.bx(cx + s * 0.56, cy + i * s * 0.42 - T * 0.28, s * 0.44, T * 0.56);
      }
      g.fc(cx - s * 0.28, cy - s * 0.46, T * 0.5);
      break;

    case 'dice':                                   // die face: rounded square + 5 pips
      g.frr(cx - s, cy - s, 2 * s, 2 * s, s * 0.28);
      g.fc(cx - s * 0.45, cy - s * 0.45, T * 0.6, g.bg);
      g.fc(cx + s * 0.45, cy - s * 0.45, T * 0.6, g.bg);
      g.fc(cx, cy, T * 0.6, g.bg);
      g.fc(cx - s * 0.45, cy + s * 0.45, T * 0.6, g.bg);
      g.fc(cx + s * 0.45, cy + s * 0.45, T * 0.6, g.bg);
      break;

    case 'goniometer':                             // protractor: top half-disc + pivot + angle arm
      g.fc(cx, cy + s * 0.45, s);
      g.bx(cx - s - 1, cy + s * 0.45, 2 * s + 2, s + 2, g.bg);
      g.bx(cx - s * 0.5, cy + s * 0.05, s, s * 0.42, g.bg);
      g.ln(cx, cy + s * 0.45, cx + s * 0.66, cy - s * 0.55, T * 0.55);
      g.fc(cx, cy + s * 0.45, T * 0.7);
      break;

    case 'pedometer':                              // footprint: ball + heel + a row of toes
      g.fc(cx, cy - s * 0.05, s * 0.52);
      g.fc(cx + s * 0.12, cy + s * 0.66, s * 0.32);
      g.fc(cx - s * 0.42, cy - s * 0.55, T * 0.34);
      g.fc(cx - s * 0.15, cy - s * 0.72, T * 0.38);
      g.fc(cx + s * 0.13, cy - s * 0.74, T * 0.36);
      g.fc(cx + s * 0.37, cy - s * 0.62, T * 0.32);
      break;

    case 'alarm':                                  // alarm bell: dome body + base rim + clapper
      g.fc(cx, cy - s * 0.6, T * 0.4);
      g.tri(cx, cy - s * 0.5, cx - s * 0.6, cy + s * 0.4, cx + s * 0.6, cy + s * 0.4);
      g.fc(cx, cy - s * 0.42, s * 0.3);
      g.frr(cx - s * 0.66, cy + s * 0.36, s * 1.32, T * 0.5, T * 0.2);
      g.fc(cx, cy + s * 0.66, T * 0.42);
      break;

    case 'clock':
      g.fc(cx, cy, s); g.fc(cx, cy, s - T, g.bg);
      g.bx(cx - T / 2, cy - s * 0.6, T, s * 0.6); g.bx(cx, cy - T / 2, s * 0.5, T); g.fc(cx, cy, T * 0.7);
      break;

    case 'anima': {
      const a = s, b = s * 0.34;
      g.tri(cx, cy - a, cx - b, cy, cx + b, cy); g.tri(cx, cy + a, cx - b, cy, cx + b, cy);
      g.tri(cx - a, cy, cx, cy - b, cx, cy + b);  g.tri(cx + a, cy, cx, cy - b, cx, cy + b);
      const gx = cx + s * 0.66, gy = cy - s * 0.66, c = s * 0.3;
      g.tri(gx, gy - c, gx - c * 0.4, gy, gx + c * 0.4, gy); g.tri(gx, gy + c, gx - c * 0.4, gy, gx + c * 0.4, gy);
      break;
    }

    case 'calc':
      g.frr(cx - s, cy - s, 2 * s, 2 * s, s * 0.28);
      g.frr(cx - s * 0.66, cy - s * 0.7, s * 1.32, s * 0.5, 2, g.bg);
      for (let r = 0; r < 2; r++) for (let c = 0; c < 3; c++)
        g.fc(cx - s * 0.5 + c * s * 0.5, cy + s * 0.08 + r * s * 0.5, T * 0.45, g.bg);
      break;

    case 'files':
      g.frr(cx - s, cy - s * 0.6, s * 0.95, s * 0.4, T * 0.6);
      g.frr(cx - s, cy - s * 0.3, 2 * s, s * 1.5, s * 0.22);
      g.bx(cx - s * 0.7, cy - s * 0.1, s * 1.4, T * 0.55, g.bg);
      break;

    case 'calendar':
      g.frr(cx - s, cy - s * 0.78, 2 * s, s * 1.66, s * 0.22);
      g.bx(cx - s, cy - s * 0.78, 2 * s, s * 0.5, g.col);
      g.frr(cx - s + T, cy - s * 0.28 + T, 2 * s - 2 * T, s * 1.16 - 2 * T, 2, g.bg);
      g.bx(cx - s * 0.55, cy - s, T, s * 0.34); g.bx(cx + s * 0.55 - T, cy - s, T, s * 0.34);
      g.fc(cx, cy + s * 0.34, T * 0.7, g.col);
      break;

    case 'notepad':
      g.frr(cx - s * 0.8, cy - s, s * 1.6, 2 * s, s * 0.2);
      for (let i = 0; i < 3; i++) g.bx(cx - s * 0.5, cy - s * 0.45 + i * s * 0.5, s * (1 - i * 0.18), T * 0.55, g.bg);
      break;

    case 'usb':
      g.bx(cx - T, cy - s, 2 * T, s * 0.4);
      g.frr(cx - s * 0.5, cy - s * 0.62, s, s * 1.62, s * 0.2);
      g.bx(cx - s * 0.5, cy + s * 0.12, s, T * 0.55, g.bg);
      break;

    case 'usbkbd':
      g.frr(cx - s, cy - s * 0.6, 2 * s, s * 1.2, s * 0.22);
      for (let r = 0; r < 2; r++) for (let c = 0; c < 4; c++)
        g.bx(cx - s * 0.72 + c * s * 0.46, cy - s * 0.32 + r * s * 0.42, T * 0.6, T * 0.6, g.bg);
      g.bx(cx - s * 0.4, cy + s * 0.34, s * 0.8, T * 0.55, g.bg);
      break;

    case 'music':
      g.fc(cx - s * 0.45, cy + s * 0.55, T * 1.05);
      g.bx(cx - s * 0.45 + T * 0.6, cy - s * 0.82, T * 0.8, s * 1.45);
      g.tri(cx - s * 0.45 + T * 1.4, cy - s * 0.82, cx - s * 0.45 + T * 1.4, cy - s * 0.12, cx + s * 0.72, cy - s * 0.45);
      break;

    case 'video':
      g.frr(cx - s, cy - s * 0.75, 2 * s, s * 1.5, s * 0.22);
      g.tri(cx - s * 0.3, cy - s * 0.42, cx - s * 0.3, cy + s * 0.42, cx + s * 0.5, cy, g.bg);
      break;
    case 'Media':
      g.tri(cx - s * 0.7, cy - s, cx - s * 0.7, cy + s, cx + s, cy);
      break;

    case 'photos':
      g.frr(cx - s, cy - s, 2 * s, 2 * s, s * 0.22);
      g.frr(cx - s + T, cy - s + T, 2 * s - 2 * T, 2 * s - 2 * T, 2, g.bg);
      g.fc(cx - s * 0.4, cy - s * 0.4, T * 0.8, g.col);
      g.tri(cx - s * 0.85, cy + s * 0.62, cx - s * 0.1, cy - s * 0.15, cx + s * 0.25, cy + s * 0.62, g.col);
      g.tri(cx - s * 0.05, cy + s * 0.62, cx + s * 0.45, cy + s * 0.05, cx + s * 0.85, cy + s * 0.62, g.col);
      break;

    case 'recorder': case 'voice': case 'Voice':
      g.frr(cx - T * 1.15, cy - s, T * 2.3, s * 1.25, T * 1.15);
      g.ln(cx - s * 0.58, cy, cx - s * 0.58, cy + s * 0.22, T * 0.6); g.ln(cx + s * 0.58, cy, cx + s * 0.58, cy + s * 0.22, T * 0.6);
      g.ln(cx - s * 0.58, cy + s * 0.2, cx, cy + s * 0.5, T * 0.6); g.ln(cx + s * 0.58, cy + s * 0.2, cx, cy + s * 0.5, T * 0.6);
      g.bx(cx - T / 2, cy + s * 0.45, T, s * 0.35); g.bx(cx - s * 0.45, cy + s * 0.78, s * 0.9, T * 0.6);
      break;

    case 'micspec': {
      const h = [0.55, 1.0, 0.4, 0.85, 0.6];
      for (let i = 0; i < 5; i++) g.bx(cx - s + i * (2 * s / 5) + T * 0.2, cy + s - 2 * s * h[i], T, 2 * s * h[i]);
      break;
    }

    case 'voicelab':
      g.frr(cx - s, cy - s * 0.8, 2 * s, s * 1.25, s * 0.3);
      g.tri(cx - s * 0.5, cy + s * 0.35, cx - s * 0.5, cy + s, cx, cy + s * 0.4);
      for (let i = -1; i <= 1; i++) g.fc(cx + i * s * 0.5, cy - s * 0.18, T * 0.5, g.bg);
      break;

    case 'info': case 'Connect':
      g.fc(cx, cy + s * 0.7, T * 0.85);
      g.tri(cx - s * 0.55, cy + s * 0.15, cx + s * 0.55, cy + s * 0.15, cx, cy + s * 0.55);
      g.tri(cx - s * 0.28, cy + s * 0.33, cx + s * 0.28, cy + s * 0.33, cx, cy + s * 0.55, g.bg);
      g.tri(cx - s, cy - s * 0.42, cx + s, cy - s * 0.42, cx, cy + s * 0.1);
      g.tri(cx - s * 0.62, cy - s * 0.18, cx + s * 0.62, cy - s * 0.18, cx, cy + s * 0.1, g.bg);
      break;

    case 'sysmon': {
      const h = [0.5, 1.0, 0.7];
      for (let i = 0; i < 3; i++) g.bx(cx - s * 0.8 + i * s * 0.72, cy + s * 0.7 - 1.4 * s * h[i], T, 1.4 * s * h[i]);
      g.bx(cx - s, cy + s * 0.7, 2 * s, T * 0.55);
      break;
    }

    case 'System':
      g.frr(cx - s * 0.7, cy - s * 0.7, s * 1.4, s * 1.4, s * 0.16);
      g.frr(cx - s * 0.3, cy - s * 0.3, s * 0.6, s * 0.6, 2, g.bg);
      for (let i = -1; i <= 1; i++) {
        g.bx(cx + i * s * 0.42 - T * 0.3, cy - s, T * 0.6, s * 0.32); g.bx(cx + i * s * 0.42 - T * 0.3, cy + s * 0.68, T * 0.6, s * 0.32);
        g.bx(cx - s, cy + i * s * 0.42 - T * 0.3, s * 0.32, T * 0.6); g.bx(cx + s * 0.68, cy + i * s * 0.42 - T * 0.3, s * 0.32, T * 0.6);
      }
      break;

    case 'radio':
      g.frr(cx - s, cy - s * 0.3, 2 * s, s * 1.25, s * 0.18);
      g.ln(cx + s * 0.4, cy - s * 0.3, cx + s * 0.85, cy - s, T * 0.6); g.fc(cx + s * 0.85, cy - s, T * 0.6);
      g.fc(cx - s * 0.45, cy + s * 0.32, s * 0.38, g.bg); g.fc(cx + s * 0.5, cy + s * 0.32, T * 0.8, g.bg);
      break;

    case 'remote':
      g.frr(cx - s, cy - s * 0.8, 2 * s, s * 1.3, s * 0.18);
      g.frr(cx - s + T, cy - s * 0.8 + T, 2 * s - 2 * T, s * 1.3 - 2 * T, 2, g.bg);
      g.fc(cx - s * 0.55, cy + s * 0.82, T * 0.7); g.tri(cx - s * 0.78, cy + s * 0.55, cx - s * 0.32, cy + s * 0.55, cx - s * 0.55, cy + s * 0.85);
      break;

    case 'ir':
      g.frr(cx - s * 0.5, cy - s, s, 2 * s, s * 0.3);
      g.fc(cx - s * 0.02, cy - s * 0.62, T * 0.45, g.bg); g.bx(cx - s * 0.22, cy - s * 0.12, s * 0.4, T * 0.45, g.bg); g.bx(cx - s * 0.22, cy + s * 0.28, s * 0.4, T * 0.45, g.bg);
      g.ln(cx + s * 0.6, cy - s * 0.7, cx + s, cy - s, T * 0.6); g.ln(cx + s * 0.6, cy - s * 0.35, cx + s, cy - s * 0.55, T * 0.6);
      break;

    case 'qr': {
      const e = s * 0.6;
      for (const [ox, oy] of [[-1, -1], [1, -1], [-1, 1]]) {
        g.bx(cx + ox * e - e * 0.5, cy + oy * e - e * 0.5, e, e);
        g.bx(cx + ox * e - e * 0.26, cy + oy * e - e * 0.26, e * 0.52, e * 0.52, g.bg);
        g.fc(cx + ox * e, cy + oy * e, e * 0.16, g.col);
      }
      for (const [dx, dy] of [[0.32, 0.32], [0.72, 0.42], [0.42, 0.72], [0.74, 0.74]]) g.bx(cx + dx * s, cy + dy * s, T * 0.55, T * 0.55);
      break;
    }

    case 'notify':
      g.frr(cx - s * 0.72, cy - s * 0.7, s * 1.44, s * 1.2, s * 0.7);
      g.bx(cx - s * 0.82, cy + s * 0.34, s * 1.64, T * 0.65);
      g.bx(cx - T / 2, cy - s, T, T * 0.8); g.fc(cx, cy + s * 0.74, T * 0.6);
      break;

    case 'torch':
      g.frr(cx - s, cy - s * 0.45, s * 0.9, s * 0.9, s * 0.18);
      g.tri(cx - s * 0.12, cy - s * 0.62, cx + s * 0.38, cy - s, cx + s * 0.38, cy + s); g.tri(cx - s * 0.12, cy + s * 0.62, cx + s * 0.38, cy + s, cx + s * 0.38, cy - s);
      g.bx(cx + s * 0.34, cy - s * 0.55, T * 0.8, s * 1.1);
      g.ln(cx + s * 0.6, cy - s * 0.5, cx + s, cy - s * 0.8, T * 0.55); g.ln(cx + s * 0.6, cy, cx + s, cy, T * 0.55); g.ln(cx + s * 0.6, cy + s * 0.5, cx + s, cy + s * 0.8, T * 0.55);
      break;

    case 'theme':
      g.fc(cx, cy, s); g.bx(cx, cy - s, s + 1, 2 * s, g.bg); g.el(cx, cy, s, s, T * 0.5, g.col);
      break;

    case 'wifi':
      for (let i = 0; i < 3; i++) { const y = cy - s * 0.55 + i * s * 0.55; g.bx(cx - s, y - T * 0.28, 2 * s, T * 0.55); g.fc(cx - s * 0.5 + i * s * 0.5, y, T * 0.85); }
      break;

    case 'link': {
      const a = s * 0.78;
      g.ln(cx - a, cy, cx + a, cy - a, T * 0.6); g.ln(cx - a, cy, cx + a, cy + a, T * 0.6);
      g.fc(cx - a, cy, T * 1.05); g.fc(cx + a, cy - a, T * 1.05); g.fc(cx + a, cy + a, T * 1.05);
      break;
    }

    case 'swarm': {
      // a hub with peers all around it — a mesh/swarm of devices (kept in sync with launcher_render.cpp)
      const R = s * 0.84, o = [[1, 0], [0.5, 0.87], [-0.5, 0.87], [-1, 0], [-0.5, -0.87], [0.5, -0.87]];
      for (const [dx, dy] of o) { const px = cx + R * dx, py = cy + R * dy; g.ln(cx, cy, px, py, T * 0.5); g.fc(px, py, T * 0.66); }
      g.fc(cx, cy, T * 1.05);
      break;
    }

    case 'ssh':
      g.frr(cx - s, cy - s * 0.8, 2 * s, s * 1.6, s * 0.18);
      g.bx(cx - s, cy - s * 0.8, 2 * s, s * 0.42, g.col); g.frr(cx - s + T * 0.6, cy - s * 0.28, 2 * s - T * 1.2, s * 1 - T * 0.6, 2, g.bg);
      g.tri(cx - s * 0.5, cy - s * 0.05, cx - s * 0.5, cy + s * 0.32, cx - s * 0.08, cy + s * 0.14, g.col);
      g.bx(cx, cy + s * 0.24, s * 0.45, T * 0.55, g.col);
      break;

    case 'ethernet':
      g.frr(cx - s * 0.72, cy - s * 0.8, s * 1.44, s * 1.3, s * 0.16);
      for (let i = 0; i < 4; i++) g.bx(cx - s * 0.5 + i * s * 0.32, cy - s * 0.8, T * 0.45, s * 0.4, g.bg);
      g.bx(cx - T * 0.7, cy + s * 0.5, T * 1.4, s * 0.5);
      break;

    case 'beacon':
      g.tri(cx - s * 0.8, cy + s, cx + s * 0.8, cy + s, cx, cy - s * 0.15);  // wide signal tower
      g.frr(cx - s * 0.5, cy + s * 0.82, s, T * 0.7, 2, g.bg);               // base strut (cut-out)
      g.fc(cx, cy - s * 0.28, T * 0.8);                                      // emitter
      g.ln(cx + s * 0.34, cy - s * 0.55, cx + s * 0.62, cy - s * 0.78, T * 0.5); g.ln(cx + s * 0.34, cy - s * 0.18, cx + s * 0.66, cy - s * 0.32, T * 0.5);
      g.ln(cx - s * 0.34, cy - s * 0.55, cx - s * 0.62, cy - s * 0.78, T * 0.5); g.ln(cx - s * 0.34, cy - s * 0.18, cx - s * 0.66, cy - s * 0.32, T * 0.5);
      break;

    case 'wifiatk':
      g.fc(cx - s * 0.3, cy + s * 0.58, T * 0.75);
      g.tri(cx - s * 0.8, cy + s * 0.05, cx + s * 0.2, cy + s * 0.05, cx - s * 0.3, cy + s * 0.5); g.tri(cx - s * 0.55, cy + s * 0.22, cx - s * 0.05, cy + s * 0.22, cx - s * 0.3, cy + s * 0.5, g.bg);
      g.ln(cx + s * 0.1, cy - s, cx + s, cy - s * 0.2, T * 0.85); g.ln(cx + s, cy - s, cx + s * 0.1, cy - s * 0.2, T * 0.85);
      break;

    case 'evilportal':
      g.frr(cx - s, cy - s * 0.85, 2 * s, s * 1.7, s * 0.16);
      g.bx(cx - s, cy - s * 0.85, 2 * s, s * 0.4, g.col); g.frr(cx - s + T * 0.6, cy - s * 0.4 + T * 0.6, 2 * s - T * 1.2, s * 1.18, 2, g.bg);
      g.frr(cx - s * 0.4, cy + s * 0.02, s * 0.8, s * 0.56, 2, g.col); g.el(cx, cy - s * 0.05, s * 0.26, s * 0.26, T * 0.6, g.col);
      break;

    case 'Security':
      g.frr(cx - s, cy - s, 2 * s, s, s * 0.32);
      g.tri(cx - s, cy - s * 0.5, cx + s, cy - s * 0.5, cx, cy + s);
      break;

    case 'Games':
      g.frr(cx - s, cy - s * 0.5, 2 * s, s * 1.05, s * 0.5);
      g.bx(cx - s * 0.62, cy - T * 0.28, s * 0.5, T * 0.55, g.bg); g.bx(cx - s * 0.42, cy - s * 0.25, T * 0.55, s * 0.5, g.bg);
      g.fc(cx + s * 0.4, cy - s * 0.12, T * 0.55, g.bg); g.fc(cx + s * 0.66, cy + s * 0.1, T * 0.55, g.bg); g.fc(cx + s * 0.14, cy + s * 0.1, T * 0.55, g.bg);
      break;

    case 'reactor':
      g.el(cx, cy, s, s * 0.42, T * 0.5); g.el(cx, cy, s * 0.42, s, T * 0.5);
      g.fc(cx, cy, T * 1.05); g.fc(cx + s * 0.92, cy, T * 0.55); g.fc(cx, cy - s * 0.92, T * 0.55);
      break;

    case 'pong':                                     // two paddles + ball + a hint of the centre net
      g.frr(cx - s * 0.92, cy - s * 0.6, s * 0.26, s * 1.2, 2);
      g.frr(cx + s * 0.66, cy - s * 0.15, s * 0.26, s * 1.2, 2);
      g.fc(cx + s * 0.06, cy + s * 0.04, s * 0.22);
      g.fc(cx, cy - s * 0.62, T * 0.35); g.fc(cx, cy + s * 0.62, T * 0.35);
      break;

    case 'tanks':                                    // tank: hull + wheels + turret + raised barrel
      g.frr(cx - s * 0.9, cy + s * 0.12, s * 1.8, s * 0.5, 3);
      g.fc(cx - s * 0.55, cy + s * 0.66, T * 0.55); g.fc(cx, cy + s * 0.66, T * 0.55); g.fc(cx + s * 0.55, cy + s * 0.66, T * 0.55);
      g.frr(cx - s * 0.34, cy - s * 0.22, s * 0.68, s * 0.42, 2);
      g.ln(cx + s * 0.2, cy - s * 0.05, cx + s, cy - s * 0.62, T * 0.6);
      break;

    case 'stelle': {                                 // constellation: a path of stars, varied sizes
      const p = [[-0.78, -0.5], [-0.05, -0.15], [0.7, -0.6], [0.4, 0.6], [-0.55, 0.7]];
      const link = [[0, 1], [1, 2], [1, 3], [3, 4]];
      for (const [a, b] of link) g.ln(cx + p[a][0] * s, cy + p[a][1] * s, cx + p[b][0] * s, cy + p[b][1] * s, T * 0.35);
      const r = [0.55, 0.9, 0.5, 0.62, 0.45];
      p.forEach((pt, i) => g.fc(cx + pt[0] * s, cy + pt[1] * s, T * r[i]));
      break;
    }

    case 'giardino':                                 // sprout: stem + two leaves + bud, over a soil mound
      g.frr(cx - s * 0.85, cy + s * 0.55, s * 1.7, s * 0.45, s * 0.16);
      g.bx(cx - T * 0.4, cy - s * 0.25, T * 0.8, s * 0.85);
      g.fc(cx - s * 0.34, cy - s * 0.28, s * 0.34); g.fc(cx + s * 0.34, cy - s * 0.28, s * 0.34);
      g.fc(cx, cy - s * 0.62, s * 0.3);
      break;

    case 'slots':
      g.frr(cx - s * 0.85, cy - s * 0.7, s * 1.7, s * 1.5, s * 0.16);
      g.frr(cx - s * 0.6, cy - s * 0.45, s * 1.2, s * 0.9, 2, g.bg);
      g.bx(cx - s * 0.2, cy - s * 0.45, T * 0.45, s * 0.9, g.col); g.bx(cx + s * 0.2 - T * 0.45, cy - s * 0.45, T * 0.45, s * 0.9, g.col);
      g.bx(cx + s * 0.85, cy - s * 0.6, T * 0.7, s * 0.7); g.fc(cx + s * 0.85 + T * 0.35, cy - s * 0.6, T * 0.7);
      break;

    case 'Tools':
      for (let i = 0; i < 8; i++) { const a = i * Math.PI / 4; g.bx(cx + Math.cos(a) * s * 0.84 - T * 0.45, cy + Math.sin(a) * s * 0.84 - T * 0.45, T * 0.9, T * 0.9); }
      g.fc(cx, cy, s * 0.6); g.fc(cx, cy, s * 0.24, g.bg);
      break;

    case 'Office':
      g.frr(cx - s, cy - s * 0.4, 2 * s, s * 1.3, s * 0.16);
      g.frr(cx - s * 0.45, cy - s * 0.8, s * 0.9, s * 0.5, s * 0.16, g.col); g.frr(cx - s * 0.28, cy - s * 0.62, s * 0.56, s * 0.4, 2, g.bg);
      g.bx(cx - s, cy + s * 0.06, 2 * s, T * 0.65, g.bg);
      break;

    case 'device':                                   // phone / device: body, screen, earpiece, home dot
      g.frr(cx - s * 0.58, cy - s, s * 1.16, 2 * s, s * 0.22);
      g.frr(cx - s * 0.4, cy - s * 0.68, s * 0.8, s * 1.25, 2, g.bg);
      g.bx(cx - s * 0.18, cy - s * 0.85, s * 0.36, T * 0.4, g.bg); g.fc(cx, cy + s * 0.78, T * 0.5, g.bg);
      break;

    case 'poker':                                    // playing card with a heart pip
      g.frr(cx - s * 0.68, cy - s, s * 1.36, 2 * s, s * 0.18);
      g.fc(cx - s * 0.2, cy - s * 0.18, s * 0.22, g.bg); g.fc(cx + s * 0.2, cy - s * 0.18, s * 0.22, g.bg);
      g.tri(cx - s * 0.41, cy - s * 0.06, cx + s * 0.41, cy - s * 0.06, cx, cy + s * 0.45, g.bg);
      g.fc(cx - s * 0.44, cy - s * 0.76, T * 0.42, g.bg); g.fc(cx + s * 0.44, cy + s * 0.76, T * 0.42, g.bg);
      break;

    default:
      return false;
  }
  return true;
}

// The id list this set covers, for the gallery and for sanity checks.
export const ICON_IDS = [
  'clock', 'anima', 'calc', 'files', 'calendar', 'notepad', 'usb', 'usbkbd', 'music', 'video', 'Media',
  'photos', 'recorder', 'micspec', 'voicelab', 'info', 'Connect', 'sysmon', 'System', 'radio', 'remote',
  'ir', 'qr', 'notify', 'torch', 'theme', 'wifi', 'link', 'swarm', 'ssh', 'ethernet', 'beacon', 'wifiatk',
  'evilportal', 'Security', 'Games', 'reactor', 'pong', 'tanks', 'stelle', 'giardino', 'slots', 'Tools', 'Office',
  'device', 'poker',
];
