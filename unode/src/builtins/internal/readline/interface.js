'use strict';

const EventEmitter = require('events');

const kAddHistory = Symbol('kAddHistory');
const kDecoder = Symbol('kDecoder');
const kDeleteLeft = Symbol('kDeleteLeft');
const kDeleteLineLeft = Symbol('kDeleteLineLeft');
const kDeleteLineRight = Symbol('kDeleteLineRight');
const kDeleteRight = Symbol('kDeleteRight');
const kDeleteWordLeft = Symbol('kDeleteWordLeft');
const kDeleteWordRight = Symbol('kDeleteWordRight');
const kGetDisplayPos = Symbol('kGetDisplayPos');
const kHistoryNext = Symbol('kHistoryNext');
const kHistoryPrev = Symbol('kHistoryPrev');
const kInsertString = Symbol('kInsertString');
const kLine = Symbol('kLine');
const kLine_buffer = Symbol('kLine_buffer');
const kMoveCursor = Symbol('kMoveCursor');
const kNormalWrite = Symbol('kNormalWrite');
const kOldPrompt = Symbol('kOldPrompt');
const kOnLine = Symbol('kOnLine');
const kPreviousKey = Symbol('kPreviousKey');
const kPrompt = Symbol('kPrompt');
const kQuestion = Symbol('kQuestion');
const kQuestionCallback = Symbol('kQuestionCallback');
const kQuestionCancel = Symbol('kQuestionCancel');
const kQuestionReject = Symbol('kQuestionReject');
const kRefreshLine = Symbol('kRefreshLine');
const kSawKeyPress = Symbol('kSawKeyPress');
const kSawReturnAt = Symbol('kSawReturnAt');
const kSetRawMode = Symbol('kSetRawMode');
const kTabComplete = Symbol('kTabComplete');
const kTabCompleter = Symbol('kTabCompleter');
const kTtyWrite = Symbol('kTtyWrite');
const kWordLeft = Symbol('kWordLeft');
const kWordRight = Symbol('kWordRight');
const kWriteToOutput = Symbol('kWriteToOutput');

class Interface extends EventEmitter {
  constructor(input, output) {
    super();
    this.input = input;
    this.output = output;
    this[kLine_buffer] = '';
    this[kSawReturnAt] = 0;
  }
  pause() { if (this.input && typeof this.input.pause === 'function') this.input.pause(); return this; }
  resume() { if (this.input && typeof this.input.resume === 'function') this.input.resume(); return this; }
  close() { this.pause(); this[kSetRawMode](false); this.emit('close'); return this; }
  [kSetRawMode](mode) { if (this.input && typeof this.input.setRawMode === 'function') this.input.setRawMode(mode); }
  [kQuestion](query, cb) { if (typeof cb === 'function') process.nextTick(cb, ''); }
  [kQuestionCancel]() {}
  [kOnLine]() {}
  [kWriteToOutput](s) { if (this.output && typeof this.output.write === 'function') this.output.write(String(s)); }
}

function InterfaceConstructor(input, output, completer, terminal) {
  this.input = input;
  this.output = output;
  this[kLine_buffer] = '';
  this[kSawReturnAt] = 0;
  return this;
}

module.exports = {
  Interface,
  InterfaceConstructor,
  kAddHistory,
  kDecoder,
  kDeleteLeft,
  kDeleteLineLeft,
  kDeleteLineRight,
  kDeleteRight,
  kDeleteWordLeft,
  kDeleteWordRight,
  kGetDisplayPos,
  kHistoryNext,
  kHistoryPrev,
  kInsertString,
  kLine,
  kLine_buffer,
  kMoveCursor,
  kNormalWrite,
  kOldPrompt,
  kOnLine,
  kPreviousKey,
  kPrompt,
  kQuestion,
  kQuestionCallback,
  kQuestionCancel,
  kQuestionReject,
  kRefreshLine,
  kSawKeyPress,
  kSawReturnAt,
  kSetRawMode,
  kTabComplete,
  kTabCompleter,
  kTtyWrite,
  kWordLeft,
  kWordRight,
  kWriteToOutput,
};
