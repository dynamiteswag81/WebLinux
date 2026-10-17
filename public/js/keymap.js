// KeyboardEvent.code -> Linux input keycode (include/uapi/linux/input-event-codes.h).

export const KEYCODES = {
  Escape: 1, Digit1: 2, Digit2: 3, Digit3: 4, Digit4: 5, Digit5: 6, Digit6: 7,
  Digit7: 8, Digit8: 9, Digit9: 10, Digit0: 11, Minus: 12, Equal: 13, Backspace: 14,
  Tab: 15, KeyQ: 16, KeyW: 17, KeyE: 18, KeyR: 19, KeyT: 20, KeyY: 21, KeyU: 22,
  KeyI: 23, KeyO: 24, KeyP: 25, BracketLeft: 26, BracketRight: 27, Enter: 28,
  ControlLeft: 29, KeyA: 30, KeyS: 31, KeyD: 32, KeyF: 33, KeyG: 34, KeyH: 35,
  KeyJ: 36, KeyK: 37, KeyL: 38, Semicolon: 39, Quote: 40, Backquote: 41,
  ShiftLeft: 42, Backslash: 43, KeyZ: 44, KeyX: 45, KeyC: 46, KeyV: 47, KeyB: 48,
  KeyN: 49, KeyM: 50, Comma: 51, Period: 52, Slash: 53, ShiftRight: 54,
  NumpadMultiply: 55, AltLeft: 56, Space: 57, CapsLock: 58, F1: 59, F2: 60, F3: 61,
  F4: 62, F5: 63, F6: 64, F7: 65, F8: 66, F9: 67, F10: 68, NumLock: 69,
  ScrollLock: 70, Numpad7: 71, Numpad8: 72, Numpad9: 73, NumpadSubtract: 74,
  Numpad4: 75, Numpad5: 76, Numpad6: 77, NumpadAdd: 78, Numpad1: 79, Numpad2: 80,
  Numpad3: 81, Numpad0: 82, NumpadDecimal: 83, IntlBackslash: 86, F11: 87, F12: 88,
  IntlRo: 89, NumpadEnter: 96, ControlRight: 97, NumpadDivide: 98, PrintScreen: 99,
  AltRight: 100, Home: 102, ArrowUp: 103, PageUp: 104, ArrowLeft: 105,
  ArrowRight: 106, End: 107, ArrowDown: 108, PageDown: 109, Insert: 110,
  Delete: 111, NumpadEqual: 117, Pause: 119, IntlYen: 124, MetaLeft: 125,
  MetaRight: 126, ContextMenu: 127,
};

// Linux event types/codes used by the host devices
export const EV_SYN = 0, EV_KEY = 1, EV_REL = 2, EV_ABS = 3;
export const BTN_LEFT = 0x110, BTN_RIGHT = 0x111, BTN_MIDDLE = 0x112;
export const REL_HWHEEL = 6, REL_WHEEL = 8;
export const ABS_X = 0, ABS_Y = 1, ABS_MAX = 32767;

// Characters that need Shift on a US layout, per key: lets synthetic
// events (key "$" with code Digit4 and no Shift) come out right.
const SHIFTED = {
  Digit1: '!', Digit2: '@', Digit3: '#', Digit4: '$', Digit5: '%', Digit6: '^',
  Digit7: '&', Digit8: '*', Digit9: '(', Digit0: ')', Minus: '_', Equal: '+',
  BracketLeft: '{', BracketRight: '}', Backslash: '|', Semicolon: ':', Quote: '"',
  Backquote: '~', Comma: '<', Period: '>', Slash: '?',
};
export function isShifted(code, key) {
  if (SHIFTED[code] === key) return true;
  return code.startsWith('Key') && key.length === 1 && key >= 'A' && key <= 'Z';
}

// US layout: character -> [KeyboardEvent.code, shift], for typing pasted text
const UNSHIFTED = {
  Minus: '-', Equal: '=', BracketLeft: '[', BracketRight: ']', Backslash: '\\',
  Semicolon: ';', Quote: "'", Backquote: '`', Comma: ',', Period: '.', Slash: '/',
  Space: ' ', Enter: '\n', Tab: '\t',
};
const CHARS = new Map();
for (let i = 0; i < 26; i++) {
  const l = String.fromCharCode(97 + i), code = 'Key' + l.toUpperCase();
  CHARS.set(l, [code, false]);
  CHARS.set(l.toUpperCase(), [code, true]);
}
for (let d = 0; d < 10; d++) CHARS.set(String(d), ['Digit' + d, false]);
for (const [code, ch] of Object.entries(UNSHIFTED)) CHARS.set(ch, [code, false]);
for (const [code, ch] of Object.entries(SHIFTED)) CHARS.set(ch, [code, true]);
export function charKey(ch) { return CHARS.get(ch === '\r' ? '\n' : ch); }
