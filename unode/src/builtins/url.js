'use strict';
function pathToFileURL(p) {
  const path = require('path');
  const joined = path.join ? path.join(p) : String(p);
  const href = 'file://' + (joined.indexOf('/') === 0 ? joined : '/' + joined);
  return { href: href };
}
module.exports = { pathToFileURL };
