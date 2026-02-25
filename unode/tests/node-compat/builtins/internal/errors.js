'use strict';

class AbortError extends Error {
  constructor(message = 'The operation was aborted', options = undefined) {
    super(message);
    this.name = 'AbortError';
    this.code = 'ABORT_ERR';
    if (options && 'cause' in options) {
      this.cause = options.cause;
    }
  }
}

class ERR_INVALID_ARG_VALUE extends TypeError {
  constructor(name, value) {
    super(`The argument '${name}' is invalid. Received ${String(value)}`);
    this.code = 'ERR_INVALID_ARG_VALUE';
  }
}

class ERR_INVALID_CURSOR_POS extends RangeError {
  constructor() {
    super('Cannot set cursor row/column to NaN');
    this.code = 'ERR_INVALID_CURSOR_POS';
  }
}

class ERR_USE_AFTER_CLOSE extends Error {
  constructor(name) {
    super(`${name} was closed`);
    this.code = 'ERR_USE_AFTER_CLOSE';
  }
}

class ERR_INVALID_ARG_TYPE extends TypeError {
  constructor(name, expected, actual) {
    let received;
    if (actual == null) {
      received = ` Received ${actual}`;
    } else if (typeof actual === 'function') {
      received = ` Received function ${actual.name || '<anonymous>'}`;
    } else if (typeof actual === 'object') {
      if (actual.constructor?.name) {
        received = ` Received an instance of ${actual.constructor.name}`;
      } else {
        received = ` Received ${typeof actual}`;
      }
    } else {
      const str = String(actual);
      let shown = str.slice(0, 25);
      if (str.length > 25) {
        shown += '...';
      }
      received = ` Received type ${typeof actual} (${shown})`;
    }
    super(`The "${name}" argument must be of type ${expected}.${received}`);
    this.code = 'ERR_INVALID_ARG_TYPE';
  }
}

module.exports = {
  AbortError,
  codes: {
    ERR_INVALID_ARG_VALUE,
    ERR_INVALID_CURSOR_POS,
    ERR_USE_AFTER_CLOSE,
    ERR_INVALID_ARG_TYPE,
  },
};
