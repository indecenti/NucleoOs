// Tests for the USB Keyboard translation spec (web/device/apps/usb-hid.js), the JS twin of the
// firmware HID keymap (nucleo_usbhid.c) + key handling (app_usbkbd.cpp). Run: npm test.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { asciiToHid, translateKey, nkModsToHid, MOD, HIDMOD, HID } from '../web/device/apps/usb-hid.js';

// ---- ASCII -> HID keycode (US layout) ---------------------------------------
test('lowercase letters map to 0x04..0x1d, no modifier', () => {
  assert.deepEqual(asciiToHid('a'), { mod: 0, key: 0x04 });
  assert.deepEqual(asciiToHid('z'), { mod: 0, key: 0x1d });
});
test('uppercase letters add Shift', () => {
  assert.deepEqual(asciiToHid('A'), { mod: HIDMOD.SHIFT, key: 0x04 });
  assert.deepEqual(asciiToHid('C'), { mod: HIDMOD.SHIFT, key: 0x06 });
});
test('digits 1-9 then 0', () => {
  assert.deepEqual(asciiToHid('1'), { mod: 0, key: 0x1e });
  assert.deepEqual(asciiToHid('9'), { mod: 0, key: 0x26 });
  assert.deepEqual(asciiToHid('0'), { mod: 0, key: 0x27 });
});
test('shifted number-row symbols share the digit key with Shift', () => {
  assert.deepEqual(asciiToHid('!'), { mod: HIDMOD.SHIFT, key: 0x1e });
  assert.deepEqual(asciiToHid(')'), { mod: HIDMOD.SHIFT, key: 0x27 });
});
test('punctuation: base and shifted variants', () => {
  assert.deepEqual(asciiToHid('-'), { mod: 0, key: 0x2d });
  assert.deepEqual(asciiToHid('_'), { mod: HIDMOD.SHIFT, key: 0x2d });
  assert.deepEqual(asciiToHid('/'), { mod: 0, key: 0x38 });
  assert.deepEqual(asciiToHid('?'), { mod: HIDMOD.SHIFT, key: 0x38 });
  assert.deepEqual(asciiToHid(';'), { mod: 0, key: 0x33 });
  assert.deepEqual(asciiToHid(':'), { mod: HIDMOD.SHIFT, key: 0x33 });
});
test('unmapped chars return null', () => {
  assert.equal(asciiToHid('é'), null);   // é (non-ASCII)
  assert.equal(asciiToHid(''), null);
});

// ---- modifier bitmask folding ----------------------------------------------
test('Cardputer mods fold to HID modifier byte (shift excluded here)', () => {
  assert.equal(nkModsToHid(MOD.CTRL), HIDMOD.CTRL);
  assert.equal(nkModsToHid(MOD.ALT | MOD.GUI), HIDMOD.ALT | HIDMOD.GUI);
  assert.equal(nkModsToHid(MOD.SHIFT), 0);       // shift is per-key, not folded here
  assert.equal(nkModsToHid(MOD.FN), 0);          // Fn is a local layer, never sent
});

// ---- full key translation (mirrors kbd_loop) -------------------------------
test('plain letter', () => {
  assert.deepEqual(translateKey({ key: 'CHAR', ch: 'a' }), { mod: 0, key: 0x04 });
});
test('Ctrl+C combo', () => {
  assert.deepEqual(translateKey({ key: 'CHAR', ch: 'c', mods: MOD.CTRL }), { mod: HIDMOD.CTRL, key: 0x06 });
});
test('Ctrl+Shift+C keeps both modifiers', () => {
  assert.deepEqual(translateKey({ key: 'CHAR', ch: 'C', mods: MOD.CTRL | MOD.SHIFT }),
    { mod: HIDMOD.CTRL | HIDMOD.SHIFT, key: 0x06 });
});
test('Alt+Tab', () => {
  assert.deepEqual(translateKey({ key: 'TAB', mods: MOD.ALT }), { mod: HIDMOD.ALT, key: HID.TAB });
});
test('GUI (Win) + e', () => {
  assert.deepEqual(translateKey({ key: 'CHAR', ch: 'e', mods: MOD.GUI }), { mod: HIDMOD.GUI, key: 0x08 });
});
test('Enter / Backspace / Space specials', () => {
  assert.deepEqual(translateKey({ key: 'ENTER' }), { mod: 0, key: HID.ENTER });
  assert.deepEqual(translateKey({ key: 'DEL' }), { mod: 0, key: HID.BKSP });
  assert.deepEqual(translateKey({ key: 'CHAR', ch: ' ' }), { mod: 0, key: HID.SPACE });
});
test('arrow legends type punctuation by default (no Fn)', () => {
  // ';' key reports as UP with ch=';' — without Fn it must send the comma/semicolon char, not Up.
  assert.deepEqual(translateKey({ key: 'UP', ch: ';' }), { mod: 0, key: 0x33 });
  assert.deepEqual(translateKey({ key: 'LEFT', ch: ',' }), { mod: 0, key: 0x36 });
});
test('Fn turns the legends into real arrows', () => {
  assert.deepEqual(translateKey({ key: 'UP', ch: ';', mods: MOD.FN }), { mod: 0, key: HID.UP });
  assert.deepEqual(translateKey({ key: 'RIGHT', ch: '/', mods: MOD.FN }), { mod: 0, key: HID.RIGHT });
});
test('backtick exits the keyboard (not sent to host)', () => {
  assert.deepEqual(translateKey({ key: 'BACK' }), { exit: true });
});
test('no key / unmapped sends nothing', () => {
  assert.equal(translateKey({ key: 'NONE' }), null);
  assert.equal(translateKey({ key: 'CHAR', ch: 'é' }), null);
});
