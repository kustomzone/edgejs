'use strict';

function Stream() {
  this.writable = true;
}

Stream.prototype.write = function write(chunk, cb) {
  if (typeof cb === 'function') cb(null);
  return true;
};

module.exports = Stream;
