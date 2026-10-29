// Exercise the actual freestanding Wasm module without a browser, window or WASI runtime
import fs from 'node:fs';
import assert from 'node:assert/strict';
import { Worker, isMainThread, workerData } from 'node:worker_threads';

const encoder = new TextEncoder(), decoder = new TextDecoder();
const modulePath = process.argv[2] || new URL('../sheepshaver.wasm', import.meta.url);
const module = isMainThread ? new WebAssembly.Module(fs.readFileSync(modulePath)) : workerData.module;
const memory = isMainThread ? new WebAssembly.Memory({ initial: 256, maximum: 32768, shared: true }) : workerData.memory;
const completion = isMainThread ? new Int32Array(new SharedArrayBuffer(16)) : new Int32Array(workerData.completion);
const fixturePaths = isMainThread ? process.argv.slice(3) : workerData.fixturePaths;
let instance, worker, workerAllocations = [];
const files = new Map();
let nextHandle = 1, readCount = 0;

function readString(pointer) {
    // Decode a borrowed reply from the current shared-memory buffer
    if (!pointer) return '';
    const bytes = new Uint8Array(memory.buffer);
    let end = pointer;
    while (end < bytes.length && bytes[end]) ++end;
    assert(end < bytes.length, 'Unterminated module string');
    return decoder.decode(bytes.subarray(pointer, end));
}
function allocateString(text) {
    // Allocate host replies through the same public allocator used by the real WAHE host
    const bytes = encoder.encode(text + '\0');
    const pointer = instance.exports.module_malloc(bytes.length);
    assert(pointer);
    new Uint8Array(memory.buffer).set(bytes, pointer);
    return pointer;
}
function send(text) {
    // Preserve borrowed reply contents before returning the request buffer to the allocator
    const pointer = allocateString(text);
    const reply = readString(instance.exports.module_message_input(pointer));
    instance.exports.module_free(pointer);
    return reply;
}
function hostCommand(pointer) {
    // Implement only WAHE services and reject hidden dependencies on a process or JavaScript runtime
    const command = readString(pointer);
    if (command.startsWith('Shrink memory to ')) return 0;
    if (command === 'Get raw time') return allocateString(process.env.WAHE_TEST_BAD_CLOCK ? 'Invalid clock' : `Raw time ${Number(process.hrtime.bigint()) / 1e9}`);
    if (command === 'Get wall time') {
        // Count startup and periodic calendar refreshes across the main host and CPU runner
        Atomics.add(completion, 3, 1);
        // Supply a paired host instant and local offset without introducing a Wasm OS import
        if (process.env.WAHE_TEST_BAD_WALL_CLOCK) return allocateString('Invalid wall clock');
        const now = new Date();
        return allocateString(`Unix time ${now.getTime() / 1000} seconds\nUTC offset ${-now.getTimezoneOffset() * 60} seconds`);
    }
    if (command === 'Get audio frame specs') return allocateString('Sample count: 512');
    if (command === 'Mouse grab' || command === 'Mouse release') return 0;
    if (command.startsWith('Print ')) {
        // Keep meaningful emulator diagnostics visible while discarding allocator housekeeping
        fs.writeSync(1, command.slice(6) + '\n');
        return 0;
    }
    if (command.startsWith('Create thread with ')) {
        // Clone the module with private stack and TLS allocations while sharing its linear memory
        assert(isMainThread && !worker);
        const stackSize = 1024 * 1024;
        const stack = instance.exports.module_malloc(stackSize + 16);
        const tlsSize = instance.exports.__tls_size.value;
        const tlsAlign = instance.exports.__tls_align.value;
        const tls = instance.exports.module_malloc(tlsSize + tlsAlign);
        const message = allocateString(command.slice(command.indexOf('\n') + 1));
        workerAllocations = [stack, tls, message];
        worker = new Worker(new URL(import.meta.url), { workerData: {
            module, memory, completion: completion.buffer, fixturePaths,
            stack: (stack + stackSize) & ~15, tls: Math.ceil(tls / tlsAlign) * tlsAlign, message
        } });
        return allocateString('Thread created 1');
    }
    if (command === 'Join thread ID 1') {
        // Join without servicing the main event loop so shutdown must remain independent of UI activity
        const deadline = Date.now() + 30000;
        while (!Atomics.load(completion, 0) && Date.now() < deadline) Atomics.wait(completion, 0, 0, 100);
        assert.equal(Atomics.load(completion, 0), 1, 'CPU runner did not stop');
        assert.equal(Atomics.load(completion, 1), 0, 'CPU runner trapped');
        for (const pointer of workerAllocations) instance.exports.module_free(pointer);
        workerAllocations = [];
        return allocateString('Thread joined 1');
    }
    let match;
    if ((match = command.match(/^Open file \(([^)]+)\) at (.+)$/s))) {
        // Mount the real fixtures read-only and keep all NVRAM writes in memory
        const [, mode, path] = match;
        let file;
        if (fixturePaths.includes(path)) file = { fd: fs.openSync(path, 'r') };
        else if (path === 'memory-nvram' && mode.includes('w')) file = { data: Buffer.alloc(0) };
        else return 0;
        const handle = nextHandle++;
        files.set(handle, file);
        return allocateString(`File handle ${handle}`);
    }
    if ((match = command.match(/^Get size of file (\d+)$/))) {
        // Return precise provider sizes without exposing an operating-system descriptor to Wasm
        const file = files.get(+match[1]);
        return allocateString(`File size ${file.fd === undefined ? file.data.length : fs.fstatSync(file.fd).size} bytes`);
    }
    if ((match = command.match(/^Load from file (\d+) at 0x([\da-f]+) to location: (\d+) bytes at 0x([\da-f]+)$/))) {
        // Copy provider bytes directly into a bounds-checked module buffer
        const file = files.get(+match[1]), offset = parseInt(match[2], 16), count = +match[3], address = parseInt(match[4], 16);
        const target = new Uint8Array(memory.buffer, address, count);
        const loaded = file.fd === undefined ? Math.min(count, Math.max(0, file.data.length - offset)) : fs.readSync(file.fd, target, 0, count, offset);
        if (file.fd === undefined) target.set(file.data.subarray(offset, offset + loaded));
        ++readCount;
        return allocateString(`Loaded ${loaded} bytes`);
    }
    if ((match = command.match(/^Write to file (\d+) at 0x([\da-f]+) from location: (\d+) bytes at 0x([\da-f]+)$/))) {
        // Reject writes to fixtures and accept writes only to the synthetic NVRAM file
        const file = files.get(+match[1]), offset = parseInt(match[2], 16), count = +match[3], address = parseInt(match[4], 16);
        assert.equal(file.fd, undefined, 'Attempt to write a read-only fixture');
        if (file.data.length < offset + count) {
            // Grow only the test-owned memory file
            const replacement = Buffer.alloc(offset + count);
            file.data.copy(replacement);
            file.data = replacement;
        }
        file.data.set(new Uint8Array(memory.buffer, address, count), offset);
        return allocateString(`Wrote ${count} bytes, position ${(offset + count).toString(16)}`);
    }
    if ((match = command.match(/^Close file (\d+)$/))) {
        // Close descriptors on the same runner that owns the provider handle
        const file = files.get(+match[1]);
        if (file?.fd !== undefined) fs.closeSync(file.fd);
        files.delete(+match[1]);
        return 0;
    }
    throw Error(`Unexpected WAHE host command: ${command}`);
}

// Require a genuinely freestanding import surface and the complete WAHE lifecycle
assert.deepEqual(WebAssembly.Module.imports(module), [
    { module: 'env', name: 'memory', kind: 'memory' },
    { module: 'env', name: 'wahe_run_command', kind: 'function' }
]);
instance = new WebAssembly.Instance(module, { env: { memory, wahe_run_command: hostCommand } });

if (!isMainThread) {
    // Initialize private runner state before entering the CPU callback
    instance.exports.__stack_pointer.value = workerData.stack;
    instance.exports.__wasm_init_tls(workerData.tls);
    try {
        instance.exports.module_thread_entry(workerData.message);
        assert.equal(files.size, 0, 'Provider handles leaked during shutdown');
        Atomics.store(completion, 2, readCount);
    } catch (error) {
        // Publish traps independently of worker message delivery so a blocked join can observe them
        console.error(error);
        Atomics.store(completion, 1, 1);
    } finally {
        // Notify the joining host after the runner has left all Wasm callbacks
        Atomics.store(completion, 0, 1);
        Atomics.notify(completion, 0);
    }
} else {
    // Exercise construction, explicit error replies, configuration, capture and shared-heap allocation
    instance.exports._initialize();
    assert.equal(send('Status'), 'State: configured');
    assert.match(send('SheepShaver RAM 8'), /Error: Unknown or invalid/);
    assert.equal(send('SheepShaver RAM 64'), '');
    assert.equal(send('SheepShaver gamma table identity'), '');
    assert.match(send('SheepShaver mouse capture on'), /on/);
    assert.match(send('SheepShaver mouse capture off'), /off/);
    assert.match(send('Documentation'), /RGBA float linear/);
    // Exercise the manual baseline independently of an unavailable automatic wall time service
    if (process.env.WAHE_TEST_MANUAL_CLOCK) assert.equal(send('SheepShaver clock unix 1700000000'), '');
    if (fixturePaths.length === 2) {
        // Boot the actual Macintosh image while drawing and sound callbacks run independently
        send(`SheepShaver ROM path ${fixturePaths[0]}`);
        send(`SheepShaver disk path *${fixturePaths[1]}`);
        send('SheepShaver NVRAM path memory-nvram');
        assert.doesNotMatch(send('Init'), /Error:/);
        let nonblank = false, audioSeen = false, frames = 0;
        try {
            // Allow interpreted Wasm more startup time than the native CPU backend
            const deadline = Date.now() + 120000;
            while (Date.now() < deadline && (!nonblank || frames < 300)) {
                const status = send('Status');
                assert.doesNotMatch(status, /failed|Error:/);
                const frame = readString(instance.exports.module_draw(0));
                if (frame) {
                    // Validate normalized linear output before comparing pixels for guest activity
                    const location = frame.match(/Framebuffer location: (\d+) bytes at 0x([\da-f]+)/);
                    assert(location && frame.includes('RGBA float linear'), frame);
                    const pixels = new Float32Array(memory.buffer, parseInt(location[2], 16), +location[1] / 4);
                    for (let p = 0; p < pixels.length; p += 4) {
                        assert(Number.isFinite(pixels[p]) && pixels[p] >= 0 && pixels[p] <= 1);
                        assert.equal(pixels[p + 3], 1);
                        if (pixels[p] !== pixels[0] || pixels[p + 1] !== pixels[1] || pixels[p + 2] !== pixels[2]) nonblank = true;
                    }
                    ++frames;
                }
                if (readString(instance.exports.module_proc_sound(0)).includes('Channels: 2')) audioSeen = true;
                if (frames === 20) send('Key down: 82\nKey up: 82\nMouse delta 2 -1');
                await new Promise(resolve => setTimeout(resolve, 10));
            }
            assert(nonblank, 'Macintosh ROM never produced a nonblank framebuffer');
            assert(audioSeen, 'Audio callback never became available');
            // Require repeated wall time callbacks while the interpreted Macintosh is running
            assert(Atomics.load(completion, 3) >= 3, 'Guest calendar was not refreshed after startup');
            console.log(`PASS: real Macintosh boot, ${frames} Wasm frames, linear framebuffer, audio and input`);
        } finally {
            // Always join the CPU before the host releases fixture and module resources
            send('Deinit');
            assert(Atomics.load(completion, 2) > 10, 'Boot did not read the ROM and disk through callbacks');
        }
    } else {
        // Exercise explicit clock or asynchronous missing-ROM errors without private Macintosh fixtures
        assert.equal(fixturePaths.length, 0, 'Supply both ROM and disk paths');
        send('Init');
        const deadline = Date.now() + 10000;
        let status;
        do {
            // Allow startup failures to return from the worker before joining its completed runner
            await new Promise(resolve => setTimeout(resolve, 10));
            status = send('Status');
        } while (!status.includes('failed') && Date.now() < deadline);
        // Require clock failures to surface at startup while the explicit override bypasses wall time queries
        const expected = process.env.WAHE_TEST_BAD_CLOCK ? /Error: Host did not provide Get raw time/ :
            process.env.WAHE_TEST_BAD_WALL_CLOCK && !process.env.WAHE_TEST_MANUAL_CLOCK ? /Error: Host did not provide Get wall time/ : /Error: Cannot open ROM/;
        assert.match(status, expected);
        // Verify that choosing the manual baseline removes the wall time service dependency
        if (process.env.WAHE_TEST_MANUAL_CLOCK) assert.equal(Atomics.load(completion, 3), 0);
        send('Deinit');
        assert.match(send('Status'), /State: failed/);
    }
    console.log('PASS: freestanding WAHE Wasm imports, allocator, TLS, configuration and shutdown');
}
