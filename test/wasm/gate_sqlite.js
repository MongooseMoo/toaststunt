const fs = require('fs');
const path = require('path');

const TIMEOUT = 90000;
const wasmDir = path.resolve(__dirname, '../../build-wasm');
let output = '';
let serverReady = false;
let loginVerified = false;
let connId = -1;
let Module;
let phase = 'boot'; // boot -> connect -> check_builtin -> open_db -> query -> done

const timer = setTimeout(() => {
  console.error('TIMEOUT: No response received within ' + TIMEOUT + 'ms');
  console.error('State: phase=' + phase + ' serverReady=' + serverReady +
                ' loginVerified=' + loginVerified);
  console.error('Output so far:', output);
  process.exit(1);
}, TIMEOUT);

const ToastStuntModule = require(path.join(wasmDir, 'moo.js'));

// Use toastcore.db which has eval verb support
const dbPath = path.resolve(__dirname, '../../toastcore.db');
const dbText = fs.readFileSync(dbPath, 'utf-8').replace(/\r\n/g, '\n');
const dbData = new TextEncoder().encode(dbText);

let builtinResult = null;
let openResult = null;
let queryResult = null;

function handlePrintOutput(text) {
  // Phase: waiting for connect confirmation
  if (phase === 'connect') {
    if (text.includes('*** Connected ***')) {
      loginVerified = true;
      console.log('[gate_sqlite] Connected as wizard');
      phase = 'check_builtin_pending';
      setTimeout(testBuiltinExists, 2000);
      return;
    }
  }

  // Phase: check if sqlite_handles builtin exists
  if (phase === 'check_builtin') {
    // sqlite_handles() returns {} (empty list) when no handles open
    if (text.includes('{}')) {
      builtinResult = 'PASS';
      console.log('[gate_sqlite] Phase CHECK_BUILTIN PASSED: sqlite_handles() returned {}');
      phase = 'open_db_pending';
      setTimeout(testOpenDb, 2000);
      return;
    }
    if (text.includes('E_VERBNF') || text.includes('Unknown built-in')) {
      builtinResult = 'FAIL';
      console.error('[gate_sqlite] Phase CHECK_BUILTIN FAILED: sqlite_handles not registered');
      console.error('GATE_SQLITE_FAIL');
      clearTimeout(timer);
      process.exit(1);
    }
  }

  // Phase: open in-memory SQLite database
  if (phase === 'open_db') {
    // sqlite_open returns an integer handle (e.g. "=> 1" or just "1")
    if (text.match(/^(=> )?[0-9]+$/)) {
      openResult = 'PASS';
      console.log('[gate_sqlite] Phase OPEN_DB PASSED: got handle ' + text.trim());
      phase = 'query_pending';
      setTimeout(testQuery, 2000);
      return;
    }
    if (text.includes('E_PERM') || text.includes('E_INVARG') || text.includes('E_QUOTA')) {
      openResult = 'FAIL';
      console.error('[gate_sqlite] Phase OPEN_DB FAILED: ' + text);
      console.error('GATE_SQLITE_FAIL');
      clearTimeout(timer);
      process.exit(1);
    }
  }

  // Phase: execute a query
  if (phase === 'query') {
    // sqlite_execute on "SELECT 1+1" should return {{2}} or similar containing "2"
    if (text.match(/\{.*2.*\}/)) {
      queryResult = 'PASS';
      console.log('[gate_sqlite] Phase QUERY PASSED: SELECT 1+1 returned: ' + text);
      allDone();
      return;
    }
    if (text.includes('E_PERM') || text.includes('E_INVARG') || text.includes('E_TYPE')) {
      queryResult = 'FAIL';
      console.error('[gate_sqlite] Phase QUERY FAILED: ' + text);
      console.error('GATE_SQLITE_FAIL');
      clearTimeout(timer);
      process.exit(1);
    }
  }
}

function allDone() {
  if (builtinResult === 'PASS' && openResult === 'PASS' && queryResult === 'PASS') {
    phase = 'done';
    console.log('[gate_sqlite] ALL PHASES PASSED');
    console.log('GATE_SQLITE_PASS');
    clearTimeout(timer);
    process.exit(0);
  }
}

function doConnect() {
  phase = 'connect';
  console.log('[gate_sqlite] Sending: connect Wizard');
  Module.ccall('wasm_inject_input', null, ['number', 'string'], [connId, 'connect Wizard']);
}

function testBuiltinExists() {
  phase = 'check_builtin';
  // sqlite_handles() should return {} (empty list) if registered
  const cmd = '; sqlite_handles()';
  console.log('[gate_sqlite] Injecting builtin check: ' + cmd);
  Module.ccall('wasm_inject_input', null, ['number', 'string'], [connId, cmd]);

  // Fallback poll
  setTimeout(() => {
    if (phase !== 'check_builtin') return;
    const outBuf = Module.ccall('wasm_get_output', 'string', ['number'], [connId]);
    console.log('[gate_sqlite] Builtin check fallback poll: ' + JSON.stringify(outBuf));
    if (outBuf) {
      outBuf.split('\n').forEach(line => handlePrintOutput(line));
    }
  }, 10000);
}

function testOpenDb() {
  phase = 'open_db';
  const cmd = '; sqlite_open(":memory:")';
  console.log('[gate_sqlite] Injecting open test: ' + cmd);
  Module.ccall('wasm_inject_input', null, ['number', 'string'], [connId, cmd]);

  // Fallback poll
  setTimeout(() => {
    if (phase !== 'open_db') return;
    const outBuf = Module.ccall('wasm_get_output', 'string', ['number'], [connId]);
    console.log('[gate_sqlite] Open DB fallback poll: ' + JSON.stringify(outBuf));
    if (outBuf) {
      outBuf.split('\n').forEach(line => handlePrintOutput(line));
    }
  }, 10000);
}

function testQuery() {
  phase = 'query';
  // sqlite_execute(handle, query, args) - handle 1, simple SELECT, empty args list
  const cmd = '; sqlite_execute(1, "SELECT 1+1 AS result", {})';
  console.log('[gate_sqlite] Injecting query test: ' + cmd);
  Module.ccall('wasm_inject_input', null, ['number', 'string'], [connId, cmd]);

  // Fallback poll
  setTimeout(() => {
    if (phase !== 'query') return;
    const outBuf = Module.ccall('wasm_get_output', 'string', ['number'], [connId]);
    console.log('[gate_sqlite] Query fallback poll: ' + JSON.stringify(outBuf));
    if (outBuf) {
      outBuf.split('\n').forEach(line => handlePrintOutput(line));
    }
  }, 15000);
}

function onServerReady() {
  if (serverReady) return;
  serverReady = true;
  console.log('[gate_sqlite] Server is ready, scheduling connection creation...');
  setTimeout(() => {
    try {
      console.log('[gate_sqlite] Creating virtual connection...');
      connId = Module._wasm_new_connection();
      console.log('[gate_sqlite] Connection created, id=' + connId);
      setTimeout(doConnect, 2000);
    } catch(e) {
      console.error('[gate_sqlite] Connection error:', e);
    }
  }, 1000);
}

ToastStuntModule({
  print: (text) => {
    output += text + '\n';
    console.log('[out] ' + text);
    handlePrintOutput(text);
  },
  printErr: (text) => {
    output += '[err] ' + text + '\n';
    console.error('[err] ' + text);
    if (text.includes('LISTEN:') && !serverReady && Module) {
      onServerReady();
    }
  },
  locateFile: (p) => path.join(wasmDir, p),
  preRun: [(mod) => {
    mod.FS.writeFile('/toastcore.db', dbData);
    mod.FS.writeFile('/toastcore.db.new', new Uint8Array(0));
  }],
  arguments: ['/toastcore.db', '/toastcore.db.new'],
  noExitRuntime: true
}).then((m) => {
  Module = m;
  console.log('[gate_sqlite] Module loaded');
  if (!serverReady && output.includes('LISTEN:')) {
    onServerReady();
  }
}).catch((e) => {
  if (e.status !== 0) {
    console.error('Module error:', e);
    clearTimeout(timer);
    process.exit(1);
  }
});
