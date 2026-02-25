'use strict';

const primordials = require('internal/util/primordials');
const {
  ArrayPrototypeToSorted,
  RegExpPrototypeExec,
  StringFromCharCode,
  StringPrototypeCharCodeAt,
  StringPrototypeCodePointAt,
  StringPrototypeSlice,
  StringPrototypeSplit,
  StringPrototypeToLowerCase,
  Symbol,
} = primordials;

const kUTF16SurrogateThreshold = 0x10000;
const kEscape = '\x1b';
const kSubstringSearch = Symbol('kSubstringSearch');

function CSI(strings, ...args) {
  let ret = `${kEscape}[`;
  for (let n = 0; n < strings.length; n++) {
    ret += strings[n];
    if (n < args.length) ret += args[n];
  }
  return ret;
}

CSI.kEscape = kEscape;
CSI.kClearToLineBeginning = CSI`1K`;
CSI.kClearToLineEnd = CSI`0K`;
CSI.kClearLine = CSI`2K`;
CSI.kClearScreenDown = CSI`0J`;

function charLengthLeft(str, i) {
  if (i <= 0) return 0;
  if ((i > 1 && StringPrototypeCodePointAt(str, i - 2) >= kUTF16SurrogateThreshold) ||
      StringPrototypeCodePointAt(str, i - 1) >= kUTF16SurrogateThreshold) {
    return 2;
  }
  return 1;
}

function charLengthAt(str, i) {
  if (str.length <= i) return 1;
  return StringPrototypeCodePointAt(str, i) >= kUTF16SurrogateThreshold ? 2 : 1;
}

function* emitKeys(stream) {
  while (true) {
    let ch = yield;
    let s = ch;
    let escaped = false;
    const key = {
      sequence: null,
      name: undefined,
      ctrl: false,
      meta: false,
      shift: false,
    };

    if (ch === kEscape) {
      escaped = true;
      s += (ch = yield);
      if (ch === kEscape) s += (ch = yield);
    }

    if (escaped && (ch === 'O' || ch === '[')) {
      let code = ch;
      let modifier = 0;

      if (ch === 'O') {
        s += (ch = yield);
        if (ch >= '0' && ch <= '9') {
          modifier = (ch >> 0) - 1;
          s += (ch = yield);
        }
        code += ch;
      } else if (ch === '[') {
        s += (ch = yield);
        if (ch === '[') {
          code += ch;
          s += (ch = yield);
        }

        const cmdStart = s.length - 1;
        if (ch >= '0' && ch <= '9') {
          s += (ch = yield);
          if (ch >= '0' && ch <= '9') {
            s += (ch = yield);
            if (ch >= '0' && ch <= '9') s += (ch = yield);
          }
        }

        if (ch === ';') {
          s += (ch = yield);
          if (ch >= '0' && ch <= '9') s += yield;
        }

        const cmd = StringPrototypeSlice(s, cmdStart);
        let match;
        if ((match = RegExpPrototypeExec(/^(?:(\d\d?)(?:;(\d))?([~^$])|(\d{3}~))$/, cmd))) {
          if (match[4]) {
            code += match[4];
          } else {
            code += match[1] + match[3];
            modifier = (match[2] || 1) - 1;
          }
        } else if ((match = RegExpPrototypeExec(/^((\d;)?(\d))?([A-Za-z])$/, cmd))) {
          code += match[4];
          modifier = (match[3] || 1) - 1;
        } else {
          code += cmd;
        }
      }

      key.ctrl = !!(modifier & 4);
      key.meta = !!(modifier & 10);
      key.shift = !!(modifier & 1);
      key.code = code;

      switch (code) {
        case '[P': key.name = 'f1'; break;
        case '[Q': key.name = 'f2'; break;
        case '[R': key.name = 'f3'; break;
        case '[S': key.name = 'f4'; break;
        case 'OP': key.name = 'f1'; break;
        case 'OQ': key.name = 'f2'; break;
        case 'OR': key.name = 'f3'; break;
        case 'OS': key.name = 'f4'; break;
        case '[11~': key.name = 'f1'; break;
        case '[12~': key.name = 'f2'; break;
        case '[13~': key.name = 'f3'; break;
        case '[14~': key.name = 'f4'; break;
        case '[200~': key.name = 'paste-start'; break;
        case '[201~': key.name = 'paste-end'; break;
        case '[[A': key.name = 'f1'; break;
        case '[[B': key.name = 'f2'; break;
        case '[[C': key.name = 'f3'; break;
        case '[[D': key.name = 'f4'; break;
        case '[[E': key.name = 'f5'; break;
        case '[15~': key.name = 'f5'; break;
        case '[17~': key.name = 'f6'; break;
        case '[18~': key.name = 'f7'; break;
        case '[19~': key.name = 'f8'; break;
        case '[20~': key.name = 'f9'; break;
        case '[21~': key.name = 'f10'; break;
        case '[23~': key.name = 'f11'; break;
        case '[24~': key.name = 'f12'; break;
        case '[A': key.name = 'up'; break;
        case '[B': key.name = 'down'; break;
        case '[C': key.name = 'right'; break;
        case '[D': key.name = 'left'; break;
        case '[E': key.name = 'clear'; break;
        case '[F': key.name = 'end'; break;
        case '[H': key.name = 'home'; break;
        case 'OA': key.name = 'up'; break;
        case 'OB': key.name = 'down'; break;
        case 'OC': key.name = 'right'; break;
        case 'OD': key.name = 'left'; break;
        case 'OE': key.name = 'clear'; break;
        case 'OF': key.name = 'end'; break;
        case 'OH': key.name = 'home'; break;
        case '[1~': key.name = 'home'; break;
        case '[2~': key.name = 'insert'; break;
        case '[3~': key.name = 'delete'; break;
        case '[4~': key.name = 'end'; break;
        case '[5~': key.name = 'pageup'; break;
        case '[6~': key.name = 'pagedown'; break;
        case '[[5~': key.name = 'pageup'; break;
        case '[[6~': key.name = 'pagedown'; break;
        case '[7~': key.name = 'home'; break;
        case '[8~': key.name = 'end'; break;
        case '[a': key.name = 'up'; key.shift = true; break;
        case '[b': key.name = 'down'; key.shift = true; break;
        case '[c': key.name = 'right'; key.shift = true; break;
        case '[d': key.name = 'left'; key.shift = true; break;
        case '[e': key.name = 'clear'; key.shift = true; break;
        case '[2$': key.name = 'insert'; key.shift = true; break;
        case '[3$': key.name = 'delete'; key.shift = true; break;
        case '[5$': key.name = 'pageup'; key.shift = true; break;
        case '[6$': key.name = 'pagedown'; key.shift = true; break;
        case '[7$': key.name = 'home'; key.shift = true; break;
        case '[8$': key.name = 'end'; key.shift = true; break;
        case 'Oa': key.name = 'up'; key.ctrl = true; break;
        case 'Ob': key.name = 'down'; key.ctrl = true; break;
        case 'Oc': key.name = 'right'; key.ctrl = true; break;
        case 'Od': key.name = 'left'; key.ctrl = true; break;
        case 'Oe': key.name = 'clear'; key.ctrl = true; break;
        case '[2^': key.name = 'insert'; key.ctrl = true; break;
        case '[3^': key.name = 'delete'; key.ctrl = true; break;
        case '[5^': key.name = 'pageup'; key.ctrl = true; break;
        case '[6^': key.name = 'pagedown'; key.ctrl = true; break;
        case '[7^': key.name = 'home'; key.ctrl = true; break;
        case '[8^': key.name = 'end'; key.ctrl = true; break;
        case '[Z': key.name = 'tab'; key.shift = true; break;
        default: key.name = 'undefined'; break;
      }
    } else if (ch === '\r') {
      key.name = 'return';
      key.meta = escaped;
    } else if (ch === '\n') {
      key.name = 'enter';
      key.meta = escaped;
    } else if (ch === '\t') {
      key.name = 'tab';
      key.meta = escaped;
    } else if (ch === '\b' || ch === '\x7f') {
      key.name = 'backspace';
      key.meta = escaped;
    } else if (ch === kEscape) {
      key.name = 'escape';
      key.meta = escaped;
    } else if (ch === ' ') {
      key.name = 'space';
      key.meta = escaped;
    } else if (!escaped && ch <= '\x1a') {
      key.name = StringFromCharCode(
        StringPrototypeCharCodeAt(ch) + StringPrototypeCharCodeAt('a') - 1,
      );
      key.ctrl = true;
    } else if (RegExpPrototypeExec(/^[0-9A-Za-z]$/, ch) !== null) {
      key.name = StringPrototypeToLowerCase(ch);
      key.shift = RegExpPrototypeExec(/^[A-Z]$/, ch) !== null;
      key.meta = escaped;
    } else if (escaped) {
      key.name = ch.length ? undefined : 'escape';
      key.meta = true;
    }

    key.sequence = s;
    if (s.length !== 0 && (key.name !== undefined || escaped)) {
      stream.emit('keypress', escaped ? undefined : s, key);
    } else if (charLengthAt(s, 0) === s.length) {
      stream.emit('keypress', s, key);
    }
  }
}

function commonPrefix(strings) {
  if (strings.length === 0) return '';
  if (strings.length === 1) return strings[0];
  const sorted = ArrayPrototypeToSorted(strings);
  const min = sorted[0];
  const max = sorted[sorted.length - 1];
  for (let i = 0; i < min.length; i++) {
    if (min[i] !== max[i]) return StringPrototypeSlice(min, 0, i);
  }
  return min;
}

function reverseString(line, from = '\r', to = '\r') {
  const parts = StringPrototypeSplit(line, from);
  let result = '';
  for (let i = parts.length - 1; i > 0; i--) result += parts[i] + to;
  result += parts[0];
  return result;
}

module.exports = {
  charLengthAt,
  charLengthLeft,
  commonPrefix,
  emitKeys,
  reverseString,
  kSubstringSearch,
  CSI,
};
