const fs = require('fs');
const path = require('path');

const TIMEOUT = 90000;
const wasmDir = path.resolve(__dirname, '../../build-wasm');
let output = '';
let serverReady = false;
let loginVerified = false;
let connId = -1;
let Module;
let phase = 'boot'; // boot -> login -> connect -> pcre_match -> pcre_replace -> pcre_cache_stats -> done

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

let matchResult = null;
let replaceResult = null;
let cacheResult = null;

function handlePrintOutput(text) {
  // Phase: waiting for connect confirmation
  if (phase === 'connect') {
    if (text.includes('*** Connected ***')) {
      loginVerified = true;
      console.log('[gate_pcre] Connected as wizard');
      phase = 'match_pending';
      setTimeout(testPcreMatch, 2000);
      return;
    }
  }

  // Phase: pcre_match test
  if (phase === 'pcre_match') {
    // pcre_match returns a list of maps; a successful match contains "match"
    // We look for "Hello" in the output since we're matching "Hello World" against "(\\w+)"
    if (text.includes('Hello')) {
      matchResult = 'PASS';
      console.log('[gate_pcre] Phase PCRE_MATCH PASSED: ' + text);
      phase = 'replace_pending';
      setTimeout(testPcreReplace, 2000);
      return;
    }
    if (text.includes('E_VERBNF')) {
      matchResult = 'FAIL';
      console.error('[gate_pcre] Phase PCRE_MATCH FAILED: verb not found (PCRE not registered)');
      console.error('GATE_PCRE_FAIL');
      clearTimeout(timer);
      process.exit(1);
    }
    if (text.includes('E_PERM') || text.includes('E_INVARG')) {
      matchResult = 'FAIL';
      console.error('[gate_pcre] Phase PCRE_MATCH FAILED: ' + text);
      console.error('GATE_PCRE_FAIL');
      clearTimeout(timer);
      process.exit(1);
    }
  }

  // Phase: pcre_replace test
  if (phase === 'pcre_replace') {
    // pcre_replace("Hello World", "s/World/MOO/") should return "Hello MOO"
    if (text.includes('Hello MOO')) {
      replaceResult = 'PASS';
      console.log('[gate_pcre] Phase PCRE_REPLACE PASSED: ' + text);
      phase = 'cache_pending';
      setTimeout(testPcreCacheStats, 2000);
      return;
    }
    if (text.includes('E_VERBNF')) {
      replaceResult = 'FAIL';
      console.error('[gate_pcre] Phase PCRE_REPLACE FAILED: verb not found');
      console.error('GATE_PCRE_FAIL');
      clearTimeout(timer);
      process.exit(1);
    }
    if (text.includes('E_PERM') || text.includes('E_INVARG')) {
      replaceResult = 'FAIL';
      console.error('[gate_pcre] Phase PCRE_REPLACE FAILED: ' + text);
      console.error('GATE_PCRE_FAIL');
      clearTimeout(timer);
      process.exit(1);
    }
  }

  // Phase: pcre_cache_stats test
  if (phase === 'pcre_cache_stats') {
    // pcre_cache_stats() returns a list; even an empty list {} is valid
    // A successful call means the function exists and is callable by wizard
    if (text.includes('{') || text.includes('cache_hits')) {
      cacheResult = 'PASS';
      console.log('[gate_pcre] Phase PCRE_CACHE_STATS PASSED: ' + text);
      allDone();
      return;
    }
    if (text.includes('E_VERBNF')) {
      cacheResult = 'FAIL';
      console.error('[gate_pcre] Phase PCRE_CACHE_STATS FAILED: verb not found');
      console.error('GATE_PCRE_FAIL');
      clearTimeout(timer);
      process.exit(1);
    }
  }
}

function allDone() {
  if (matchResult === 'PASS' && replaceResult === 'PASS' && cacheResult === 'PASS') {
    phase = 'done';
    console.log('[gate_pcre] ALL PHASES PASSED');
    console.log('GATE_PCRE_PASS');
    clearTimeout(timer);
    process.exit(0);
  }
}

function doConnect() {
  phase = 'connect';
  console.log('[gate_pcre] Sending: connect Wizard');
  Module.ccall('wasm_inject_input', null, ['number', 'string'], [connId, 'connect Wizard']);
}

function testPcreMatch() {
  phase = 'pcre_match';
  const cmd = '; pcre_match("Hello World", "(\\\\w+)")';
  console.log('[gate_pcre] Injecting pcre_match test: ' + cmd);
  Module.ccall('wasm_inject_input', null, ['number', 'string'], [connId, cmd]);

  // Fallback poll
  setTimeout(() => {
    if (phase !== 'pcre_match') return;
    const outBuf = Module.ccall('wasm_get_output', 'string', ['number'], [connId]);
    console.log('[gate_pcre] pcre_match fallback poll: ' + JSON.stringify(outBuf));
    if (outBuf) {
      outBuf.split('\n').forEach(line => handlePrintOutput(line));
    }
  }, 10000);
}

function testPcreReplace() {
  phase = 'pcre_replace';
  const cmd = '; pcre_replace("Hello World", "s/World/MOO/")';
  console.log('[gate_pcre] Injecting pcre_replace test: ' + cmd);
  Module.ccall('wasm_inject_input', null, ['number', 'string'], [connId, cmd]);

  // Fallback poll
  setTimeout(() => {
    if (phase !== 'pcre_replace') return;
    const outBuf = Module.ccall('wasm_get_output', 'string', ['number'], [connId]);
    console.log('[gate_pcre] pcre_replace fallback poll: ' + JSON.stringify(outBuf));
    if (outBuf) {
      outBuf.split('\n').forEach(line => handlePrintOutput(line));
    }
  }, 10000);
}

function testPcreCacheStats() {
  phase = 'pcre_cache_stats';
  const cmd = '; pcre_cache_stats()';
  console.log('[gate_pcre] Injecting pcre_cache_stats test: ' + cmd);
  Module.ccall('wasm_inject_input', null, ['number', 'string'], [connId, cmd]);

  // Fallback poll
  setTimeout(() => {
    if (phase !== 'pcre_cache_stats') return;
    const outBuf = Module.ccall('wasm_get_output', 'string', ['number'], [connId]);
    console.log('[gate_pcre] pcre_cache_stats fallback poll: ' + JSON.stringify(outBuf));
    if (outBuf) {
      outBuf.split('\n').forEach(line => handlePrintOutput(line));
    }
  }, 10000);
}

function onServerReady() {
  if (serverReady) return;
  serverReady = true;
  console.log('[gate_pcre] Server is ready, scheduling connection creation...');
  setTimeout(() => {
    try {
      console.log('[gate_pcre] Creating virtual connection...');
      connId = Module._wasm_new_connection();
      console.log('[gate_pcre] Connection created, id=' + connId);
      setTimeout(doConnect, 2000);
    } catch(e) {
      console.error('[gate_pcre] Connection error:', e);
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
  console.log('[gate_pcre] Module loaded');
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
