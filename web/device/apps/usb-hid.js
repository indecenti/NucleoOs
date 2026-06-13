// JS twin of the USB Keyboard translation logic in firmware:
//   - firmware/components/nucleo_usbhid/nucleo_usbhid.c  (nucleo_usbhid_ascii: ASCII -> HID)
//   - firmware/components/nucleo_app/app_usbkbd.cpp       (kbd_loop: key+mods -> HID report)
//
// This is the reference SPEC for what each Cardputer key sends to the host over USB HID. The
// firmware must match it; the test suite (tools/usb-hid.test.mjs) locks it down so a regression in
// the keycode/modifier rules is caught without hardware. (The physical SD/USB enumeration itself
// is verified on-device.)

// Cardputer modifier bitmask (NK_MOD_* in nucleo_kbd.h).
export const MOD = { SHIFT: 0x01, CTRL: 0x02, ALT: 0x04, GUI: 0x08, FN: 0x10 };

// USB HID modifier byte bits (NK_HIDMOD_* in nucleo_usbhid.h).
export const HIDMOD = { CTRL: 0x01, SHIFT: 0x02, ALT: 0x04, GUI: 0x08 };

// HID usage IDs for the non-printable keys the app sends (NK_HID_* in nucleo_usbhid.h).
export const HID = {
  ENTER: 0x28, ESC: 0x29, BKSP: 0x2a, TAB: 0x2b, SPACE: 0x2c,
  RIGHT: 0x4f, LEFT: 0x50, DOWN: 0x51, UP: 0x52,
};

// Shifted-symbol map for the US layout (char -> base unshifted char on the same key).
const SHIFTED = {
  '!': '1', '@': '2', '#': '3', $: '4', '%': '5', '^': '6', '&': '7', '*': '8', '(': '9', ')': '0',
  _: '-', '+': '=', '{': '[', '}': ']', '|': '\\', ':': ';', '"': "'", '~': '`', '<': ',', '>': '.', '?': '/',
};
// Base (unshifted) char -> HID usage id.
const BASE = {
  '-': 0x2d, '=': 0x2e, '[': 0x2f, ']': 0x30, '\\': 0x31, ';': 0x33, "'": 0x34, '`': 0x35,
  ',': 0x36, '.': 0x37, '/': 0x38, ' ': 0x2c,
};

// ASCII char -> { mod, key } (HID), or null if unmapped. Mirrors TinyUSB's HID_ASCII_TO_KEYCODE
// for the US layout — the table nucleo_usbhid_ascii() reads.
export function asciiToHid(ch) {
  if (typeof ch !== 'string' || ch.length !== 1) return null;
  const c = ch.charCodeAt(0);
  if (c >= 97 && c <= 122) return { mod: 0, key: 0x04 + (c - 97) };            // a-z
  if (c >= 65 && c <= 90) return { mod: HIDMOD.SHIFT, key: 0x04 + (c - 65) };  // A-Z
  if (c >= 49 && c <= 57) return { mod: 0, key: 0x1e + (c - 49) };             // 1-9
  if (ch === '0') return { mod: 0, key: 0x27 };
  if (ch in SHIFTED) {
    const base = asciiToHid(SHIFTED[ch]);
    return base ? { mod: HIDMOD.SHIFT, key: base.key } : null;
  }
  if (ch in BASE) return { mod: 0, key: BASE[ch] };
  return null;
}

// Map a Cardputer modifier bitmask to the HID modifier byte (Ctrl/Alt/Gui; Shift is folded in per
// key by asciiToHid). Mirrors the hidmod assembly in kbd_loop().
export function nkModsToHid(mods) {
  let m = 0;
  if (mods & MOD.CTRL) m |= HIDMOD.CTRL;
  if (mods & MOD.ALT) m |= HIDMOD.ALT;
  if (mods & MOD.GUI) m |= HIDMOD.GUI;
  return m;
}

// Translate a key event { key, ch, mods } to the HID report it sends, mirroring kbd_loop():
//   - { exit: true }         backtick leaves the keyboard (not sent to host)
//   - { mod, key }           a keystroke to send
//   - null                   nothing to send
// `key` is one of: CHAR, UP, DOWN, LEFT, RIGHT, ENTER, DEL, TAB, BACK, NONE.
export function translateKey({ key = 'CHAR', ch = '', mods = 0 } = {}) {
  if (key === 'BACK') return { exit: true };
  if (key === 'NONE') return null;

  const hidmod = nkModsToHid(mods);
  const fn = !!(mods & MOD.FN);
  const shift = mods & MOD.SHIFT ? HIDMOD.SHIFT : 0;

  if (fn && (key === 'UP' || key === 'DOWN' || key === 'LEFT' || key === 'RIGHT')) {
    return { mod: hidmod, key: HID[key] };
  }
  if (key === 'ENTER') return { mod: hidmod | shift, key: HID.ENTER };
  if (key === 'DEL') return { mod: hidmod, key: HID.BKSP };
  if (key === 'TAB') return { mod: hidmod | shift, key: HID.TAB };
  if (ch === ' ') return { mod: hidmod, key: HID.SPACE };

  const code = ch.charCodeAt ? ch.charCodeAt(0) : 0;
  if (code >= 33 && code < 127) {
    const a = asciiToHid(ch);
    if (a) return { mod: hidmod | a.mod, key: a.key };
  }
  return null;
}
