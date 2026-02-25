'use strict';

const util = require('util');

function stripVTControlCharacters(str) {
  return String(str).replace(/\u001b\[[0-9;]*[A-Za-z]/g, '');
}

function getStringWidth(str) {
  let width = 0;
  for (const ch of String(str)) {
    const code = ch.codePointAt(0);
    if (code <= 0x1f || (code >= 0x7f && code <= 0x9f)) continue;
    width += code > 0xff ? 2 : 1;
  }
  return width;
}

module.exports = {
  inspect: util.inspect || ((value) => String(value)),
  getStringWidth,
  stripVTControlCharacters,
};
