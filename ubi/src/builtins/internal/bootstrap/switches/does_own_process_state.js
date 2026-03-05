'use strict';

const path = require('path');

require(path.resolve(__dirname, '../../../../../../node-lib/internal/bootstrap/switches/does_own_process_state.js'));

const processMethods = internalBinding('process_methods');

// Node exposes process.abort as a non-constructor builtin function.
if (processMethods && typeof processMethods.abort === 'function') {
  process.abort = (...args) => processMethods.abort(...args);
}

const processProto = Object.getPrototypeOf(process);
if (processProto) {
  const ProcessCtor = function process() {};
  Object.defineProperty(processProto, 'constructor', {
    __proto__: null,
    value: ProcessCtor,
    writable: true,
    enumerable: false,
    configurable: true,
  });
}

if (!Object.prototype.hasOwnProperty.call(process, Symbol.toStringTag)) {
  Object.defineProperty(process, Symbol.toStringTag, {
    __proto__: null,
    value: 'process',
    writable: false,
    enumerable: false,
    configurable: true,
  });
}

const ERR_INVALID_OBJECT_DEFINE_PROPERTY = 'ERR_INVALID_OBJECT_DEFINE_PROPERTY';
const kInvalidDataDescriptorMessage =
  '\'process.env\' only accepts a configurable, writable, and enumerable data descriptor';
const kInvalidAccessorDescriptorMessage =
  '\'process.env\' does not accept an accessor(getter/setter) descriptor';

function throwEnvDefinePropertyError(message) {
  const err = new TypeError(message);
  err.code = ERR_INVALID_OBJECT_DEFINE_PROPERTY;
  throw err;
}

function coerceEnvKey(prop) {
  if (typeof prop === 'symbol') return null;
  return String(prop);
}

function coerceEnvValue(value) {
  if (typeof value === 'symbol') {
    throw new TypeError('Cannot convert a Symbol value to a string');
  }
  return String(value);
}

function installProcessEnvProxy() {
  const source = process.env && typeof process.env === 'object' ? process.env : {};
  const store = {};
  for (const key of Object.keys(source)) {
    store[key] = coerceEnvValue(source[key]);
  }

  const applySet = (key, value) => {
    if (key === '') return true;
    const stringValue = coerceEnvValue(value);
    store[key] = stringValue;
    if (processMethods && typeof processMethods._setEnv === 'function') {
      processMethods._setEnv(key, stringValue);
    }
    return true;
  };

  const handler = {
    get(target, prop, receiver) {
      if (typeof prop === 'symbol') {
        return Reflect.get(target, prop, receiver);
      }
      const key = String(prop);
      if (Object.prototype.hasOwnProperty.call(target, key)) return target[key];
      return Reflect.get(target, prop, receiver);
    },
    set(target, prop, value) {
      if (typeof prop === 'symbol') {
        throw new TypeError('Cannot convert a Symbol value to a string');
      }
      const key = String(prop);
      return applySet(key, value);
    },
    deleteProperty(target, prop) {
      if (typeof prop === 'symbol') return true;
      const key = String(prop);
      delete target[key];
      if (processMethods && typeof processMethods._unsetEnv === 'function') {
        processMethods._unsetEnv(key);
      }
      return true;
    },
    has(target, prop) {
      if (typeof prop === 'symbol') return false;
      return Object.prototype.hasOwnProperty.call(target, String(prop));
    },
    ownKeys(target) {
      return Object.keys(target);
    },
    getOwnPropertyDescriptor(target, prop) {
      if (typeof prop === 'symbol') return undefined;
      const key = String(prop);
      if (!Object.prototype.hasOwnProperty.call(target, key)) return undefined;
      return {
        __proto__: null,
        value: target[key],
        writable: true,
        enumerable: true,
        configurable: true,
      };
    },
    defineProperty(target, prop, descriptor) {
      const key = coerceEnvKey(prop);
      if (key === null) {
        throw new TypeError('Cannot convert a Symbol value to a string');
      }

      if (descriptor &&
          (Object.prototype.hasOwnProperty.call(descriptor, 'get') ||
           Object.prototype.hasOwnProperty.call(descriptor, 'set'))) {
        throwEnvDefinePropertyError(kInvalidAccessorDescriptorMessage);
      }

      const hasConfigurable = descriptor && Object.prototype.hasOwnProperty.call(descriptor, 'configurable');
      const hasWritable = descriptor && Object.prototype.hasOwnProperty.call(descriptor, 'writable');
      const hasEnumerable = descriptor && Object.prototype.hasOwnProperty.call(descriptor, 'enumerable');
      if (!hasConfigurable || !hasWritable || !hasEnumerable ||
          descriptor.configurable !== true ||
          descriptor.writable !== true ||
          descriptor.enumerable !== true) {
        throwEnvDefinePropertyError(kInvalidDataDescriptorMessage);
      }

      return applySet(key, descriptor?.value);
    },
  };

  const envProxy = new Proxy(store, handler);
  Object.defineProperty(process, 'env', {
    __proto__: null,
    value: envProxy,
    writable: true,
    enumerable: true,
    configurable: true,
  });
}

installProcessEnvProxy();
