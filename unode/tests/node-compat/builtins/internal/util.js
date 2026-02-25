'use strict';

const isWindows = typeof process !== 'undefined' && process.platform === 'win32';
const kEmptyObject = Object.freeze({ __proto__: null });

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

function assignFunctionName(name, fn) {
  try {
    Object.defineProperty(fn, 'name', {
      __proto__: null,
      configurable: true,
      value: typeof name === 'symbol' ? name.description || '' : String(name),
    });
  } catch {
    // Ignore defineProperty failures for non-configurable names.
  }
  return fn;
}

const promisify = function promisify(fn) {
  return (...args) => new Promise((resolve, reject) => {
    fn(...args, (err, value) => (err ? reject(err) : resolve(value)));
  });
};
promisify.custom = Symbol.for('nodejs.util.promisify.custom');

module.exports = {
  getLazy,
  isWindows,
  assignFunctionName,
  kEmptyObject,
  promisify,
};
