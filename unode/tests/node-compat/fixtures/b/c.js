const d = require('./d');
const assert = require('assert');
const pkg = require('./package');

assert.strictEqual('world', pkg.hello);

var string = 'C';

exports.SomeClass = function() {};

exports.C = function() {
  return string;
};

exports.D = function() {
  return d.D();
};
