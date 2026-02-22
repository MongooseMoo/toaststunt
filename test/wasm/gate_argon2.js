const fs = require('fs');
const path = require('path');

const TIMEOUT = 120000;
const wasmDir = path.resolve(__dirname, '../../build-wasm');
let output = '';
let serverReady = false;
let loginVerified = false;
let connId = -1;
let Module;
let phase = 'boot'; // boot -> login -> connect -> argon2_hash -> argon2_verify -> done

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

let argon2HashResult = null;
let argon2VerifyResult = null;
let capturedHash = '';

function handlePrintOutput(text) {
  // Phase: waiting for connect confirmation
  if (phase === 'connect') {
    if (text.includes('*** Connected ***')) {
      loginVerified = true;
      console.log('[gate_argon2] Connected as wizard');
      phase = 'argon2_hash_pending';
      setTimeout(testArgon2Hash, 2000);
      return;
    }
  }

  // Phase: argon2 hash test
  if (phase === 'argon2_hash') {
    // argon2id encoded hash starts with $argon2id$
    const hashMatch = text.match(/(\$argon2id\$[^\s"]+)/);
    if (hashMatch) {
      capturedHash = hashMatch[1];
      argon2HashResult = 'PASS';
      console.log('[gate_argon2] Phase ARGON2_HASH PASSED: returned hash: ' + capturedHash.substring(0, 60) + '...');
      phase = 'argon2_verify_pending';
      setTimeout(testArgon2Verify, 2000);
      return;
    }
    if (text.includes('E_PERM')) {
      argon2HashResult = 'FAIL';
      console.error('[gate_argon2] Phase ARGON2_HASH FAILED: returned E_PERM');
      console.error('GATE_ARGON2_FAIL');
      clearTimeout(timer);
      process.exit(1);
    }
    if (text.includes('E_VERBNF')) {
      argon2HashResult = 'FAIL';
      console.error('[gate_argon2] Phase ARGON2_HASH FAILED: returned E_VERBNF (builtin not registered)');
      console.error('GATE_ARGON2_FAIL');
      clearTimeout(timer);
      process.exit(1);
    }
    if (text.includes('E_INVIND') || text.includes('E_INVARG')) {
      argon2HashResult = 'FAIL';
      console.error('[gate_argon2] Phase ARGON2_HASH FAILED: ' + text);
      console.error('GATE_ARGON2_FAIL');
      clearTimeout(timer);
      process.exit(1);
    }
  }

  // Phase: argon2 verify test
  if (phase === 'argon2_verify') {
    // argon2_verify returns 1 on success, 0 on failure
    if (text.match(/^1$/m) || text.includes('=> 1')) {
      argon2VerifyResult = 'PASS';
      console.log('[gate_argon2] Phase ARGON2_VERIFY PASSED: returned 1');
      allDone();
      return;
    }
    if (text.match(/^0$/m) || text.includes('=> 0')) {
      argon2VerifyResult = 'FAIL';
      console.error('[gate_argon2] Phase ARGON2_VERIFY FAILED: returned 0 (verification failed)');
      console.error('GATE_ARGON2_FAIL');
      clearTimeout(timer);
      process.exit(1);
    }
    if (text.includes('E_PERM')) {
      argon2VerifyResult = 'FAIL';
      console.error('[gate_argon2] Phase ARGON2_VERIFY FAILED: returned E_PERM');
      console.error('GATE_ARGON2_FAIL');
      clearTimeout(timer);
      process.exit(1);
    }
    if (text.includes('E_VERBNF')) {
      argon2VerifyResult = 'FAIL';
      console.error('[gate_argon2] Phase ARGON2_VERIFY FAILED: returned E_VERBNF');
      console.error('GATE_ARGON2_FAIL');
      clearTimeout(timer);
      process.exit(1);
    }
  }
}

function allDone() {
  if (argon2HashResult === 'PASS' && argon2VerifyResult === 'PASS') {
    phase = 'done';
    console.log('[gate_argon2] ALL PHASES PASSED');
    console.log('GATE_ARGON2_PASS');
    clearTimeout(timer);
    process.exit(0);
  }
}

function doConnect() {
  phase = 'connect';
  console.log('[gate_argon2] Sending: connect Wizard');
  Module.ccall('wasm_inject_input', null, ['number', 'string'], [connId, 'connect Wizard']);
}

function testArgon2Hash() {
  phase = 'argon2_hash';
  // argon2(password, salt, iterations, memory_kb, parallelism)
  // Use small parameters for WASM: 2 iterations, 1024 KB memory, 1 thread
  const cmd = '; argon2("password", "saltsalt", 2, 1024, 1)';
  console.log('[gate_argon2] Injecting argon2 hash test: ' + cmd);
  Module.ccall('wasm_inject_input', null, ['number', 'string'], [connId, cmd]);

  // Fallback poll
  setTimeout(() => {
    if (phase !== 'argon2_hash') return;
    const outBuf = Module.ccall('wasm_get_output', 'string', ['number'], [connId]);
    console.log('[gate_argon2] argon2_hash fallback poll: ' + JSON.stringify(outBuf));
    if (outBuf) {
      outBuf.split('\n').forEach(line => handlePrintOutput(line));
    }
  }, 15000);
}

function testArgon2Verify() {
  phase = 'argon2_verify';
  // Verify the hash we captured against the same password
  const cmd = '; argon2_verify("' + capturedHash + '", "password")';
  console.log('[gate_argon2] Injecting argon2_verify test: ' + cmd);
  Module.ccall('wasm_inject_input', null, ['number', 'string'], [connId, cmd]);

  // Fallback poll
  setTimeout(() => {
    if (phase !== 'argon2_verify') return;
    const outBuf = Module.ccall('wasm_get_output', 'string', ['number'], [connId]);
    console.log('[gate_argon2] argon2_verify fallback poll: ' + JSON.stringify(outBuf));
    if (outBuf) {
      outBuf.split('\n').forEach(line => handlePrintOutput(line));
    }
  }, 15000);
}

function onServerReady() {
  if (serverReady) return;
  serverReady = true;
  console.log('[gate_argon2] Server is ready, scheduling connection creation...');
  setTimeout(() => {
    try {
      console.log('[gate_argon2] Creating virtual connection...');
      connId = Module._wasm_new_connection();
      console.log('[gate_argon2] Connection created, id=' + connId);
      setTimeout(doConnect, 2000);
    } catch(e) {
      console.error('[gate_argon2] Connection error:', e);
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
  console.log('[gate_argon2] Module loaded');
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
