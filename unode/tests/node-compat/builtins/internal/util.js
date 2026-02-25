'use strict';

const isWindows = typeof process !== 'undefined' && process.platform === 'win32';

function getLazy(initializer) {
  let initialized = false;
  let value;
  return function lazyValue() {
    if (!initialized) {
      value = initializer();
      initialized = true;
    }
    return value;
  };
}

module.exports = {
  getLazy,
  isWindows,
};
