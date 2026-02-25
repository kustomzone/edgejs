'use strict';

function runWithContext(code, context) {
  const sandbox = context && typeof context === 'object' ? context : {};
  const keys = Object.keys(sandbox);
  const values = keys.map((k) => sandbox[k]);
  const wrapped = new Function(...keys, '__code', '"use strict"; return eval(__code);');
  return wrapped(...values, String(code));
}

function runInNewContext(code, context) {
  return runWithContext(code, context);
}

function runInThisContext(code) {
  return (0, eval)(String(code));
}

module.exports = {
  runInNewContext,
  runInThisContext,
};
