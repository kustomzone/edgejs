'use strict';

const {
  internalBinding,
  primordials,
} = require('internal/bootstrap/internal_binding');

const kInternalPrefix = 'internal/';
const kSchemeOnlyBuiltinIds = new Set();
const processBindingAllowList = new Set([
  'buffer',
  'cares_wrap',
  'config',
  'constants',
  'contextify',
  'fs',
  'fs_event_wrap',
  'icu',
  'inspector',
  'js_stream',
  'os',
  'pipe_wrap',
  'process_wrap',
  'spawn_sync',
  'stream_wrap',
  'tcp_wrap',
  'tls_wrap',
  'tty_wrap',
  'udp_wrap',
  'uv',
  'zlib',
]);
const runtimeDeprecatedList = new Set();
const legacyWrapperList = new Set(['natives', 'util']);

const { builtinIds = [] } = internalBinding('builtins') || {};
const allBuiltinSet = new Set(builtinIds);
const publicBuiltinIds = builtinIds.filter((id) =>
  !id.startsWith(kInternalPrefix) && id !== 'internal_test_binding'
);
const internalBuiltinIds = builtinIds.filter((id) =>
  id.startsWith(kInternalPrefix) && id !== 'internal/bootstrap/realm'
);

let canBeRequiredByUsersList = new Set(publicBuiltinIds);
let canBeRequiredByUsersWithoutSchemeList = new Set(
  publicBuiltinIds.filter((id) => !kSchemeOnlyBuiltinIds.has(id))
);

if (!Array.isArray(process.moduleLoadList)) {
  const moduleLoadList = [];
  Object.defineProperty(process, 'moduleLoadList', {
    __proto__: null,
    value: moduleLoadList,
    configurable: true,
    enumerable: true,
    writable: false,
  });
}

{
  const bindingObj = Object.create(null);
  process.binding = function binding(module) {
    module = String(module);
    let mod = bindingObj[module];
    if (typeof mod === 'object') return mod;

    if (runtimeDeprecatedList.has(module)) {
      process.emitWarning(
        `Access to process.binding('${module}') is deprecated.`,
        'DeprecationWarning',
        'DEP0111'
      );
      mod = internalBinding(module);
    } else if (legacyWrapperList.has(module)) {
      mod = require('internal/legacy/processbinding')[module]();
    } else if (processBindingAllowList.has(module)) {
      mod = internalBinding(module);
    } else {
      throw new Error(`No such module: ${module}`);
    }

    bindingObj[module] = mod;
    process.moduleLoadList.push(`Binding ${module}`);
    return mod;
  };

  process._linkedBinding = function _linkedBinding(module) {
    module = String(module);
    let mod = bindingObj[module];
    if (typeof mod !== 'object') {
      const getLinkedBinding = globalThis.getLinkedBinding;
      mod = typeof getLinkedBinding === 'function' ? getLinkedBinding(module) : undefined;
      if (typeof mod === 'object' && mod !== null) {
        bindingObj[module] = mod;
      }
    }
    return mod;
  };
}

function BuiltinModule() {}

BuiltinModule.exists = function exists(id) {
  const value = String(id || '');
  const normalized = value.startsWith('node:') ? value.slice(5) : value;
  return allBuiltinSet.has(normalized);
};

BuiltinModule.canBeRequiredByUsers = function canBeRequiredByUsers(id) {
  return canBeRequiredByUsersList.has(String(id || ''));
};

BuiltinModule.canBeRequiredWithoutScheme = function canBeRequiredWithoutScheme(id) {
  return canBeRequiredByUsersWithoutSchemeList.has(String(id || ''));
};

BuiltinModule.allowRequireByUsers = function allowRequireByUsers(id) {
  const normalized = String(id || '');
  if (!allBuiltinSet.has(normalized)) return;
  canBeRequiredByUsersList.add(normalized);
  if (!kSchemeOnlyBuiltinIds.has(normalized)) {
    canBeRequiredByUsersWithoutSchemeList.add(normalized);
  }
};

BuiltinModule.setRealmAllowRequireByUsers = function setRealmAllowRequireByUsers(ids) {
  const allow = new Set();
  const allowWithoutScheme = new Set();
  for (const id of ids || []) {
    const normalized = String(id || '');
    if (!allBuiltinSet.has(normalized)) continue;
    allow.add(normalized);
    if (!kSchemeOnlyBuiltinIds.has(normalized)) {
      allowWithoutScheme.add(normalized);
    }
  }
  canBeRequiredByUsersList = allow;
  canBeRequiredByUsersWithoutSchemeList = allowWithoutScheme;
};

BuiltinModule.exposeInternals = function exposeInternals() {
  for (const id of internalBuiltinIds) {
    BuiltinModule.allowRequireByUsers(id);
  }
};

BuiltinModule.normalizeRequirableId = function normalizeRequirableId(id) {
  const value = String(id || '');
  if (value.startsWith('node:')) {
    const normalized = value.slice(5);
    if (BuiltinModule.canBeRequiredByUsers(normalized)) return normalized;
    return undefined;
  }
  if (BuiltinModule.canBeRequiredWithoutScheme(value)) return value;
  return undefined;
};

BuiltinModule.isBuiltin = function isBuiltin(id) {
  return this.normalizeRequirableId(id) !== undefined;
};

BuiltinModule.getSchemeOnlyModuleNames = function getSchemeOnlyModuleNames() {
  return Array.from(kSchemeOnlyBuiltinIds);
};

BuiltinModule.getAllBuiltinModuleIds = function getAllBuiltinModuleIds() {
  const ids = Array.from(canBeRequiredByUsersWithoutSchemeList);
  for (const id of kSchemeOnlyBuiltinIds) {
    ids.push(`node:${id}`);
  }
  ids.sort();
  return ids;
};

function setupPrepareStackTrace() {
  try {
    const errorsBinding = internalBinding('errors');
    if (!errorsBinding || typeof errorsBinding !== 'object') return;

    const {
      setEnhanceStackForFatalException,
      setPrepareStackTraceCallback,
    } = errorsBinding;
    if (typeof setPrepareStackTraceCallback !== 'function' ||
        typeof setEnhanceStackForFatalException !== 'function') {
      return;
    }

    const {
      prepareStackTraceCallback,
      ErrorPrepareStackTrace,
      fatalExceptionStackEnhancers,
    } = require('internal/errors');

    if (typeof prepareStackTraceCallback === 'function') {
      setPrepareStackTraceCallback(prepareStackTraceCallback);
    }

    const beforeInspector =
      fatalExceptionStackEnhancers &&
      fatalExceptionStackEnhancers.beforeInspector;
    const afterInspector =
      fatalExceptionStackEnhancers &&
      fatalExceptionStackEnhancers.afterInspector;
    if (typeof beforeInspector === 'function' &&
        typeof afterInspector === 'function') {
      setEnhanceStackForFatalException(beforeInspector, afterInspector);
    }

    if (typeof ErrorPrepareStackTrace === 'function') {
      Object.defineProperty(Error, 'prepareStackTrace', {
        __proto__: null,
        writable: true,
        enumerable: false,
        configurable: true,
        value: ErrorPrepareStackTrace,
      });
    }
  } catch {
    // Keep bootstrap resilient in subprocess/minimal startup modes.
  }
}

setupPrepareStackTrace();

module.exports = { internalBinding, primordials, BuiltinModule };
