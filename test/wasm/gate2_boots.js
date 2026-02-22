const fs = require('fs');
const path = require('path');

const TIMEOUT = 15000; // 15 seconds max
const wasmDir = path.resolve(__dirname, '../../build-wasm');

let output = '';
let exited = false;

const timer = setTimeout(() => {
  // If we haven't exited and we have LISTEN in output, that's a pass
  if (output.includes('LISTEN:')) {
    console.log('GATE2_PASS');
    process.exit(0);
  } else {
    console.error('TIMEOUT: Server did not produce LISTEN line');
    console.error('Output so far:', output);
    process.exit(1);
  }
}, TIMEOUT);

const ToastStuntModule = require(path.join(wasmDir, 'moo.js'));

ToastStuntModule({
  print: (text) => { output += text + '\n'; console.log(text); },
  printErr: (text) => { output += text + '\n'; console.error(text); },
  locateFile: (p) => path.join(wasmDir, p),
  preRun: [(mod) => {
    // Read DB as text and normalize CRLF to LF.
    // Emscripten's virtual FS does not perform text-mode CRLF translation,
    // so the DB parser (which expects lines ending in \n) chokes on \r\n.
    const raw = fs.readFileSync(path.resolve(__dirname, '../../Minimal.db'), 'utf-8');
    const normalized = raw.replace(/\r\n/g, '\n');
    const data = new Uint8Array(Buffer.from(normalized, 'utf-8'));
    mod.FS.writeFile('/Minimal.db', data);
    // Also create output db path
    mod.FS.writeFile('/Minimal.db.new', new Uint8Array(0));
  }],
  arguments: ['/Minimal.db', '/Minimal.db.new'],
  noExitRuntime: true
}).then((m) => {
  // Module loaded, main will be called automatically with arguments
}).catch((e) => {
  if (e.status === 0) {
    // Clean exit
  } else {
    console.error('Module error:', e);
    clearTimeout(timer);
    process.exit(1);
  }
});

process.on('exit', (code) => {
  clearTimeout(timer);
  if (code === 0 && !output.includes('GATE2_PASS')) {
    // Check if we got far enough
    if (output.includes('LISTEN:')) {
      // Pass was already printed
    }
  }
});
