const fs = require('fs');
const path = require('path');

const TIMEOUT = 30000;
const wasmDir = path.resolve(__dirname, '../../build-wasm');
let output = '';
let serverReady = false;
let loginVerified = false;
let commandInjected = false;
let connId = -1;
let Module;

const timer = setTimeout(() => {
  console.error('TIMEOUT: No response received within ' + TIMEOUT + 'ms');
  console.error('State: serverReady=' + serverReady + ' loginVerified=' + loginVerified + ' commandInjected=' + commandInjected);
  console.error('Output so far:', output);
  process.exit(1);
}, TIMEOUT);

const ToastStuntModule = require(path.join(wasmDir, 'moo.js'));

// Normalize CRLF in Minimal.db
const dbText = fs.readFileSync(path.resolve(__dirname, '../../Minimal.db'), 'utf-8').replace(/\r\n/g, '\n');
const dbData = new TextEncoder().encode(dbText);

// Phase 2 check: look for command response AFTER we injected input
function checkCommandResponse(text) {
  if (!commandInjected) return;
  // Any of these indicate the command pipeline processed our input:
  // - "I couldn't understand that." (unrecognized command in Minimal.db)
  // - "That is not a valid command." or similar
  // - Room description text
  // - Any eval result (for "; 1 + 1" => "2")
  if (text.includes('I couldn\'t') ||
      text.includes('don\'t understand') ||
      text.includes('huh') ||
      text.includes('not a valid command') ||
      text.includes('The First Room') ||
      text.includes('Room') ||
      // eval result: "; 1 + 1" produces "=> 2" or just "2"
      /^\s*=?>?\s*2\s*$/.test(text) ||
      text.includes('=> 2')) {
    console.log('[gate3] Phase 2 PASSED: Command response received: ' + JSON.stringify(text));
    console.log('GATE3_PASS');
    clearTimeout(timer);
    process.exit(0);
  }
}

// Phase 1: After login output is seen, inject a command
function onLoginOutput(text) {
  if (loginVerified || !serverReady) return;
  if (text.includes('*** Connected ***')) {
    loginVerified = true;
    console.log('[gate3] Phase 1 PASSED: Login output verified ("*** Connected ***")');
    // Now inject a command after a short delay to let the main loop process
    setTimeout(() => {
      injectCommand();
    }, 1000);
  }
}

function injectCommand() {
  if (commandInjected || connId < 0) return;
  commandInjected = true;

  // Try "; 1 + 1" which is a MOO eval expression — Wizard (#3) can eval
  const cmd = '; 1 + 1';
  console.log('[gate3] Injecting command: ' + JSON.stringify(cmd));
  Module.ccall('wasm_inject_input', null, ['number', 'string'], [connId, cmd]);

  // Also set a fallback: after a delay, check the output buffer directly
  setTimeout(() => {
    const outBuf = Module.ccall('wasm_get_output', 'string', ['number'], [connId]);
    console.log('[gate3] Output buffer after command: ' + JSON.stringify(outBuf));
    // Check the full buffer for command response
    if (outBuf) {
      checkCommandResponse(outBuf);
    }
    // If still not passed, try a second command as fallback
    if (!commandInjected) return; // already exited
    console.log('[gate3] First command response not detected, trying "xyzzy"...');
    Module.ccall('wasm_inject_input', null, ['number', 'string'], [connId, 'xyzzy']);
    setTimeout(() => {
      const outBuf2 = Module.ccall('wasm_get_output', 'string', ['number'], [connId]);
      console.log('[gate3] Output buffer after xyzzy: ' + JSON.stringify(outBuf2));
      if (outBuf2) {
        checkCommandResponse(outBuf2);
      }
      console.error('GATE3_FAIL: Command was injected but no recognizable response received');
      console.error('Full output buffer:', outBuf2);
      process.exit(1);
    }, 3000);
  }, 3000);
}

function onServerReady() {
  if (serverReady) return;
  serverReady = true;
  console.log('[gate3] Server is ready, scheduling connection creation...');
  setTimeout(() => {
    try {
      console.log('[gate3] Creating virtual connection...');
      connId = Module._wasm_new_connection();
      console.log('[gate3] Connection created, id=' + connId);
    } catch(e) {
      console.error('[gate3] Connection error:', e);
    }
  }, 1000);
}

ToastStuntModule({
  print: (text) => {
    output += text + '\n';
    console.log('[out] ' + text);
    onLoginOutput(text);
    checkCommandResponse(text);
  },
  printErr: (text) => {
    output += '[err] ' + text + '\n';
    console.error('[err] ' + text);
    // Server log goes to stderr - detect LISTEN here
    if (text.includes('LISTEN:') && !serverReady && Module) {
      onServerReady();
    }
  },
  locateFile: (p) => path.join(wasmDir, p),
  preRun: [(mod) => {
    mod.FS.writeFile('/Minimal.db', dbData);
    mod.FS.writeFile('/Minimal.db.new', new Uint8Array(0));
  }],
  arguments: ['/Minimal.db', '/Minimal.db.new'],
  noExitRuntime: true
}).then((m) => {
  Module = m;
  console.log('[gate3] Module loaded');
  console.log('[gate3] Available exports:', Object.keys(m).filter(k => k.startsWith('_wasm')));
  // Check if LISTEN was already received before Module was set
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
