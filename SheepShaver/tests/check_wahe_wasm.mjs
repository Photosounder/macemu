// Audit the delivered binary without creating a host, worker or window
import fs from 'node:fs';
import assert from 'node:assert/strict';

const bytes = fs.readFileSync(process.argv[2] || new URL('../sheepshaver.wasm', import.meta.url));
const module = new WebAssembly.Module(bytes);
assert.deepEqual(WebAssembly.Module.imports(module), [
    { module: 'env', name: 'memory', kind: 'memory' },
    { module: 'env', name: 'wahe_run_command', kind: 'function' }
]);
let offset = 8;
function readULEB() {
    // Decode a section length without depending on a separate Wasm tooling package
    let value = 0, shift = 0, byte;
    do {
        // Accumulate each seven-bit group from the bounded module buffer
        assert(offset < bytes.length && shift < 35);
        byte = bytes[offset++];
        value += (byte & 127) * 2 ** shift;
        shift += 7;
    } while (byte & 128);
    return value;
}
while (offset < bytes.length) {
    // Reject exception tags even when they would add no host import
    const section = bytes[offset++], length = readULEB();
    assert.notEqual(section, 13, 'Module requires Wasm exception handling');
    offset += length;
    assert(offset <= bytes.length);
}
// Allow the linker's shared-data initialization while rejecting application entry exports
for (const name of ['main', '_main', '_start', 'WinMain', 'SDL_main']) {
    // Keep guest execution exclusively behind the WAHE callback interface
    assert(!WebAssembly.Module.exports(module).some(item => item.name === name), `Unexpected entry point ${name}`);
}
for (const name of ['_initialize', 'module_message_input', 'module_draw', 'module_proc_sound',
    'module_thread_entry', 'module_malloc', 'module_realloc', 'module_free', '__wasm_init_tls']) {
    // Require the complete callback and runner interface expected by WAHE
    assert(WebAssembly.Module.exports(module).some(item => item.name === name), `Missing export ${name}`);
}
console.log('PASS: WAHE callbacks only; no WASI, process entry point or Wasm exception tags');
