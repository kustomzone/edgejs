'use strict';

// Verify that require('node:assert') resolves to the same builtin as require('assert').
require('../common');
const assertFromNode = require('node:assert');
const assertFromBare = require('assert');

assertFromBare.strictEqual(assertFromNode, assertFromBare, 'node:assert and assert should be the same module');
assertFromBare.strictEqual(assertFromNode.strictEqual, assertFromBare.strictEqual);
assertFromBare.strictEqual(assertFromNode.AssertionError, assertFromBare.AssertionError);

// Basic API smoke test for node:assert
assertFromNode.ok(true);
assertFromNode.strictEqual(1, 1);
assertFromNode.deepStrictEqual({ a: 1 }, { a: 1 });
assertFromNode.throws(function () { throw new Error('expected'); });
assertFromNode.throws(function () { throw new Error('msg'); }, /msg/);

console.log('node:assert and assert tests passed');
