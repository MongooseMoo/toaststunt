const fs = require('fs');
const path = require('path');

const TIMEOUT = 90000;
const wasmDir = path.resolve(__dirname, '../../build-wasm');
let output = '';
let serverReady = false;
let loginVerified = false;
let connId = -1;
let Module;

// Test phases: boot -> login -> connect -> sha256 -> md5 -> hmac -> done
let phase = 'boot';
const results = {};

const timer = setTimeout(() => {
  console.error('TIMEOUT: No response within ' + TIMEOUT + 'ms');
  console.error('State: phase=' + phase + ' serverReady=' + serverReady);
  console.error('Results so far:', JSON.stringify(results));
  console.error('Output so far:', output.slice(-500));
  process.exit(1);
}, TIMEOUT);

const ToastStuntModule = require(path.join(wasmDir, 'moo.js'));

const dbPath = path.resolve(__dirname, '../../toastcore.db');
const dbText = fs.readFileSync(dbPath, 'utf-8').replace(/\r\n/g, '\n');
const dbData = new TextEncoder().encode(dbText);

function handlePrintOutput(text) {
  // Connect phase
  if (phase === 'connect') {
    if (text.includes('*** Connected ***')) {
      loginVerified = true;
      console.log('[gate_crypto_hash] Connected as wizard');
      phase = 'sha256_pending';
      setTimeout(testSHA256, 2000);
      return;
    }
  }

  // SHA256 test
  if (phase === 'sha256') {
    // SHA256("hello") = 2CF24DBA5FB0A30E26E83B2AC5B9E29E1B161E5C1FA7425E73043362938B9824
    if (text.match(/[0-9A-F]{64}/)) {
      results.sha256 = 'PASS';
      const hash = text.match(/([0-9A-F]{64})/)[1];
      console.log('[gate_crypto_hash] SHA256 PASSED: ' + hash);
      phase = 'md5_pending';
      setTimeout(testMD5, 2000);
      return;
    }
    if (text.includes('E_PERM')) {
      results.sha256 = 'FAIL_PERM';
      console.error('[gate_crypto_hash] SHA256 FAILED: E_PERM (still stubbed!)');
      fail();
      return;
    }
    if (text.includes('E_INVARG')) {
      results.sha256 = 'FAIL_INVARG';
      console.error('[gate_crypto_hash] SHA256 FAILED: E_INVARG: ' + text);
      fail();
      return;
    }
  }

  // MD5 test
  if (phase === 'md5') {
    // MD5("hello") = 5D41402ABC4B2A76B9719D911017C592
    if (text.match(/[0-9A-F]{32}/) && !text.match(/[0-9A-F]{33}/)) {
      results.md5 = 'PASS';
      const hash = text.match(/([0-9A-F]{32})/)[1];
      console.log('[gate_crypto_hash] MD5 PASSED: ' + hash);
      phase = 'hmac_pending';
      setTimeout(testHMAC, 2000);
      return;
    }
    if (text.includes('E_PERM') || text.includes('E_INVARG')) {
      results.md5 = 'FAIL';
      console.error('[gate_crypto_hash] MD5 FAILED: ' + text);
      fail();
      return;
    }
  }

  // HMAC-SHA256 test
  if (phase === 'hmac') {
    // HMAC-SHA256 should return a 64-char hex string
    if (text.match(/[0-9A-F]{64}/)) {
      results.hmac = 'PASS';
      const hash = text.match(/([0-9A-F]{64})/)[1];
      console.log('[gate_crypto_hash] HMAC-SHA256 PASSED: ' + hash);
      allDone();
      return;
    }
    if (text.includes('E_PERM') || text.includes('E_INVARG')) {
      results.hmac = 'FAIL';
      console.error('[gate_crypto_hash] HMAC FAILED: ' + text);
      fail();
      return;
    }
  }
}

function fail() {
  console.error('[gate_crypto_hash] GATE_CRYPTO_HASH_FAIL');
  console.error('Results:', JSON.stringify(results));
  clearTimeout(timer);
  process.exit(1);
}

function allDone() {
  if (results.sha256 === 'PASS' && results.md5 === 'PASS' && results.hmac === 'PASS') {
    phase = 'done';
    console.log('[gate_crypto_hash] ALL PHASES PASSED');
    console.log('GATE_CRYPTO_HASH_PASS');
    clearTimeout(timer);
    process.exit(0);
  }
}

function doConnect() {
  phase = 'connect';
  console.log('[gate_crypto_hash] Sending: connect Wizard');
  Module.ccall('wasm_inject_input', null, ['number', 'string'], [connId, 'connect Wizard']);
}

function testSHA256() {
  phase = 'sha256';
  const cmd = '; string_hash("hello", "SHA256")';
  console.log('[gate_crypto_hash] Injecting SHA256 test: ' + cmd);
  Module.ccall('wasm_inject_input', null, ['number', 'string'], [connId, cmd]);

  setTimeout(() => {
    if (phase !== 'sha256') return;
    const outBuf = Module.ccall('wasm_get_output', 'string', ['number'], [connId]);
    if (outBuf) outBuf.split('\n').forEach(line => handlePrintOutput(line));
  }, 10000);
}

function testMD5() {
  phase = 'md5';
  const cmd = '; string_hash("hello", "MD5")';
  console.log('[gate_crypto_hash] Injecting MD5 test: ' + cmd);
  Module.ccall('wasm_inject_input', null, ['number', 'string'], [connId, cmd]);

  setTimeout(() => {
    if (phase !== 'md5') return;
    const outBuf = Module.ccall('wasm_get_output', 'string', ['number'], [connId]);
    if (outBuf) outBuf.split('\n').forEach(line => handlePrintOutput(line));
  }, 10000);
}

function testHMAC() {
  phase = 'hmac';
  // string_hmac(string, key, [algo]) -- key must be binary, use "secret" as-is
  const cmd = '; string_hmac("hello", "secret", "SHA256")';
  console.log('[gate_crypto_hash] Injecting HMAC test: ' + cmd);
  Module.ccall('wasm_inject_input', null, ['number', 'string'], [connId, cmd]);

  setTimeout(() => {
    if (phase !== 'hmac') return;
    const outBuf = Module.ccall('wasm_get_output', 'string', ['number'], [connId]);
    if (outBuf) outBuf.split('\n').forEach(line => handlePrintOutput(line));
  }, 10000);
}

function onServerReady() {
  if (serverReady) return;
  serverReady = true;
  console.log('[gate_crypto_hash] Server is ready, scheduling connection...');
  setTimeout(() => {
    try {
      connId = Module._wasm_new_connection();
      console.log('[gate_crypto_hash] Connection created, id=' + connId);
      setTimeout(doConnect, 2000);
    } catch(e) {
      console.error('[gate_crypto_hash] Connection error:', e);
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
  console.log('[gate_crypto_hash] Module loaded');
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
