// Runs a program translated by a64wasm.py in a JavaScript engine: Node (V8) or jsc
// (JavaScriptCore, WebKit's engine, the one WKWebView uses on iOS).
// usage: node run.js DIR    |    jsc run.js -- DIR     (DIR holds bench.wasm/json/mem)
'use strict';
const isNode = typeof process !== 'undefined' && process.versions && process.versions.node;
let readBin, readText, out, now, args;
if (isNode) {
    const fs = require('fs');
    readBin = p => new Uint8Array(fs.readFileSync(p));
    readText = p => fs.readFileSync(p, 'utf8');
    out = s => process.stdout.write(s + '\n');
    now = () => performance.now();
    args = process.argv.slice(2);
} else {
    readBin = p => new Uint8Array(read(p, 'binary'));
    readText = p => read(p);
    out = s => print(s);
    now = () => (typeof preciseTime === 'function' ? preciseTime() * 1000 : Date.now());
    args = scriptArgs;
}
const dir = args[0];
const meta = JSON.parse(readText(dir + '/bench.json'));
const image = readBin(dir + '/bench.mem');
const wasm = readBin(dir + '/bench.wasm');
const mem = new WebAssembly.Memory({ initial: meta.mem_size / 65536 });
new Uint8Array(mem.buffer).set(image);
const base = BigInt(meta.base);
let printed = '';
function sys(n, a, b, c) {
    if (n === 64n) {
        const bytes = new Uint8Array(mem.buffer, Number(b - base), Number(c));
        printed += String.fromCharCode.apply(null, bytes);
        return c;
    }
    return -38n;
}
const t0 = now();
const mod = new WebAssembly.Module(wasm);
const inst = new WebAssembly.Instance(mod, { env: { sys, mem } });
const t1 = now();
const rc = inst.exports.run(meta.entry_slot, BigInt(meta.sp));
const t2 = now();
out('output: ' + printed.trim() + '  exit ' + rc);
out('compile ' + ((t1 - t0) / 1000).toFixed(3) + ' s, run ' + ((t2 - t1) / 1000).toFixed(3) + ' s');
