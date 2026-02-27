'use strict';

function now() {
  if (typeof performance === 'object' &&
      performance &&
      typeof performance.now === 'function') {
    return performance.now();
  }
  return Date.now();
}

module.exports = {
  now,
};
