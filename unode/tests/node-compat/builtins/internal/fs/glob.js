'use strict';

const { validateString } = require('internal/validators');

function escapeRegexChar(char) {
  return /[\\^$.*+?()[\]{}|]/.test(char) ? `\\${char}` : char;
}

function compilePattern(pattern) {
  let out = '^';
  for (let i = 0; i < pattern.length; i++) {
    const ch = pattern[i];
    if (ch === '*') {
      if (pattern[i + 1] === '*') {
        while (pattern[i + 1] === '*') i++;
        out += '.*';
      } else {
        out += '[^/]*';
      }
      continue;
    }

    if (ch === '[') {
      const close = pattern.indexOf(']', i + 1);
      if (close !== -1) {
        let cls = pattern.slice(i + 1, close);
        if (cls.startsWith('!')) cls = `^${cls.slice(1)}`;
        out += `[${cls}]`;
        i = close;
        continue;
      }
    }

    out += escapeRegexChar(ch);
  }
  out += '$';
  return new RegExp(out);
}

function normalizeWinPath(value) {
  return value.replace(/\\/g, '/');
}

function matchGlobPattern(path, pattern, isWindows) {
  validateString(path, 'path');
  validateString(pattern, 'pattern');

  const normalizedPath = isWindows ? normalizeWinPath(path) : path;
  const normalizedPattern = isWindows ? normalizeWinPath(pattern) : pattern;
  return compilePattern(normalizedPattern).test(normalizedPath);
}

module.exports = {
  matchGlobPattern,
};
