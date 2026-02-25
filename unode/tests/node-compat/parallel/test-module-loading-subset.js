'use strict';
require('../common');
const assert = require('assert');
const path = require('../common/path');

{
  const a_js = require('../fixtures/a.js');
  assert.strictEqual(a_js.number, 42);
}

{
  const foo_no_ext = require('../fixtures/foo');
  assert.strictEqual(foo_no_ext.foo, 'ok');
}

const a = require('../fixtures/a');
const d = require('../fixtures/b/d');
const d2 = require('../fixtures/b/d');

{
  const c = require('../fixtures/b/c');
  const d3 = require(path.join(__dirname, '../fixtures/b/d'));
  const d4 = require('../fixtures/b/d');

  assert.ok(a.A instanceof Function);
  assert.strictEqual(a.A(), 'A');

  assert.ok(a.C instanceof Function);
  assert.strictEqual(a.C(), 'C');

  assert.ok(a.D instanceof Function);
  assert.strictEqual(a.D(), 'D');

  assert.ok(d.D instanceof Function);
  assert.strictEqual(d.D(), 'D');

  assert.ok(d2.D instanceof Function);
  assert.strictEqual(d2.D(), 'D');

  assert.ok(d3.D instanceof Function);
  assert.strictEqual(d3.D(), 'D');

  assert.ok(d4.D instanceof Function);
  assert.strictEqual(d4.D(), 'D');

  assert.ok((new a.SomeClass()) instanceof c.SomeClass);
}

{
  const one = require('../fixtures/nested-index/one');
  const two = require('../fixtures/nested-index/two');
  assert.notStrictEqual(one.hello, two.hello);
}

{
  const three = require('../fixtures/nested-index/three');
  const threeFolder = require('../fixtures/nested-index/three/');
  const threeIndex = require('../fixtures/nested-index/three/index.js');
  assert.strictEqual(threeFolder.label, threeIndex.label);
  assert.notStrictEqual(threeFolder.label, three.label);
}

assert.strictEqual(require('../fixtures/packages/index').ok, 'ok');
assert.strictEqual(require('../fixtures/packages/main').ok, 'ok');
assert.strictEqual(require('../fixtures/packages/main-index').ok, 'ok');
