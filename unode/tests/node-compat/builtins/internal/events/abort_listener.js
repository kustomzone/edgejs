'use strict';

const { SymbolDispose } = require('internal/util/primordials');

function addAbortListener(signal, listener) {
  if (!signal || typeof signal.addEventListener !== 'function') {
    return { [SymbolDispose]() {} };
  }
  signal.addEventListener('abort', listener, { once: true });
  return {
    [SymbolDispose]() {
      signal.removeEventListener('abort', listener);
    },
  };
}

module.exports = { addAbortListener };
