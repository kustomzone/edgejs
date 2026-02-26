// Copyright Joyent, Inc. and other Node contributors.
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the
// "Software"), to deal in the Software without restriction, including
// without limitation the rights to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to permit
// persons to whom the Software is furnished to do so, subject to the
// following conditions:
//
// The above copyright notice and this permission notice shall be included
// in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN
// NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
// DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
// OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
// USE OR OTHER DEALINGS IN THE SOFTWARE.

'use strict';

const EventEmitter = require('events');
const {
  clearLine,
  clearScreenDown,
  cursorTo,
  moveCursor,
} = require('internal/readline/callbacks');
const emitKeypressEvents = require('internal/readline/emitKeypressEvents');

class Interface extends EventEmitter {
  constructor(input, output) {
    super();
    this.input = input;
    this.output = output;
    this.closed = false;
  }
  resume() {
    if (this.input && typeof this.input.resume === 'function') this.input.resume();
    return this;
  }
  pause() {
    if (this.input && typeof this.input.pause === 'function') this.input.pause();
    return this;
  }
  _setRawMode(mode) {
    if (this.input && typeof this.input.setRawMode === 'function') this.input.setRawMode(mode);
    return this;
  }
  close() {
    if (this.closed) return this;
    this.pause();
    this._setRawMode(false);
    this.closed = true;
    this.emit('close');
    return this;
  }
}

function createInterface(input, output) {
  return new Interface(input, output);
}

module.exports = {
  Interface,
  clearLine,
  clearScreenDown,
  createInterface,
  cursorTo,
  emitKeypressEvents,
  moveCursor,
  promises: require('readline/promises'),
};
