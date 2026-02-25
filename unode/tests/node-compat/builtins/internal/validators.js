'use strict';

function formatReceived(value) {
  if (value === null) return ' Received null';
  if (value === undefined) return ' Received undefined';
  if (typeof value === 'function') {
    return ` Received function ${value.name || '<anonymous>'}`;
  }
  if (typeof value === 'object') {
    if (value && value.constructor && value.constructor.name) {
      return ` Received an instance of ${value.constructor.name}`;
    }
    return ' Received an instance of Object';
  }
  const text = String(value);
  if (text.length > 25) {
    return ` Received type ${typeof value} (${text.slice(0, 25)}...)`;
  }
  return ` Received type ${typeof value} (${text})`;
}

function createTypeError(name, expectedType, actualValue) {
  const err = new TypeError(`The "${name}" argument must be of type ${expectedType}.${formatReceived(actualValue)}`);
  err.code = 'ERR_INVALID_ARG_TYPE';
  return err;
}

function validateString(value, name) {
  if (typeof value !== 'string') {
    throw createTypeError(name, 'string', value);
  }
}

function validateObject(value, name) {
  if (value === null || typeof value !== 'object') {
    throw createTypeError(name, 'object', value);
  }
}

module.exports = {
  validateObject,
  validateString,
};
