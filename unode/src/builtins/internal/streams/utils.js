'use strict';

function isWritable(stream) {
  return stream != null && typeof stream.write === 'function';
}

module.exports = { isWritable };
