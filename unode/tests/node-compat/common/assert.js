'use strict';

// Re-export the builtin assert so tests can use require('../common/assert') or require('assert').
// Both resolve to the same implementation (unode/tests/node-compat/builtins/assert.js).
module.exports = require('assert');
