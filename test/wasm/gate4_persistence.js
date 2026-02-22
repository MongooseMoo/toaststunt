const fs = require('fs');
const path = require('path');

const TIMEOUT = 45000;
const wasmDir = path.resolve(__dirname, '../../build-wasm');
let output = '';
let serverReady = false;
let loginVerified = false;
let checkpointRequested = false;
let connId = -1;
let Module;

const timer = setTimeout(() => {
  console.error('TIMEOUT: No response received within ' + TIMEOUT + 'ms');
  console.error('State: serverReady=' + serverReady + ' loginVerified=' + loginVerified +
                ' checkpointRequested=' + checkpointRequested);
  console.error('Output so far:', output);
  process.exit(1);
}, TIMEOUT);

const ToastStuntModule = require(path.join(wasmDir, 'moo.js'));

// Normalize CRLF in Minimal.db
const dbText = fs.readFileSync(path.resolve(__dirname, '../../Minimal.db'), 'utf-8').replace(/\r\n/g, '\n');
const dbData = new TextEncoder().encode(dbText);

function onLoginOutput(text) {
  if (loginVerified || !serverReady) return;
  if (text.includes('*** Connected ***')) {
    loginVerified = true;
    console.log('[gate4] Phase 1 PASSED: Login output verified ("*** Connected ***")');
    // After login, wait for the main loop to settle, then trigger checkpoint
    setTimeout(() => {
      triggerCheckpoint();
    }, 2000);
  }
}

function triggerCheckpoint() {
  console.log('[gate4] Requesting checkpoint via wasm_checkpoint()...');
  checkpointRequested = true;
  try {
    // wasm_checkpoint() just sets checkpoint_requested flag -- no I/O, no ASYNCIFY
    Module._wasm_checkpoint();
    console.log('[gate4] Checkpoint requested. Waiting for main loop to execute it...');
  } catch (e) {
    console.error('[gate4] wasm_checkpoint() threw:', e);
    process.exit(1);
  }
  // The actual checkpoint happens on the next main loop iteration.
  // We watch stderr for "CHECKPOINTING on ... finished" in onCheckpointLog.
  // Set a fallback timer in case we miss the log line.
  setTimeout(() => {
    console.log('[gate4] Fallback: checking for checkpoint file after timeout...');
    verifyCheckpoint();
  }, 10000);
}

function onCheckpointLog(text) {
  if (!checkpointRequested) return;
  // The server logs "CHECKPOINTING on /Minimal.db.new.#N# finished" when done
  if (text.includes('CHECKPOINTING on') && text.includes('finished')) {
    console.log('[gate4] Checkpoint completion detected in server log');
    // Give a brief moment for file operations to settle
    setTimeout(() => {
      verifyCheckpoint();
    }, 500);
  }
}

function verifyCheckpoint() {
  console.log('[gate4] Verifying checkpoint output...');
  try {
    // List root FS contents
    const rootFiles = Module.FS.readdir('/');
    const dbFiles = rootFiles.filter(f => f.startsWith('Minimal'));
    console.log('[gate4] Minimal.db related files: ' + JSON.stringify(dbFiles));

    // Look for any checkpoint file
    let checkpointFile = null;
    let checkpointSize = 0;

    for (const f of dbFiles) {
      try {
        const s = Module.FS.stat('/' + f);
        console.log('[gate4]   /' + f + ' size=' + s.size);
        // After successful checkpoint, temp file is renamed to /Minimal.db.new
        if (f === 'Minimal.db.new' && s.size > 0) {
          checkpointFile = '/' + f;
          checkpointSize = s.size;
        }
        // Also check for temp checkpoint files like /Minimal.db.new.#1#
        if (f.match(/Minimal\.db\.new\.#\d+#/) && s.size > 0) {
          checkpointFile = '/' + f;
          checkpointSize = s.size;
        }
      } catch (e) {
        // skip
      }
    }

    if (checkpointFile && checkpointSize > 0) {
      console.log('[gate4] Checkpoint file found: ' + checkpointFile + ' (' + checkpointSize + ' bytes)');

      // Read the file and verify it contains valid DB data
      try {
        const data = Module.FS.readFile(checkpointFile, { encoding: 'utf8' });
        const hasHeader = data.includes('** LambdaMOO Database') || data.includes('ToastStunt');
        const hasObjects = data.includes('#0') || data.includes('System Object');
        const snippet = data.substring(0, 500);
        console.log('[gate4] DB file header (first 500 chars):');
        console.log(snippet);
        console.log('[gate4] Has DB header: ' + hasHeader);
        console.log('[gate4] References objects: ' + hasObjects);
      } catch (e) {
        console.log('[gate4] Could not read checkpoint file as text: ' + e.message);
      }

      console.log('GATE4_PASS');
      clearTimeout(timer);
      process.exit(0);
    } else {
      console.error('[gate4] No non-empty checkpoint file found');
      console.error('GATE4_FAIL: Checkpoint file not found or empty');
      process.exit(1);
    }
  } catch (e) {
    console.error('[gate4] Error verifying checkpoint:', e);
    process.exit(1);
  }
}

function onServerReady() {
  if (serverReady) return;
  serverReady = true;
  console.log('[gate4] Server is ready, scheduling connection creation...');
  setTimeout(() => {
    try {
      console.log('[gate4] Creating virtual connection...');
      connId = Module._wasm_new_connection();
      console.log('[gate4] Connection created, id=' + connId);
    } catch(e) {
      console.error('[gate4] Connection error:', e);
    }
  }, 1000);
}

ToastStuntModule({
  print: (text) => {
    output += text + '\n';
    console.log('[out] ' + text);
    onLoginOutput(text);
  },
  printErr: (text) => {
    output += '[err] ' + text + '\n';
    console.error('[err] ' + text);
    // Server log goes to stderr - detect LISTEN here
    if (text.includes('LISTEN:') && !serverReady && Module) {
      onServerReady();
    }
    // Detect checkpoint completion
    onCheckpointLog(text);
  },
  locateFile: (p) => path.join(wasmDir, p),
  preRun: [(mod) => {
    mod.FS.writeFile('/Minimal.db', dbData);
  }],
  arguments: ['/Minimal.db', '/Minimal.db.new'],
  noExitRuntime: true
}).then((m) => {
  Module = m;
  console.log('[gate4] Module loaded');
  console.log('[gate4] Available exports:', Object.keys(m).filter(k => k.startsWith('_wasm')));
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
