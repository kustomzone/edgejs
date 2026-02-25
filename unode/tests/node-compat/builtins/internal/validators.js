'use strict';

const {
  codes: {
    ERR_INVALID_ARG_TYPE,
    ERR_INVALID_ARG_VALUE,
  },
} = require('internal/errors');

function validateFunction(value, name) {
  if (typeof value !== 'function') {
    throw new ERR_INVALID_ARG_TYPE(name, 'Function', value);
  }
}

function validateAbortSignal(value, name) {
  if (!value || typeof value !== 'object' || typeof value.aborted !== 'boolean') {
    throw new ERR_INVALID_ARG_TYPE(name, 'AbortSignal', value);
  }
}

function validateString(value, name) {
  if (typeof value !== 'string') {
    throw new ERR_INVALID_ARG_TYPE(name, 'string', value);
  }
}

function validateUint32(value, name, positive) {
  if (!Number.isInteger(value)) {
    throw new ERR_INVALID_ARG_TYPE(name, 'number', value);
  }
  if (value < 0 || value > 0xFFFFFFFF || (positive && value === 0)) {
    throw new ERR_INVALID_ARG_VALUE(name, value);
  }
}

function validateObject(value, name) {
  if (value === null || typeof value !== 'object') {
    throw new ERR_INVALID_ARG_TYPE(name, 'object', value);
  }
}

function validateBoolean(value, name) {
  if (typeof value !== 'boolean') {
    throw new ERR_INVALID_ARG_TYPE(name, 'boolean', value);
  }
}

function validateInteger(value, name, min = Number.MIN_SAFE_INTEGER, max = Number.MAX_SAFE_INTEGER) {
  if (!Number.isInteger(value)) {
    throw new ERR_INVALID_ARG_TYPE(name, 'number', value);
  }
  if (value < min || value > max) {
    throw new ERR_INVALID_ARG_VALUE(name, value);
  }
}

module.exports = {
  validateObject,
  validateAbortSignal,
  validateBoolean,
  validateFunction,
  validateInteger,
  validateString,
  validateUint32,
};
