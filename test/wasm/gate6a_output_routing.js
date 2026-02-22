/**
 * Gate 6a: Per-connection output routing
 *
 * Verifies that onConnectionOutput fires with correct connId
 * for each connection.
 */
const fs = require('fs');
const path = require('path');

const TIMEOUT = 30000;
const wasmDir = path.resolve(__dirname, '../../build-wasm');
let serverReady = false;
let listenSeen = false;
let Module;

const timer = setTimeout(() => {
    console.error('TIMEOUT: No response received within ' + TIMEOUT + 'ms');
    console.error('State: serverReady=' + serverReady);
    process.exit(1);
}, TIMEOUT);

const ToastStuntModule = require(path.join(wasmDir, 'moo.js'));

// Normalize CRLF in Minimal.db
const dbText = fs.readFileSync(path.resolve(__dirname, '../../Minimal.db'), 'utf-8').replace(/\r\n/g, '\n');
const dbData = new TextEncoder().encode(dbText);

const outputs = []; // Collect {connId, text} pairs

function runCommands() {
    if (serverReady) return;
    serverReady = true;
    console.log('[gate6a] Server is ready, scheduling connection creation...');

    setTimeout(() => {
        try {
            // Create two connections
            const conn0 = Module._wasm_new_connection();
            const conn1 = Module._wasm_new_connection();

            console.log('[gate6a] conn0:', conn0, 'conn1:', conn1);

            if (conn0 < 0 || conn1 < 0) {
                console.log('FAIL: Could not create connections');
                clearTimeout(timer);
                process.exit(1);
            }

            // Wait a bit for login output (*** Connected ***) to arrive
            setTimeout(() => {
                // Clear any login output collected so far
                const preOutputCount = outputs.length;
                console.log('[gate6a] Pre-command outputs collected:', preOutputCount);

                // Check that we already have tagged output from the login
                const conn0login = outputs.filter(o => o.connId === conn0);
                const conn1login = outputs.filter(o => o.connId === conn1);
                console.log('[gate6a] Login outputs - conn0:', conn0login.length, 'conn1:', conn1login.length);

                // Now inject input into both
                Module.ccall('wasm_inject_input', null, ['number', 'string'], [conn0, 'xyzzy']);
                Module.ccall('wasm_inject_input', null, ['number', 'string'], [conn1, 'xyzzy']);

                // Wait for output
                setTimeout(() => {
                    console.log('[gate6a] Collected outputs:', JSON.stringify(outputs, null, 2));

                    const conn0outputs = outputs.filter(o => o.connId === conn0);
                    const conn1outputs = outputs.filter(o => o.connId === conn1);

                    if (conn0outputs.length > 0 && conn1outputs.length > 0) {
                        console.log('PASS: Output tagged with correct connId for both connections');
                        console.log('  conn0 outputs:', conn0outputs.length);
                        console.log('  conn1 outputs:', conn1outputs.length);
                        clearTimeout(timer);
                        process.exit(0);
                    } else {
                        console.log('FAIL: Missing outputs for one or both connections');
                        console.log('  conn0 outputs:', conn0outputs.length);
                        console.log('  conn1 outputs:', conn1outputs.length);
                        clearTimeout(timer);
                        process.exit(1);
                    }
                }, 3000);
            }, 2000);
        } catch(e) {
            console.error('[gate6a] Error:', e);
            clearTimeout(timer);
            process.exit(1);
        }
    }, 1000);
}

ToastStuntModule({
    onConnectionOutput: function(connId, text) {
        outputs.push({ connId, text });
        console.log('[out conn=' + connId + '] ' + text);
    },
    printErr: function(text) {
        console.error('[err] ' + text);
        if (text.includes('LISTEN:')) {
            listenSeen = true;
            if (!serverReady && Module) {
                runCommands();
            }
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
    console.log('[gate6a] Module loaded');
    console.log('[gate6a] Available exports:', Object.keys(m).filter(k => k.startsWith('_wasm')));
    // Check if LISTEN was already received before Module was set
    if (!serverReady && listenSeen) {
        runCommands();
    }
}).catch((e) => {
    if (e.status !== 0) {
        console.error('Module error:', e);
        clearTimeout(timer);
        process.exit(1);
    }
});
