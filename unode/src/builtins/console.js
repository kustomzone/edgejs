'use strict';

const util = require('util');

const kMaxGroupIndentation = 1000;

function createError(name, code, message) {
  const Ctor = name === 'RangeError' ? RangeError : TypeError;
  const err = new Ctor(message);
  err.code = code;
  return err;
}

function validateWritableStream(stream, name) {
  if (!stream || typeof stream.write !== 'function') {
    throw createError('TypeError', 'ERR_CONSOLE_WRITABLE_STREAM',
                      'Console expects a writable stream for ' + name);
  }
}

function validateGroupIndentation(groupIndentation) {
  if (groupIndentation === undefined) return;
  if (typeof groupIndentation !== 'number') {
    throw createError('TypeError', 'ERR_INVALID_ARG_TYPE',
                      'The "groupIndentation" property must be of type number');
  }
  if (!Number.isFinite(groupIndentation) || Math.floor(groupIndentation) !== groupIndentation) {
    throw createError('RangeError', 'ERR_OUT_OF_RANGE',
                      'The value of "groupIndentation" is out of range. It must be an integer.');
  }
  if (groupIndentation < 0 || groupIndentation > kMaxGroupIndentation) {
    throw createError('RangeError', 'ERR_OUT_OF_RANGE',
                      'The value of "groupIndentation" is out of range. It must be >= 0 && <= 1000.');
  }
}

function validateInspectOptions(inspectOptions) {
  if (inspectOptions === undefined) return;
  if (!inspectOptions || typeof inspectOptions !== 'object' || Array.isArray(inspectOptions)) {
    throw createError('TypeError', 'ERR_INVALID_ARG_TYPE',
                      'The "options.inspectOptions" property must be of type object.');
  }
}

function formatArgs(args, inspectOptions) {
  if (args.length === 0) return '';
  if (typeof args[0] === 'string') {
    return util.format.apply(null, args);
  }
  return args.map(function(arg) {
    return util.inspect(arg, inspectOptions);
  }).join(' ');
}

function Console(options) {
  if (!(this instanceof Console)) {
    return Reflect.construct(Console, arguments);
  }

  if (!options || typeof options.write === 'function') {
    options = {
      stdout: options,
      stderr: arguments[1],
      ignoreErrors: arguments[2],
    };
  }

  const stdout = options.stdout;
  const stderr = options.stderr || stdout;
  const ignoreErrors = options.ignoreErrors !== undefined ? Boolean(options.ignoreErrors) : true;
  const inspectOptions = options.inspectOptions || {};
  const groupIndentation = options.groupIndentation === undefined ? 2 : options.groupIndentation;

  validateWritableStream(stdout, 'stdout');
  validateWritableStream(stderr, 'stderr');
  validateInspectOptions(options.inspectOptions);
  validateGroupIndentation(groupIndentation);

  this._stdout = stdout;
  this._stderr = stderr;
  this._ignoreErrors = ignoreErrors;
  this._times = new Map();
  this._counts = new Map();
  this._inspectOptions = inspectOptions;
  this._groupIndentationWidth = groupIndentation;
  this._groupIndent = '';

  const methods = Object.keys(Console.prototype);
  for (let i = 0; i < methods.length; i += 1) {
    const key = methods[i];
    if (key === 'constructor') continue;
    const bound = this[key].bind(this);
    this[key] = (...args) => bound(...args);
    Object.defineProperty(this[key], 'name', { value: key });
  }
}

Console.prototype._writeTo = function(stream, string) {
  if (this._groupIndent) {
    string = this._groupIndent + String(string).replace(/\n/g, '\n' + this._groupIndent);
  }
  string += '\n';
  if (!this._ignoreErrors) {
    stream.write(string);
    return;
  }
  try {
    stream.write(string);
  } catch (_) {
    // Match Node console behavior: swallow write errors by default.
  }
};

Console.prototype.log = function log() {
  this._writeTo(this._stdout, formatArgs(Array.prototype.slice.call(arguments), this._inspectOptions));
};

Console.prototype.warn = function warn() {
  this._writeTo(this._stderr, formatArgs(Array.prototype.slice.call(arguments), this._inspectOptions));
};

Console.prototype.dir = function dir(object, options) {
  const opts = Object.assign({}, this._inspectOptions, options || {}, { customInspect: false });
  this._writeTo(this._stdout, util.inspect(object, opts));
};

Console.prototype.trace = function trace() {
  const msg = formatArgs(Array.prototype.slice.call(arguments), this._inspectOptions);
  const err = new Error(msg);
  err.name = 'Trace';
  this._writeTo(this._stderr, err.stack || ('Trace: ' + msg));
};

Console.prototype.assert = function assert(expression) {
  if (expression) return;
  const args = Array.prototype.slice.call(arguments, 1);
  if (args.length > 0 && typeof args[0] === 'string') {
    args[0] = 'Assertion failed: ' + args[0];
  } else {
    args.unshift('Assertion failed');
  }
  this.warn.apply(this, args);
};

Console.prototype.clear = function clear() {
  if (this._stdout && this._stdout.isTTY && process && process.env && process.env.TERM !== 'dumb') {
    this._stdout.write('\u001b[1;1H\u001b[0J');
  }
};

Console.prototype.count = function count(label) {
  const key = String(label === undefined ? 'default' : label);
  const next = (this._counts.get(key) || 0) + 1;
  this._counts.set(key, next);
  this.log('%s: %d', key, next);
};

Console.prototype.countReset = function countReset(label) {
  const key = String(label === undefined ? 'default' : label);
  if (!this._counts.has(key)) {
    if (process && typeof process.emitWarning === 'function') {
      process.emitWarning("Count for '" + key + "' does not exist");
    }
    return;
  }
  this._counts.delete(key);
};

Console.prototype.group = function group() {
  if (arguments.length > 0) {
    this.log.apply(this, arguments);
  }
  this._groupIndent += ' '.repeat(this._groupIndentationWidth);
};

Console.prototype.groupEnd = function groupEnd() {
  this._groupIndent = this._groupIndent.slice(0, Math.max(0, this._groupIndent.length - this._groupIndentationWidth));
};

Console.prototype.time = function time(label) {
  const key = String(label === undefined ? 'default' : label);
  if (this._times.has(key)) {
    if (process && typeof process.emitWarning === 'function') {
      process.emitWarning("Label '" + key + "' already exists for console.time()");
    }
    return;
  }
  this._times.set(key, Date.now());
};

function formatDiff(start) {
  const diff = Date.now() - start;
  return diff + 'ms';
}

Console.prototype.timeLog = function timeLog(label) {
  const key = String(label === undefined ? 'default' : label);
  if (!this._times.has(key)) {
    if (process && typeof process.emitWarning === 'function') {
      process.emitWarning("No such label '" + key + "' for console.timeLog()");
    }
    return;
  }
  const args = Array.prototype.slice.call(arguments, 1);
  const diff = formatDiff(this._times.get(key));
  if (args.length === 0) this.log('%s: %s', key, diff);
  else this.log.apply(this, [key + ': ' + diff].concat(args));
};

Console.prototype.timeEnd = function timeEnd(label) {
  const key = String(label === undefined ? 'default' : label);
  if (!this._times.has(key)) {
    if (process && typeof process.emitWarning === 'function') {
      process.emitWarning("No such label '" + key + "' for console.timeEnd()");
    }
    return;
  }
  const diff = formatDiff(this._times.get(key));
  this._times.delete(key);
  this.log('%s: %s', key, diff);
};

Console.prototype.table = function table(tabularData) {
  if (tabularData instanceof Set) {
    this.log(util.inspect(Array.from(tabularData), this._inspectOptions));
    return;
  }
  if (tabularData instanceof Map) {
    this.log(util.inspect(Array.from(tabularData.entries()), this._inspectOptions));
    return;
  }
  this.log(util.inspect(tabularData, this._inspectOptions));
};

Console.prototype.debug = Console.prototype.log;
Console.prototype.info = Console.prototype.log;
Console.prototype.dirxml = Console.prototype.log;
Console.prototype.error = Console.prototype.warn;
Console.prototype.groupCollapsed = Console.prototype.group;

const globalConsole = new Console({
  stdout: process.stdout,
  stderr: process.stderr,
  ignoreErrors: true,
});
globalConsole.Console = Console;

module.exports = globalConsole;
