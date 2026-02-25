'use strict';

const primordials = require('internal/util/primordials');
const {
  ArrayPrototypeJoin,
  ArrayPrototypePush,
  Promise,
} = primordials;

const { CSI } = require('internal/readline/utils');
const { validateBoolean, validateInteger } = require('internal/validators');
const { isWritable } = require('internal/streams/utils');
const { codes: {
  ERR_INVALID_ARG_TYPE,
} } = require('internal/errors');

const {
  kClearToLineBeginning,
  kClearToLineEnd,
  kClearLine,
  kClearScreenDown,
} = CSI;

class Readline {
  #autoCommit = false;
  #stream;
  #todo = [];

  constructor(stream, options = undefined) {
    if (!isWritable(stream))
      throw new ERR_INVALID_ARG_TYPE('stream', 'Writable', stream);
    this.#stream = stream;
    if (options?.autoCommit != null) {
      validateBoolean(options.autoCommit, 'options.autoCommit');
      this.#autoCommit = options.autoCommit;
    }
  }

  cursorTo(x, y = undefined) {
    validateInteger(x, 'x');
    if (y != null) validateInteger(y, 'y');
    const data = y == null ? CSI`${x + 1}G` : CSI`${y + 1};${x + 1}H`;
    if (this.#autoCommit) process.nextTick(() => this.#stream.write(data));
    else ArrayPrototypePush(this.#todo, data);
    return this;
  }

  moveCursor(dx, dy) {
    if (dx || dy) {
      validateInteger(dx, 'dx');
      validateInteger(dy, 'dy');

      let data = '';
      if (dx < 0) data += CSI`${-dx}D`;
      else if (dx > 0) data += CSI`${dx}C`;
      if (dy < 0) data += CSI`${-dy}A`;
      else if (dy > 0) data += CSI`${dy}B`;

      if (this.#autoCommit) process.nextTick(() => this.#stream.write(data));
      else ArrayPrototypePush(this.#todo, data);
    }
    return this;
  }

  clearLine(dir) {
    validateInteger(dir, 'dir', -1, 1);
    const data = dir < 0 ? kClearToLineBeginning : dir > 0 ? kClearToLineEnd : kClearLine;
    if (this.#autoCommit) process.nextTick(() => this.#stream.write(data));
    else ArrayPrototypePush(this.#todo, data);
    return this;
  }

  clearScreenDown() {
    if (this.#autoCommit) process.nextTick(() => this.#stream.write(kClearScreenDown));
    else ArrayPrototypePush(this.#todo, kClearScreenDown);
    return this;
  }

  commit() {
    return new Promise((resolve) => {
      this.#stream.write(ArrayPrototypeJoin(this.#todo, ''), resolve);
      this.#todo = [];
    });
  }

  rollback() {
    this.#todo = [];
    return this;
  }
}

module.exports = {
  Readline,
};
