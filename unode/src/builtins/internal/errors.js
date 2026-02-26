'use strict';

class AbortError extends Error {
  constructor(message = 'The operation was aborted', options = undefined) {
    super(message);
    this.name = 'AbortError';
    this.code = 'ABORT_ERR';
    if (options && 'cause' in options) {
      this.cause = options.cause;
    }
  }
}

class ERR_INVALID_ARG_VALUE extends TypeError {
  constructor(name, value, reason = 'is invalid') {
    const formatted = typeof value === 'string' ? `'${value}'` : String(value);
    const label = String(name).includes('.') ? 'property' : 'argument';
    super(`The ${label} '${name}' ${reason}. Received ${formatted}`);
    this.code = 'ERR_INVALID_ARG_VALUE';
  }
}

class ERR_INVALID_CURSOR_POS extends RangeError {
  constructor() {
    super('Cannot set cursor row/column to NaN');
    this.code = 'ERR_INVALID_CURSOR_POS';
  }
}

class ERR_USE_AFTER_CLOSE extends Error {
  constructor(name) {
    super(`${name} was closed`);
    this.code = 'ERR_USE_AFTER_CLOSE';
  }
}

class ERR_INVALID_ARG_TYPE extends TypeError {
  constructor(name, expected, actual) {
    function formatName(n) {
      const s = String(n);
      if (s.includes('.')) return `The "${s}" property`;
      if (s.endsWith(' argument')) return `The ${s}`;
      return `The "${s}" argument`;
    }
    function formatExpected(exp) {
      function joinWithOr(parts) {
        if (parts.length <= 1) return parts[0] || '';
        if (parts.length === 2) return `${parts[0]} or ${parts[1]}`;
        return `${parts.slice(0, -1).join(', ')}, or ${parts[parts.length - 1]}`;
      }
      if (Array.isArray(exp)) {
        const parts = exp.map(String);
        if (parts.length === 1) {
          if (/^[A-Z]/.test(parts[0])) return `an instance of ${parts[0]}`;
          return `of type ${parts[0]}`;
        }
        const classParts = parts.filter((p) => /^[A-Z]/.test(p));
        const typeParts = parts.filter((p) => !/^[A-Z]/.test(p));
        if (typeParts.length === 1 && classParts.length > 0) {
          const specialArrayLike = classParts.find((p) => p.toLowerCase() === 'array-like object');
          const filteredClasses = specialArrayLike ? classParts.filter((p) => p !== specialArrayLike) : classParts;
          const cls = filteredClasses.length > 0
            ? (filteredClasses.length === 1 ? filteredClasses[0] : joinWithOr(filteredClasses))
            : '';
          if (specialArrayLike) {
            if (cls) {
              return `of type ${typeParts[0]} or an instance of ${cls} or an ${specialArrayLike}`;
            }
            return `of type ${typeParts[0]} or an ${specialArrayLike}`;
          }
          return `of type ${typeParts[0]} or an instance of ${cls}`;
        }
        if (typeParts.length > 1 && classParts.length > 0) {
          const types = typeParts.length === 2
            ? `${typeParts[0]} or ${typeParts[1]}`
            : `${typeParts.slice(0, -1).join(', ')} or ${typeParts[typeParts.length - 1]}`;
          const cls = classParts.length === 1 ? classParts[0] : joinWithOr(classParts);
          return `one of type ${types} or an instance of ${cls}`;
        }
        if (parts.every((p) => /^[A-Z]/.test(p))) {
          return `an instance of ${joinWithOr(parts)}`;
        }
        if (classParts.length === 0 && typeParts.length >= 2) {
          const [first, ...rest] = typeParts;
          if (rest.length === 1) return `of type ${first} or ${rest[0]}`;
          return `of type ${first} or one of ${rest.slice(0, -1).join(', ')} or ${rest[rest.length - 1]}`;
        }
        return `one of type ${parts.slice(0, -1).join(', ')} or ${parts[parts.length - 1]}`;
      }
      const s = String(exp);
      if (s === 'Object') return 'of type object';
      if (s === 'Function') return 'of type function';
      return `of type ${s}`;
    }
    let received;
    if (actual == null) {
      received = ` Received ${actual}`;
    } else if (typeof actual === 'function') {
      received = ` Received function ${actual.name || '<anonymous>'}`;
    } else if (typeof actual === 'object') {
      if (actual.constructor?.name) {
        received = ` Received an instance of ${actual.constructor.name}`;
      } else {
        received = ` Received ${typeof actual}`;
      }
    } else {
      const str = String(actual);
      let shown = str.slice(0, 25);
      if (str.length > 25) {
        shown += '...';
      }
      if (typeof actual === 'string') {
        shown = `'${shown}'`;
      }
      received = ` Received type ${typeof actual} (${shown})`;
    }
    super(`${formatName(name)} must be ${formatExpected(expected)}.${received}`);
    this.code = 'ERR_INVALID_ARG_TYPE';
  }
}

class ERR_OUT_OF_RANGE extends RangeError {
  constructor(name, range, actual) {
    function formatReceived(v) {
      if (typeof v === 'bigint') {
        const digits = String(v < 0n ? -v : v).replace(/(\d)(?=(\d{3})+(?!\d))/g, '$1_');
        return `${v < 0n ? '-' : ''}${digits}n`;
      }
      if (name === 'value' &&
          typeof range === 'string' &&
          range.includes('2 **') &&
          typeof v === 'number' &&
          Number.isInteger(v) &&
          Number.isFinite(v) &&
          Math.abs(v) >= 1000000000) {
        const s = String(Math.abs(v)).replace(/(\d)(?=(\d{3})+(?!\d))/g, '$1_');
        return v < 0 ? `-${s}` : s;
      }
      return String(v);
    }
    super(`The value of "${name}" is out of range. It must be ${range}. Received ${formatReceived(actual)}`);
    this.code = 'ERR_OUT_OF_RANGE';
  }
}

class ERR_BUFFER_OUT_OF_BOUNDS extends RangeError {
  constructor(name = undefined) {
    super(name ? `"${name}" is outside of buffer bounds` : 'Attempt to access memory outside buffer bounds');
    this.code = 'ERR_BUFFER_OUT_OF_BOUNDS';
  }
}

class ERR_UNKNOWN_ENCODING extends TypeError {
  constructor(encoding) {
    super(`Unknown encoding: ${encoding}`);
    this.code = 'ERR_UNKNOWN_ENCODING';
  }
}

class ERR_INVALID_THIS extends TypeError {
  constructor(type) {
    super(`Value of "this" must be of type ${type}`);
    this.code = 'ERR_INVALID_THIS';
  }
}

class ERR_INVALID_BUFFER_SIZE extends RangeError {
  constructor(name = '16-bits') {
    super(`Buffer size must be a multiple of ${name}`);
    this.code = 'ERR_INVALID_BUFFER_SIZE';
  }
}

class ERR_FALSY_VALUE_REJECTION extends TypeError {
  constructor(reason) {
    super(`Promise was rejected with a falsy value: ${reason}`);
    this.code = 'ERR_FALSY_VALUE_REJECTION';
    this.reason = reason;
  }
}
ERR_FALSY_VALUE_REJECTION.HideStackFramesError = ERR_FALSY_VALUE_REJECTION;

class ERR_MISSING_ARGS extends TypeError {
  constructor(...args) {
    let names = '';
    if (args.length === 1 && Array.isArray(args[0])) {
      names = args[0].map((a) => `"${a}"`).join(' or ');
    } else if (args.length === 1) {
      names = `"${args[0]}"`;
    } else if (args.length === 2) {
      names = `"${args[0]}" and "${args[1]}"`;
    } else {
      names = args.slice(0, -1).map((a) => `"${a}"`).join(', ');
      names += `, and "${args[args.length - 1]}"`;
    }
    const noun = args.length === 1 ? 'argument' : 'arguments';
    super(`The ${names} ${noun} must be specified`);
    this.code = 'ERR_MISSING_ARGS';
  }
}

class ERR_SYSTEM_ERROR extends Error {
  constructor(info) {
    const ctx = info || {};
    const syscall = ctx.syscall || 'unknown';
    const code = ctx.code || 'UNKNOWN';
    const message = ctx.message || 'unknown error';
    super(`A system error occurred: ${syscall} returned ${code} (${message})`);
    this.name = 'SystemError';
    this.code = 'ERR_SYSTEM_ERROR';
    this.info = ctx;
    if (ctx.errno !== undefined) this.errno = ctx.errno;
    if (ctx.syscall !== undefined) this.syscall = ctx.syscall;
  }
}
ERR_SYSTEM_ERROR.HideStackFramesError = ERR_SYSTEM_ERROR;

class ERR_UNHANDLED_ERROR extends Error {
  constructor(context) {
    const msg = context === undefined ?
      'Unhandled error.' :
      `Unhandled error. (${String(context)})`;
    super(msg);
    this.code = 'ERR_UNHANDLED_ERROR';
    this.context = context;
  }
}

const kEnhanceStackBeforeInspector = Symbol('kEnhanceStackBeforeInspector');

class ERR_INVALID_URI extends URIError {
  constructor() {
    super('URI malformed');
    this.code = 'ERR_INVALID_URI';
  }
}

class ERR_INVALID_URL extends TypeError {
  constructor(input, base = undefined) {
    const b = base !== undefined ? ` with base ${String(base)}` : '';
    super(`Invalid URL: ${String(input)}${b}`);
    this.code = 'ERR_INVALID_URL';
    this.input = input;
    if (base !== undefined) this.base = base;
  }
}

class ERR_INVALID_FILE_URL_PATH extends TypeError {
  constructor(reason, input) {
    const text = reason ? `File URL path ${reason}` : 'File URL path is invalid';
    super(text);
    this.code = 'ERR_INVALID_FILE_URL_PATH';
    if (input !== undefined) this.input = input;
  }
}

class ERR_INVALID_FILE_URL_HOST extends TypeError {
  constructor(platform) {
    super(`File URL host must be "localhost" or empty on ${platform}`);
    this.code = 'ERR_INVALID_FILE_URL_HOST';
  }
}

class ERR_INVALID_URL_SCHEME extends TypeError {
  constructor(expected) {
    super(`The URL must be of scheme ${expected}`);
    this.code = 'ERR_INVALID_URL_SCHEME';
  }
}

class ERR_STREAM_CANNOT_PIPE extends Error {
  constructor() {
    super('Cannot pipe, not readable');
    this.code = 'ERR_STREAM_CANNOT_PIPE';
  }
}

class ERR_STREAM_DESTROYED extends Error {
  constructor(name = 'stream') {
    super(`Cannot call write after a stream was destroyed`);
    this.code = 'ERR_STREAM_DESTROYED';
    this.name = `${name}Error`;
  }
}

class ERR_STREAM_PREMATURE_CLOSE extends Error {
  constructor() {
    super('Premature close');
    this.code = 'ERR_STREAM_PREMATURE_CLOSE';
  }
}

class ERR_STREAM_UNABLE_TO_PIPE extends Error {
  constructor() {
    super('Unable to pipe from a closed stream');
    this.code = 'ERR_STREAM_UNABLE_TO_PIPE';
  }
}

class ERR_INVALID_RETURN_VALUE extends TypeError {
  constructor(input, name, value) {
    super(`Expected ${input} to be returned from the "${name}" function but got ${value}`);
    this.code = 'ERR_INVALID_RETURN_VALUE';
  }
}

class ERR_STREAM_WRITE_AFTER_END extends Error {
  constructor() {
    super('write after end');
    this.code = 'ERR_STREAM_WRITE_AFTER_END';
  }
}

class ERR_STREAM_NULL_VALUES extends TypeError {
  constructor() {
    super('May not write null values to stream');
    this.code = 'ERR_STREAM_NULL_VALUES';
  }
}

function mapDnsCode(code) {
  if (typeof code === 'string') return code;
  if (code === -3001) return 'EAI_MEMORY';
  if (code === -2) return 'ENOENT';
  if (code === -78) return 'ENOTFOUND';
  if (code === -12) return 'ENOMEM';
  if (code === -89) return 'ECANCELLED';
  if (code === -60 || code === -110) return 'ETIMEOUT';
  return String(code);
}

function DNSException(code, syscall, hostname) {
  const errCode = mapDnsCode(code);
  const hostPart = hostname ? ` ${hostname}` : '';
  const err = new Error(`${syscall} ${errCode}${hostPart}`);
  if (typeof Error.captureStackTrace === 'function') {
    Error.captureStackTrace(err, DNSException);
  }
  if (typeof err.stack === 'string') {
    const lines = err.stack.split('\n');
    if (lines.length > 1) lines[1] = '    at Object.<anonymous> (<anonymous>)';
    err.stack = lines.join('\n');
  }
  err.code = errCode;
  err.errno = errCode;
  err.syscall = syscall;
  if (hostname !== undefined) err.hostname = hostname;
  return err;
}

class ERR_DNS_SET_SERVERS_FAILED extends Error {
  constructor(message, servers) {
    super(`c-ares failed to set servers: "${message}" [${servers}]`);
    this.code = 'ERR_DNS_SET_SERVERS_FAILED';
  }
}

class ERR_INVALID_IP_ADDRESS extends TypeError {
  constructor(address) {
    super(`Invalid IP address: ${address}`);
    this.code = 'ERR_INVALID_IP_ADDRESS';
  }
}

class ERR_SOCKET_BAD_PORT extends RangeError {
  constructor(name, port, allowZero = true) {
    const op = allowZero ? '>=' : '>';
    super(`${name} should be ${op} 0 and < 65536. Received ${String(port)}`);
    this.code = 'ERR_SOCKET_BAD_PORT';
  }
}

class ERR_INVALID_ADDRESS_FAMILY extends RangeError {
  constructor(addressType, host, port) {
    super(`Invalid address family: ${addressType} ${host}:${port}`);
    this.code = 'ERR_INVALID_ADDRESS_FAMILY';
    this.host = host;
    this.port = port;
  }
}

class ERR_IP_BLOCKED extends Error {
  constructor(ip) {
    super(`IP(${ip}) is blocked by net.BlockList`);
    this.code = 'ERR_IP_BLOCKED';
  }
}

class ERR_SERVER_ALREADY_LISTEN extends Error {
  constructor() {
    super('Listen method has been called more than once without closing.');
    this.code = 'ERR_SERVER_ALREADY_LISTEN';
  }
}

class ERR_SOCKET_CLOSED extends Error {
  constructor() {
    super('Socket is closed');
    this.code = 'ERR_SOCKET_CLOSED';
  }
}

class ERR_SOCKET_CLOSED_BEFORE_CONNECTION extends Error {
  constructor() {
    super('Socket closed before the connection was established');
    this.code = 'ERR_SOCKET_CLOSED_BEFORE_CONNECTION';
  }
}

class NodeAggregateError extends AggregateError {
  constructor(errors, message, code = undefined) {
    super(errors, message);
    this.code = code;
  }
}

class ErrnoException extends Error {
  constructor(err, syscall, original) {
    let code = 'UNKNOWN';
    if (typeof process?.binding === 'function') {
      try {
        code = process.binding('uv').errname(err);
      } catch {}
    }
    if (code === 'UNKNOWN' && typeof err === 'number') {
      if (err === -22) code = 'EINVAL';
      if (err === -17) code = 'EEXIST';
      if (err === -98) code = 'EADDRINUSE';
      if (err === -111) code = 'ECONNREFUSED';
      if (err === -9) code = 'EBADF';
    }
    super(original ? `${syscall} ${code} ${original}` : `${syscall} ${code}`);
    this.errno = err;
    this.code = code;
    this.syscall = syscall;
  }
}

class ExceptionWithHostPort extends Error {
  constructor(err, syscall, address, port, additional) {
    let code = 'UNKNOWN';
    if (typeof process?.binding === 'function') {
      try {
        code = process.binding('uv').errname(err);
      } catch {}
    }
    if (code === 'UNKNOWN' && err === -17) code = 'EEXIST';
    let details = '';
    if (port && port > 0) details = ` ${address}:${port}`;
    else if (address) details = ` ${address}`;
    if (additional) details += ` - Local (${additional})`;
    super(`${syscall} ${code}${details}`);
    this.errno = err;
    this.code = code;
    this.syscall = syscall;
    this.address = address;
    if (port) this.port = port;
  }
}

class ERR_SOCKET_ALREADY_BOUND extends Error {
  constructor() {
    super('Socket is already bound');
    this.code = 'ERR_SOCKET_ALREADY_BOUND';
  }
}

class ERR_SOCKET_BAD_BUFFER_SIZE extends TypeError {
  constructor() {
    super('Buffer size must be a positive integer');
    this.code = 'ERR_SOCKET_BAD_BUFFER_SIZE';
  }
}

class ERR_SOCKET_BUFFER_SIZE extends Error {
  constructor(ctx) {
    const code = (ctx && ctx.code) || 'UNKNOWN';
    const syscall = (ctx && ctx.syscall) || 'uv_buffer_size';
    const message = (ctx && ctx.message) || 'unknown error';
    super(`Could not get or set buffer size: ${syscall} returned ${code} (${message})`);
    this.name = 'SystemError';
    this.code = 'ERR_SOCKET_BUFFER_SIZE';
    this.info = ctx && typeof ctx === 'object' ? ctx : {};
    let errnoValue = this.info.errno;
    let syscallValue = this.info.syscall ?? syscall;
    Object.defineProperty(this, 'errno', {
      __proto__: null,
      configurable: true,
      enumerable: true,
      get() { return errnoValue; },
      set(v) { errnoValue = v; },
    });
    Object.defineProperty(this, 'syscall', {
      __proto__: null,
      configurable: true,
      enumerable: true,
      get() { return syscallValue; },
      set(v) { syscallValue = v; },
    });
    const inspectSymbol = Symbol.for('nodejs.util.inspect.custom');
    this[inspectSymbol] = () => (
      `SystemError [ERR_SOCKET_BUFFER_SIZE]: ${this.message}\n` +
      "  code: 'ERR_SOCKET_BUFFER_SIZE',\n" +
      '  info: {\n' +
      `    errno: ${this.info.errno},\n` +
      `    code: '${this.info.code}',\n` +
      `    message: '${this.info.message}',\n` +
      `    syscall: '${this.info.syscall}'\n` +
      '  },\n' +
      `  errno: [Getter/Setter: ${this.errno}],\n` +
      `  syscall: [Getter/Setter: '${this.syscall}']\n` +
      '}'
    );
  }

}

class ERR_SOCKET_BAD_TYPE extends TypeError {
  constructor() {
    super('Bad socket type specified. Valid types are: udp4, udp6');
    this.code = 'ERR_SOCKET_BAD_TYPE';
  }
}

class ERR_INVALID_FD_TYPE extends TypeError {
  constructor(type) {
    super(`Unsupported fd type: ${type}`);
    this.code = 'ERR_INVALID_FD_TYPE';
  }
}

class ERR_INVALID_FD extends RangeError {
  constructor(fd) {
    super(`"fd" must be a positive integer: ${String(fd)}`);
    this.code = 'ERR_INVALID_FD';
  }
}

class ERR_TTY_INIT_FAILED extends Error {
  constructor(ctx) {
    const info = (ctx && typeof ctx === 'object') ? ctx : {};
    const code = info.code || 'EINVAL';
    const message = info.message || 'invalid argument';
    super(`TTY initialization failed: uv_tty_init returned ${code} (${message})`);
    this.name = 'SystemError';
    this.code = 'ERR_TTY_INIT_FAILED';
    this.info = info;
  }
}

class ERR_SOCKET_DGRAM_IS_CONNECTED extends Error {
  constructor() {
    super('Already connected');
    this.code = 'ERR_SOCKET_DGRAM_IS_CONNECTED';
  }
}

class ERR_SOCKET_DGRAM_NOT_CONNECTED extends Error {
  constructor() {
    super('Not connected');
    this.code = 'ERR_SOCKET_DGRAM_NOT_CONNECTED';
  }
}

class ERR_SOCKET_DGRAM_NOT_RUNNING extends Error {
  constructor() {
    super('Not running');
    this.code = 'ERR_SOCKET_DGRAM_NOT_RUNNING';
  }
}

class ERR_UNESCAPED_CHARACTERS extends TypeError {
  constructor(field = 'Request path') {
    super(`${field} contains unescaped characters`);
    this.code = 'ERR_UNESCAPED_CHARACTERS';
  }
}

class ERR_INVALID_PROTOCOL extends TypeError {
  constructor(protocol, expected) {
    super(`Protocol "${protocol}" not supported. Expected "${expected}"`);
    this.code = 'ERR_INVALID_PROTOCOL';
  }
}

class ERR_INVALID_HTTP_TOKEN extends TypeError {
  constructor(name, value) {
    super(`${name} must be a valid HTTP token ["${String(value)}"]`);
    this.code = 'ERR_INVALID_HTTP_TOKEN';
  }
}
ERR_INVALID_HTTP_TOKEN.HideStackFramesError = ERR_INVALID_HTTP_TOKEN;

class ERR_HTTP_INVALID_HEADER_VALUE extends TypeError {
  constructor(value, name) {
    super(`Invalid value "${String(value)}" for header "${String(name)}"`);
    this.code = 'ERR_HTTP_INVALID_HEADER_VALUE';
  }
}
ERR_HTTP_INVALID_HEADER_VALUE.HideStackFramesError = ERR_HTTP_INVALID_HEADER_VALUE;

class ERR_INVALID_CHAR extends TypeError {
  constructor(field, value) {
    super(`Invalid character in ${field || 'content'} ["${String(value)}"]`);
    this.code = 'ERR_INVALID_CHAR';
  }
}
ERR_INVALID_CHAR.HideStackFramesError = ERR_INVALID_CHAR;

class ERR_METHOD_NOT_IMPLEMENTED extends Error {
  constructor(name) {
    super(`The ${name} method is not implemented`);
    this.code = 'ERR_METHOD_NOT_IMPLEMENTED';
  }
}
ERR_METHOD_NOT_IMPLEMENTED.HideStackFramesError = ERR_METHOD_NOT_IMPLEMENTED;

class ERR_HTTP_BODY_NOT_ALLOWED extends Error {
  constructor() {
    super('Adding content for this request method or response status is not allowed.');
    this.code = 'ERR_HTTP_BODY_NOT_ALLOWED';
  }
}
ERR_HTTP_BODY_NOT_ALLOWED.HideStackFramesError = ERR_HTTP_BODY_NOT_ALLOWED;

class ERR_HTTP_CONTENT_LENGTH_MISMATCH extends Error {
  constructor(actual, expected) {
    super(`Response body's content-length of ${actual} byte(s) does not match the content-length of ${expected} byte(s) set in header`);
    this.code = 'ERR_HTTP_CONTENT_LENGTH_MISMATCH';
  }
}
ERR_HTTP_CONTENT_LENGTH_MISMATCH.HideStackFramesError = ERR_HTTP_CONTENT_LENGTH_MISMATCH;

class ERR_HTTP_HEADERS_SENT extends Error {
  constructor(verb = 'set') {
    super(`Cannot ${verb} headers after they are sent to the client`);
    this.code = 'ERR_HTTP_HEADERS_SENT';
  }
}

class ERR_HTTP_SOCKET_ASSIGNED extends Error {
  constructor() {
    super('ServerResponse has an already assigned socket');
    this.code = 'ERR_HTTP_SOCKET_ASSIGNED';
  }
}

class ERR_IPC_CHANNEL_CLOSED extends Error {
  constructor() {
    super('Channel closed');
    this.code = 'ERR_IPC_CHANNEL_CLOSED';
  }
}

ERR_INVALID_ARG_TYPE.HideStackFramesError = ERR_INVALID_ARG_TYPE;
ERR_INVALID_ARG_VALUE.HideStackFramesError = ERR_INVALID_ARG_VALUE;
ERR_OUT_OF_RANGE.HideStackFramesError = ERR_OUT_OF_RANGE;
ERR_STREAM_NULL_VALUES.HideStackFramesError = ERR_STREAM_NULL_VALUES;
ERR_STREAM_WRITE_AFTER_END.HideStackFramesError = ERR_STREAM_WRITE_AFTER_END;

function aggregateTwoErrors(innerError, outerError) {
  if (!innerError) return outerError;
  if (!outerError) return innerError;
  const err = new Error(outerError.message);
  err.code = outerError.code;
  err.cause = innerError;
  return err;
}

function hideStackFrames(fn) {
  return fn;
}

function UVExceptionWithHostPort(err, syscall, address, port, additional) {
  return new ExceptionWithHostPort(err, syscall, address, port, additional);
}

function genericNodeError(message, errorProperties) {
  const err = new Error(message);
  if (errorProperties && typeof errorProperties === 'object') {
    Object.assign(err, errorProperties);
  }
  return err;
}

function isStackOverflowError(err) {
  if (!(err instanceof Error)) return false;
  return String(err.message || '').includes('Maximum call stack size exceeded');
}

module.exports = {
  AbortError,
  DNSException,
  ErrnoException,
  ExceptionWithHostPort,
  UVExceptionWithHostPort,
  NodeAggregateError,
  hideStackFrames,
  isStackOverflowError,
  aggregateTwoErrors,
  genericNodeError,
  codes: {
    ERR_INVALID_ARG_VALUE,
    ERR_INVALID_CURSOR_POS,
    ERR_USE_AFTER_CLOSE,
    ERR_INVALID_ARG_TYPE,
    ERR_OUT_OF_RANGE,
    ERR_BUFFER_OUT_OF_BOUNDS,
    ERR_UNKNOWN_ENCODING,
    ERR_INVALID_THIS,
    ERR_INVALID_BUFFER_SIZE,
    ERR_FALSY_VALUE_REJECTION,
    ERR_MISSING_ARGS,
    ERR_SYSTEM_ERROR,
    ERR_UNHANDLED_ERROR,
    ERR_INVALID_URI,
    ERR_INVALID_URL,
    ERR_INVALID_FILE_URL_PATH,
    ERR_INVALID_FILE_URL_HOST,
    ERR_INVALID_URL_SCHEME,
    ERR_STREAM_CANNOT_PIPE,
    ERR_STREAM_DESTROYED,
    ERR_STREAM_PREMATURE_CLOSE,
    ERR_STREAM_UNABLE_TO_PIPE,
    ERR_INVALID_RETURN_VALUE,
    ERR_STREAM_WRITE_AFTER_END,
    ERR_STREAM_NULL_VALUES,
    ERR_DNS_SET_SERVERS_FAILED,
    ERR_INVALID_ADDRESS_FAMILY,
    ERR_INVALID_IP_ADDRESS,
    ERR_IP_BLOCKED,
    ERR_SERVER_ALREADY_LISTEN,
    ERR_SOCKET_BAD_PORT,
    ERR_SOCKET_ALREADY_BOUND,
    ERR_SOCKET_BAD_BUFFER_SIZE,
    ERR_SOCKET_BUFFER_SIZE,
    ERR_SOCKET_CLOSED,
    ERR_SOCKET_CLOSED_BEFORE_CONNECTION,
    ERR_SOCKET_BAD_TYPE,
    ERR_INVALID_FD,
    ERR_INVALID_FD_TYPE,
    ERR_TTY_INIT_FAILED,
    ERR_SOCKET_DGRAM_IS_CONNECTED,
    ERR_SOCKET_DGRAM_NOT_CONNECTED,
    ERR_SOCKET_DGRAM_NOT_RUNNING,
    ERR_UNESCAPED_CHARACTERS,
    ERR_INVALID_PROTOCOL,
    ERR_INVALID_HTTP_TOKEN,
    ERR_HTTP_INVALID_HEADER_VALUE,
    ERR_INVALID_CHAR,
    ERR_METHOD_NOT_IMPLEMENTED,
    ERR_HTTP_BODY_NOT_ALLOWED,
    ERR_HTTP_CONTENT_LENGTH_MISMATCH,
    ERR_HTTP_HEADERS_SENT,
    ERR_HTTP_SOCKET_ASSIGNED,
    ERR_IPC_CHANNEL_CLOSED,
  },
  kEnhanceStackBeforeInspector,
};
