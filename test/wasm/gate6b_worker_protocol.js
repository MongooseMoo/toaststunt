/**
 * Gate 6b: Worker Protocol Extensions
 *
 * Tests that the worker supports multiple connections with
 * per-connection output routing via the new message types.
 */
const path = require('path');
const fs = require('fs');

const TIMEOUT = 30000;
const wasmDir = path.resolve(__dirname, '../../build-wasm');
const createModule = require(path.join(wasmDir, 'moo.js'));

// Normalize CRLF in Minimal.db (same as gate6a)
const dbText = fs.readFileSync(path.resolve(__dirname, '../../Minimal.db'), 'utf-8').replace(/\r\n/g, '\n');
const dbData = new TextEncoder().encode(dbText);

const timer = setTimeout(() => {
    console.error('TIMEOUT: No response within ' + TIMEOUT + 'ms');
    process.exit(1);
}, TIMEOUT);

async function runTest() {
    const outputs = [];
    const connOutputs = [];
    let readyResolve;
    const readyPromise = new Promise(r => readyResolve = r);

    const Module = await createModule({
        onConnectionOutput: function(connId, text) {
            if (connId === 0) {
                outputs.push({ connId, text });
            } else {
                connOutputs.push({ connId, text });
            }
        },
        print: function(text) {
            outputs.push({ connId: -1, text });
        },
        printErr: function(text) {
            if (text.indexOf('LISTEN:') !== -1) {
                readyResolve();
            }
        },
        locateFile: (p) => path.join(wasmDir, p),
        preRun: [function(mod) {
            mod.FS.writeFile('/Minimal.db', dbData);
            mod.FS.writeFile('/Minimal.db.new', new Uint8Array(0));
        }],
        arguments: ['/Minimal.db', '/Minimal.db.new'],
        noExitRuntime: true
    });

    console.log('[gate6b] Waiting for server ready...');
    await readyPromise;
    console.log('[gate6b] Server ready');

    // Step 1: Create connection 0 (host)
    await new Promise(r => setTimeout(r, 300));
    const conn0 = Module._wasm_new_connection();
    console.log('[gate6b] conn0:', conn0);
    if (conn0 < 0) { console.log('FAIL: conn0 creation failed'); clearTimeout(timer); process.exit(1); }

    // Step 2: Create connection 1 (guest)
    await new Promise(r => setTimeout(r, 300));
    const conn1 = Module._wasm_new_connection();
    console.log('[gate6b] conn1:', conn1);
    if (conn1 < 0) { console.log('FAIL: conn1 creation failed'); clearTimeout(timer); process.exit(1); }

    // Step 3: Set name on connection 1
    Module.ccall('wasm_set_connection_name', null, ['number', 'string'], [conn1, 'guest-player']);
    console.log('[gate6b] Set conn1 name to guest-player');

    // Step 4: Inject input into conn0
    await new Promise(r => setTimeout(r, 300));
    outputs.length = 0;
    connOutputs.length = 0;
    Module.ccall('wasm_inject_input', null, ['number', 'string'], [conn0, 'xyzzy']);

    // Step 5: Inject input into conn1
    await new Promise(r => setTimeout(r, 300));
    Module.ccall('wasm_inject_input', null, ['number', 'string'], [conn1, 'xyzzy']);

    // Step 6: Wait and check outputs
    await new Promise(r => setTimeout(r, 1000));

    console.log('[gate6b] Host outputs (connId=0):', JSON.stringify(outputs));
    console.log('[gate6b] Guest outputs (connId>0):', JSON.stringify(connOutputs));

    const hasHostOutput = outputs.some(o => o.connId === 0);
    const hasGuestOutput = connOutputs.some(o => o.connId === conn1);

    // Step 7: Close connection 1
    Module._wasm_close_connection(conn1);
    console.log('[gate6b] Closed conn1');

    clearTimeout(timer);
    if (hasHostOutput && hasGuestOutput) {
        console.log('PASS: Worker protocol supports multiple connections with routed output');
        process.exit(0);
    } else {
        console.log('FAIL: Missing host or guest output');
        console.log('  hasHostOutput:', hasHostOutput);
        console.log('  hasGuestOutput:', hasGuestOutput);
        process.exit(1);
    }
}

runTest().catch(err => {
    console.error('Test error:', err);
    clearTimeout(timer);
    process.exit(1);
});
