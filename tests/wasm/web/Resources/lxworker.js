// The core (build/web/lxweb.js) in a web worker: unpacks rootfs.tar.gz into the module's
// in-memory file system at /r and runs Linux programs from it with lxrun's arguments.
'use strict';
importScripts('lxweb.js');

async function rootfsTar() {
    const res = await fetch('rootfs.tar.gz');
    const stream = res.body.pipeThrough(new DecompressionStream('gzip'));
    return new Uint8Array(await new Response(stream).arrayBuffer());
}

// ustar / GNU tar into Emscripten's FS under `root` (files, directories, symlinks,
// hard links as copies, GNU long names)
function untar(FS, tar, root) {
    const dec = new TextDecoder();
    const str = (o, n) => { let e = o; while (e < o + n && tar[e]) e++; return dec.decode(tar.subarray(o, e)); };
    const oct = (o, n) => parseInt(str(o, n).trim() || '0', 8);
    let off = 0, longName = null, files = 0;
    const links = [];
    while (off + 512 <= tar.length) {
        if (tar[off] === 0 && tar[off + 1] === 0) break;
        let name = str(off, 100);
        const prefix = str(off + 345, 155);
        if (prefix && str(off + 257, 5) === 'ustar') name = prefix + '/' + name;
        const mode = oct(off + 100, 8), size = oct(off + 124, 12), type = String.fromCharCode(tar[off + 156] || 48);
        const link = str(off + 157, 100);
        const data = off + 512;
        off = data + Math.ceil(size / 512) * 512;
        if (type === 'L') { longName = str(data, size); continue; }
        if (longName) { name = longName; longName = null; }
        name = name.replace(/^\.\//, '').replace(/\/$/, '');
        if (!name || name === '.') continue;
        const path = root + '/' + name;
        const parent = path.slice(0, path.lastIndexOf('/'));
        try { FS.mkdirTree(parent); } catch (e) {}
        try {
            if (type === '5') { try { FS.mkdir(path); } catch (e) {} FS.chmod(path, mode); }
            else if (type === '2') FS.symlink(link, path);
            else if (type === '1') links.push([root + '/' + link.replace(/^\.\//, ''), path]);
            else if (type === '0' || type === '7') { FS.writeFile(path, tar.subarray(data, data + size)); FS.chmod(path, mode); files++; }
        } catch (e) { /* (an entry the FS refuses: keep going) */ }
    }
    for (const [from, to] of links) { try { FS.writeFile(to, FS.readFile(from)); FS.chmod(to, FS.stat(from).mode); } catch (e) {} }
    return files;
}

async function run(tar, args) {
    let out = '';
    const M = await createLx({ print: s => { out += s + '\n'; }, printErr: s => { out += s + '\n'; } });
    const t0 = performance.now();
    untar(M.FS, tar, '/r');
    const t1 = performance.now();
    let rc;
    try { rc = M.callMain(['/r'].concat(args)); } catch (e) { rc = e && e.status !== undefined ? e.status : String(e); }
    return { rc, out: out.slice(-1500), untar_s: +((t1 - t0) / 1000).toFixed(2), run_s: +((performance.now() - t1) / 1000).toFixed(2) };
}

onmessage = async e => {
    if (!e.data.go) return;
    const R = {};
    try {
        let t = performance.now();
        const tar = await rootfsTar();
        R.rootfs_mb = +(tar.length / 1e6).toFixed(1);
        R.gunzip_s = +((performance.now() - t) / 1000).toFixed(2);
        const checks = [
            ['echo', ['/bin/busybox', 'echo', 'hello from Linux in WKWebView']],
            ['ls', ['/bin/busybox', 'ls', '/']],
            ['os-release', ['/bin/busybox', 'cat', '/etc/os-release']],
            ['sha256sum', ['/bin/busybox', 'sha256sum', '/etc/os-release']],
            ['pipeline', ['/bin/busybox', 'sh', '-c', 'printf "b\\nz\\na\\n" | sort | tail -n 1; ls /etc | wc -l']],
            ['background', ['/bin/busybox', 'sh', '-c', 'sleep 0.2 & wait; echo bg-ok; (exit 3); echo rc=$?']],
            ['sigpipe', ['/bin/busybox', 'sh', '-c', 'yes | head -n 2 >/dev/null; echo sigpipe-ok']],
            ['bench', ['/accel/bench']],
        ];
        for (const [name, args] of checks) {
            postMessage({ log: 'running ' + name });
            R[name] = await run(tar, args);
        }
    } catch (err) { R.error = String(err && err.stack || err); }
    postMessage(R);
};
