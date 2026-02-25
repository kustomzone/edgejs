'use strict';

const primordials = require('internal/util/primordials');
const {
  NumberIsNaN,
} = primordials;

const {
  codes: {
    ERR_INVALID_ARG_VALUE,
    ERR_INVALID_CURSOR_POS,
  },
} = require('internal/errors');

const {
  validateFunction,
} = require('internal/validators');
const {
  CSI,
} = require('internal/readline/utils');

const {
  kClearLine,
  kClearScreenDown,
  kClearToLineBeginning,
  kClearToLineEnd,
} = CSI;

function cursorTo(stream, x, y, callback) {
  if (callback !== undefined) validateFunction(callback, 'callback');
  if (typeof y === 'function') {
    callback = y;
    y = undefined;
  }
  if (NumberIsNaN(x)) throw new ERR_INVALID_ARG_VALUE('x', x);
  if (NumberIsNaN(y)) throw new ERR_INVALID_ARG_VALUE('y', y);

  if (stream == null || (typeof x !== 'number' && typeof y !== 'number')) {
    if (typeof callback === 'function') process.nextTick(callback, null);
    return true;
  }
  if (typeof x !== 'number') throw new ERR_INVALID_CURSOR_POS();
  const data = typeof y !== 'number' ? CSI`${x + 1}G` : CSI`${y + 1};${x + 1}H`;
  return stream.write(data, callback);
}

function moveCursor(stream, dx, dy, callback) {
  if (callback !== undefined) validateFunction(callback, 'callback');
  if (stream == null || !(dx || dy)) {
    if (typeof callback === 'function') process.nextTick(callback, null);
    return true;
  }
  let data = '';
  if (dx < 0) data += CSI`${-dx}D`;
  else if (dx > 0) data += CSI`${dx}C`;
  if (dy < 0) data += CSI`${-dy}A`;
  else if (dy > 0) data += CSI`${dy}B`;
  return stream.write(data, callback);
}

function clearLine(stream, dir, callback) {
  if (callback !== undefined) validateFunction(callback, 'callback');
  if (stream == null) {
    if (typeof callback === 'function') process.nextTick(callback, null);
    return true;
  }
  const type = dir < 0 ? kClearToLineBeginning : dir > 0 ? kClearToLineEnd : kClearLine;
  return stream.write(type, callback);
}

function clearScreenDown(stream, callback) {
  if (callback !== undefined) validateFunction(callback, 'callback');
  if (stream == null) {
    if (typeof callback === 'function') process.nextTick(callback, null);
    return true;
  }
  return stream.write(kClearScreenDown, callback);
}

module.exports = {
  clearLine,
  clearScreenDown,
  cursorTo,
  moveCursor,
};
