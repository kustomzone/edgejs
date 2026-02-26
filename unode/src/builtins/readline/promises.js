'use strict';

class Interface {
  constructor(input, output, completer, terminal) {
    this._rl = require('readline').createInterface(input, output, completer, terminal);
  }
  question(query, options = undefined) {
    return new Promise((resolve) => this._rl.question(query, options, resolve));
  }
  close() { return this._rl.close(); }
  pause() { return this._rl.pause(); }
  resume() { return this._rl.resume(); }
}

function createInterface(input, output, completer, terminal) {
  return new Interface(input, output, completer, terminal);
}

module.exports = {
  Interface,
  Readline: class Readline {},
  createInterface,
};
