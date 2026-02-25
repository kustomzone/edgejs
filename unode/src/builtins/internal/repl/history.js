'use strict';

class ReplHistory {
  constructor(rl, options = {}) {
    this.rl = rl;
    this.history = Array.isArray(options.history) ? options.history.slice() : [];
    this.index = -1;
    this.size = Number.isInteger(options.size) ? options.size : (Number.isInteger(options.historySize) ? options.historySize : 30);
    this.removeHistoryDuplicates = Boolean(options.removeHistoryDuplicates);
    this.isFlushing = false;
  }

  initialize(cb) {
    if (typeof cb === 'function') cb(null, this);
  }

  addHistory(isMultiline) {
    const line = isMultiline ? this.rl.line : String(this.rl.line || '').trimEnd();
    if (line.length > 0) {
      if (this.removeHistoryDuplicates) {
        this.history = this.history.filter((entry) => entry !== line);
      }
      this.history.unshift(line);
      if (this.history.length > this.size) this.history.length = this.size;
    }
    this.index = -1;
    return this.rl.line;
  }

  canNavigateToPrevious() {
    return this.history.length > 0 && this.index + 1 < this.history.length;
  }

  canNavigateToNext() {
    return this.index >= 0;
  }

  navigateToPrevious(search) {
    const nextIndex = this.findFrom(this.index + 1, +1, search);
    if (nextIndex === -1) return this.rl.line;
    this.index = nextIndex;
    return this.history[this.index] || '';
  }

  navigateToNext(search) {
    if (this.index <= 0) {
      this.index = -1;
      return '';
    }
    const nextIndex = this.findFrom(this.index - 1, -1, search);
    this.index = nextIndex;
    return nextIndex === -1 ? '' : (this.history[nextIndex] || '');
  }

  findFrom(start, step, search) {
    if (search == null || search === '') return start;
    for (let i = start; i >= 0 && i < this.history.length; i += step) {
      if (String(this.history[i]).startsWith(search)) return i;
    }
    return -1;
  }
}

module.exports = { ReplHistory };
