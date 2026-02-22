const fs = require('fs');
const path = require('path');

const TIMEOUT = 90000;
const wasmDir = path.resolve(__dirname, '../../build-wasm');
let output = '';
let serverReady = false;
let loginVerified = false;
let connId = -1;
let Module;
let phase = 'boot'; // boot -> login -> connect -> des -> bcrypt -> done

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

let desResult = null;
let bcryptResult = null;

function handlePrintOutput(text) {
  // Phase: waiting for connect confirmation
  if (phase === 'connect') {
    if (text.includes('*** Connected ***')) {
      loginVerified = true;
      console.log('[gate_crypt] Connected as wizard');
      phase = 'des_pending';
      setTimeout(testDES, 2000);
      return;
    }
  }

  // Phase: DES test
  if (phase === 'des') {
    // DES crypt returns a 13-char string starting with the 2-char salt
    const desMatch = text.match(/(xx[A-Za-z0-9./]{11})/);
    if (desMatch) {
      desResult = 'PASS';
      console.log('[gate_crypt] Phase DES PASSED: DES crypt returned hash: ' + desMatch[1]);
      phase = 'bcrypt_pending';
      setTimeout(testBcrypt, 2000);
      return;
    }
    if (text.includes('E_PERM')) {
      desResult = 'FAIL';
      console.error('[gate_crypt] Phase DES FAILED: returned E_PERM');
      console.error('GATE_CRYPT_FAIL');
      clearTimeout(timer);
      process.exit(1);
    }
    if (text.includes('E_INVARG')) {
      desResult = 'FAIL';
      console.error('[gate_crypt] Phase DES FAILED: returned E_INVARG: ' + text);
      console.error('GATE_CRYPT_FAIL');
      clearTimeout(timer);
      process.exit(1);
    }
  }

  // Phase: bcrypt test
  if (phase === 'bcrypt') {
    if (text.includes('$2a$')) {
      bcryptResult = 'PASS';
      console.log('[gate_crypt] Phase BCRYPT PASSED: bcrypt returned hash');
      allDone();
      return;
    }
    if (text.includes('E_PERM')) {
      bcryptResult = 'FAIL';
      console.error('[gate_crypt] Phase BCRYPT FAILED: returned E_PERM');
      console.error('GATE_CRYPT_FAIL');
      clearTimeout(timer);
      process.exit(1);
    }
    if (text.includes('E_INVARG')) {
      bcryptResult = 'FAIL';
      console.error('[gate_crypt] Phase BCRYPT FAILED: returned E_INVARG: ' + text);
      console.error('GATE_CRYPT_FAIL');
      clearTimeout(timer);
      process.exit(1);
    }
  }
}

function allDone() {
  if (desResult === 'PASS' && bcryptResult === 'PASS') {
    phase = 'done';
    console.log('[gate_crypt] ALL PHASES PASSED');
    console.log('GATE_CRYPT_PASS');
    clearTimeout(timer);
    process.exit(0);
  }
}

function doConnect() {
  phase = 'connect';
  // Connect as wizard - toastcore wizard password is typically empty or "wizard"
  // Try "connect Wizard" first
  console.log('[gate_crypt] Sending: connect Wizard');
  Module.ccall('wasm_inject_input', null, ['number', 'string'], [connId, 'connect Wizard']);
}

function testDES() {
  phase = 'des';
  const cmd = '; crypt("test", "xx")';
  console.log('[gate_crypt] Injecting DES test: ' + cmd);
  Module.ccall('wasm_inject_input', null, ['number', 'string'], [connId, cmd]);

  // Fallback poll
  setTimeout(() => {
    if (phase !== 'des') return;
    const outBuf = Module.ccall('wasm_get_output', 'string', ['number'], [connId]);
    console.log('[gate_crypt] DES fallback poll: ' + JSON.stringify(outBuf));
    if (outBuf) {
      outBuf.split('\n').forEach(line => handlePrintOutput(line));
    }
  }, 10000);
}

function testBcrypt() {
  phase = 'bcrypt';
  const cmd = '; crypt("test", "$2a$04$abcdefghijklmnopqrstuv")';
  console.log('[gate_crypt] Injecting bcrypt test: ' + cmd);
  Module.ccall('wasm_inject_input', null, ['number', 'string'], [connId, cmd]);

  // Fallback poll
  setTimeout(() => {
    if (phase !== 'bcrypt') return;
    const outBuf = Module.ccall('wasm_get_output', 'string', ['number'], [connId]);
    console.log('[gate_crypt] Bcrypt fallback poll: ' + JSON.stringify(outBuf));
    if (outBuf) {
      outBuf.split('\n').forEach(line => handlePrintOutput(line));
    }
  }, 15000);
}

function onServerReady() {
  if (serverReady) return;
  serverReady = true;
  console.log('[gate_crypt] Server is ready, scheduling connection creation...');
  setTimeout(() => {
    try {
      console.log('[gate_crypt] Creating virtual connection...');
      connId = Module._wasm_new_connection();
      console.log('[gate_crypt] Connection created, id=' + connId);
      // In Minimal.db, login is automatic. In toastcore.db, we need to connect explicitly
      // Wait a bit then try to connect
      setTimeout(doConnect, 2000);
    } catch(e) {
      console.error('[gate_crypt] Connection error:', e);
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
  console.log('[gate_crypt] Module loaded');
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
