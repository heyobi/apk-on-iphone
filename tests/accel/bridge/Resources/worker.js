// Worker side of the bridge benchmark: synchronous calls into the app (what a guest
// engine's syscalls would use), then the translated benchmark (bench.wasm) under the
// web view's JIT, then SharedArrayBuffer + Atomics.wait round trips through the page.
'use strict';
const us = (t0, n) => +((performance.now() - t0) * 1000 / n).toFixed(1);

function call(n, body) {
    const x = new XMLHttpRequest();
    x.open(body ? 'POST' : 'GET', 'lx://host/sys?n=' + n, false);
    x.overrideMimeType('text/plain; charset=x-user-defined');
    x.send(body || null);
    return x.responseText;
}
function bodySeen(s) {                                     // first 8 response bytes: body size the app saw
    let v = 0;
    for (let k = 0; k < 6; k++) v += (s.charCodeAt(k) & 255) * 2 ** (8 * k);
    return (s.charCodeAt(7) & 255) === 255 ? -1 : v;
}

async function bench(R) {
    const meta = await (await fetch('bench.json')).json();
    const image = new Uint8Array(await (await fetch('bench.mem')).arrayBuffer());
    const bytes = await (await fetch('bench.wasm')).arrayBuffer();
    const mem = new WebAssembly.Memory({ initial: meta.mem_size / 65536 });
    new Uint8Array(mem.buffer).set(image);
    const base = BigInt(meta.base);
    let out = '';
    const sys = (n, a, b, c) => {
        if (n === 64n) { out += String.fromCharCode.apply(null, new Uint8Array(mem.buffer, Number(b - base), Number(c))); return c; }
        return -38n;
    };
    let t = performance.now();
    const inst = new WebAssembly.Instance(new WebAssembly.Module(bytes), { env: { sys, mem } });
    R.bench_compile_s = +((performance.now() - t) / 1000).toFixed(3);
    t = performance.now();
    inst.exports.run(meta.entry_slot, BigInt(meta.sp));
    R.bench_run_s = +((performance.now() - t) / 1000).toFixed(3);
    R.bench_output = out.trim();
}

onmessage = async e => {
    if (!e.data.go) return;
    const R = {};
    try {
        let t = performance.now();
        for (let i = 0; i < 2000; i++) call(8);
        R.sync_xhr_us = us(t, 2000);
        t = performance.now();
        for (let i = 0; i < 300; i++) call(65536);
        R.sync_xhr_64k_reply_us = us(t, 300);
        const body = new Uint8Array(4096);
        R.post_body_bytes_seen_by_app = bodySeen(call(8, body));
        t = performance.now();
        for (let i = 0; i < 300; i++) call(8, body);
        R.sync_xhr_4k_post_us = us(t, 300);
    } catch (err) { R.xhr_error = String(err); }
    try { await bench(R); } catch (err) { R.bench_error = String(err); }
    if (e.data.sab) {
        const flag = new Int32Array(e.data.sab);
        const t = performance.now();
        for (let i = 0; i < 1000; i++) {
            Atomics.store(flag, 0, 0);
            postMessage({ call: i });
            Atomics.wait(flag, 0, 0, 5000);
        }
        R.sab_atomics_via_page_us = us(t, 1000);
    }
    postMessage(R);
};
