// The core (build/web/lxweb.js) in a web worker: unpacks rootfs.tar.gz into the module's
// in-memory file system at /r and runs Linux programs from it with lxrun's arguments.
'use strict';
importScripts('lxweb.js');

async function rootfsTar(name) {
    const res = await fetch(name);
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

// The module's file system under `root` as an uncompressed tar (ustar + GNU long names):
// what goes to OPFS, and comes back with untar() on the next launch.
function tarFS(FS, root) {
    const enc = new TextEncoder(), chunks = [];
    let total = 0;
    const push = b => { chunks.push(b); total += b.length; };
    const header = (name, mode, size, type, link) => {
        const h = new Uint8Array(512);
        const put = (o, n, str) => h.set(enc.encode(str).subarray(0, n), o);
        const oct = (o, n, v) => put(o, n, v.toString(8).padStart(n - 1, '0') + '\0');
        put(0, 100, name); oct(100, 8, mode & 0o7777); oct(108, 8, 0); oct(116, 8, 0);
        oct(124, 12, size); oct(136, 12, 0); h.fill(32, 148, 156); h[156] = type.charCodeAt(0);
        put(157, 100, link || ''); put(257, 6, 'ustar\0'); put(263, 2, '00');
        let sum = 0; for (const b of h) sum += b;
        put(148, 8, sum.toString(8).padStart(6, '0') + '\0 ');
        return h;
    };
    const entry = (name, mode, data, type, link) => {
        const nb = enc.encode(name);
        if (nb.length > 99) {                                     // GNU long name
            push(header('././@LongLink', 0o644, nb.length + 1, 'L'));
            const pad = new Uint8Array(Math.ceil((nb.length + 1) / 512) * 512); pad.set(nb); push(pad);
        }
        push(header(name, mode, data ? data.length : 0, type, link));
        if (data && data.length) { const pad = new Uint8Array(Math.ceil(data.length / 512) * 512); pad.set(data); push(pad); }
    };
    const walk = rel => {
        for (const n of FS.readdir(root + rel)) {
            if (n === '.' || n === '..') continue;
            const path = rel + '/' + n, full = root + path, st = FS.lstat(full), name = path.slice(1);
            if (FS.isLink(st.mode)) entry(name, st.mode, null, '2', FS.readlink(full));
            else if (FS.isDir(st.mode)) { entry(name + '/', st.mode, null, '5'); walk(path); }
            else if (FS.isFile(st.mode)) entry(name, st.mode, FS.readFile(full), '0');
        }
    };
    walk('');
    push(new Uint8Array(1024));
    const out = new Uint8Array(total);
    let o = 0; for (const c of chunks) { out.set(c, o); o += c.length; }
    return out;
}

// The terminal bridge (lxrun -i in the wasm build calls this from the scheduler's host turn):
// the guest's output goes to the app in the body of a synchronous request, the typed keys
// come back in the answer; the app may hold the answer up to waitMs when no key is due.
var ttySeq = 0;
function lxPoll(out, waitMs) {
    let total = 0;
    for (const c of out) total += c.length;
    const body = new Uint8Array(total);
    let o = 0;
    for (const c of out) { body.set(c, o); o += c.length; }
    const x = new XMLHttpRequest();
    x.open('POST', 'lx://host/tty?wait=' + waitMs + '&n=' + (++ttySeq), false);
    x.overrideMimeType('text/plain; charset=x-user-defined');
    x.send(body);
    const s = x.responseText;
    if (!s.length) return null;
    const r = new Uint8Array(s.length);
    for (let i = 0; i < s.length; i++) r[i] = s.charCodeAt(i) & 255;
    return r;
}

async function interactive(tar, args) {
    let log = '';
    const M = await createLx({ print: s => { log += s + '\n'; }, printErr: s => { log += s + '\n'; }, lxPoll });
    untar(M.FS, tar, '/r');
    const t0 = performance.now();
    let rc;
    try { rc = M.callMain(['-i', '/r'].concat(args)); } catch (e) { rc = e && e.status !== undefined ? e.status : String(e); }
    return { rc, run_s: +((performance.now() - t0) / 1000).toFixed(1), log: log.slice(-800) };
}

async function opfsFile(create) {
    const dir = await navigator.storage.getDirectory();
    const fh = await dir.getFileHandle('root.tar', { create });
    return fh.createSyncAccessHandle();
}

async function run(tar, args, keep, env) {
    let out = '';
    const M = await createLx({ print: s => { out += s + '\n'; }, printErr: s => { out += s + '\n'; },
                               preRun: [m => { m.ENV.LXRUN_STATS = '1'; if (env) Object.assign(m.ENV, env); }] });
    const t0 = performance.now();
    untar(M.FS, tar, '/r');
    if (M.BRIDGEFS) { try { M.FS.mkdirTree('/r/root/Files'); M.FS.mount(M.BRIDGEFS, {}, '/r/root/Files'); } catch (e) {} }
    const t1 = performance.now();
    let rc;
    try { rc = M.callMain(['/r'].concat(args)); } catch (e) { rc = e && e.status !== undefined ? e.status : String(e); }
    const r = { rc, out: out.slice(-1500), untar_s: +((t1 - t0) / 1000).toFixed(2), run_s: +((performance.now() - t1) / 1000).toFixed(2) };
    if (M.lxNetLog) r.net = M.lxNetLog.slice(0, 60).join(' | ');
    if (keep) r.M = M;
    return r;
}

onmessage = async e => {
    if (!e.data.go) return;
    const R = {};
    try {
        // second launch: the root saved in OPFS last time
        let h = null;
        try { h = await opfsFile(false); } catch (err) {}
        if (h && h.getSize() > 0) {
            let t = performance.now();
            const buf = new Uint8Array(h.getSize()); h.read(buf, { at: 0 }); h.close();
            R.opfs_root_mb = +(buf.length / 1e6).toFixed(1);
            R.opfs_read_s = +((performance.now() - t) / 1000).toFixed(2);
            R.persisted = await run(buf, ['/bin/bash', '-c', 'cat /root/persist.txt; echo bash ${BASH_VERSINFO[0]} from OPFS']);
            postMessage({ log: 'interactive bash' });
            R.interactive = await interactive(buf, ['/bin/bash', '-i']);
            postMessage(R);
            return;
        }
        if (h) h.close();
        let t = performance.now();
        const tar = await rootfsTar('rootfs.tar.gz');
        R.rootfs_mb = +(tar.length / 1e6).toFixed(1);
        R.gunzip_s = +((performance.now() - t) / 1000).toFixed(2);
        const checks = [
            ['echo', ['/bin/busybox', 'echo', 'hello from Linux in WKWebView']],
            ['sha256sum', ['/bin/busybox', 'sha256sum', '/etc/os-release']],
            ['pipeline', ['/bin/busybox', 'sh', '-c', 'printf "b\nz\na\n" | sort | tail -n 1; ls /etc | wc -l']],
            ['background', ['/bin/busybox', 'sh', '-c', 'sleep 0.2 & wait; echo bg-ok; (exit 3); echo rc=$?']],
            ['sigpipe', ['/bin/busybox', 'sh', '-c', 'yes | head -n 2 >/dev/null; echo sigpipe-ok']],
            ['bench', ['/accel/bench']],
        ];
        for (const [name, args] of checks) { postMessage({ log: 'running ' + name }); R[name] = await run(tar, args); }

        t = performance.now();
        const deb = await rootfsTar('rootfs-debian.tar.gz');
        R.debian_mb = +(deb.length / 1e6).toFixed(1);
        R.debian_gunzip_s = +((performance.now() - t) / 1000).toFixed(2);
        const dchecks = [
            ['deb_bash', ['/bin/bash', '-c', 'echo bash ${BASH_VERSINFO[0]}; cat /etc/debian_version; ls /etc | head -n 3 | wc -l']],
            ['deb_perl', ['/usr/bin/perl', '-e', 'print "perl ok\n"']],
            ['deb_dpkg', ['/usr/bin/dpkg', '-l', 'bash']],
            ['deb_apt', ['/usr/bin/apt-get', '--version']],
        ];
        for (const [name, args] of dchecks) { postMessage({ log: 'running ' + name }); R[name] = await run(deb, args); }
        // Python under JavaScriptCore (the web view's engine), with and without the accelerator
        const py = 'print(sum(i for i in range(1000000)))';
        const pj = 'import json,re\nd=[{"k":i,"v":str(i)*3} for i in range(20000)]\ns=json.dumps(d); x=json.loads(s); n=len(re.findall(r"[0-9]+",s)); print("done",n)';
        for (const [name, args, env] of [
            ['py_start_jit', ['/usr/bin/python3', '-c', 'pass'], null], ['py_start_interp', ['/usr/bin/python3', '-c', 'pass'], { AOI_JIT: '0' }],
            ['py_start_hot200k', ['/usr/bin/python3', '-c', 'pass'], { AOI_JIT_HOT: '200000' }], ['py_start_hot1m', ['/usr/bin/python3', '-c', 'pass'], { AOI_JIT_HOT: '1000000' }],
            ['ls_jit', ['/bin/ls', '-la', '/usr/bin'], null], ['ls_interp', ['/bin/ls', '-la', '/usr/bin'], { AOI_JIT: '0' }],
            ['py_loop_hot200k', ['/usr/bin/python3', '-c', py], { AOI_JIT_HOT: '200000' }],
            ['py_loop_jit', ['/usr/bin/python3', '-c', py], null], ['py_loop_interp', ['/usr/bin/python3', '-c', py], { AOI_JIT: '0' }],
            ['py_json_jit', ['/usr/bin/python3', '-c', pj], null], ['py_json_interp', ['/usr/bin/python3', '-c', pj], { AOI_JIT: '0' }]]) {
            postMessage({ log: 'running ' + name });
            R[name] = await run(deb, args, false, env);
        }
        // the network through the app (netbridge.js): a name, plain HTTP from Python, apt
        for (const [name, args] of [
            ['net_getent', ['/usr/bin/getent', 'hosts', 'deb.debian.org']],
            ['net_ahosts', ['/usr/bin/getent', 'ahosts', 'deb.debian.org']],
            ['net_ahostsv4', ['/usr/bin/getent', 'ahostsv4', 'deb.debian.org']],
            ['net_http', ['/usr/bin/python3', '-c', 'import urllib.request as u; r = u.urlopen("http://deb.debian.org/debian/dists/trixie/Release", timeout=30); print("http", r.status, len(r.read()))']],
            ['net_apt', ['/bin/sh', '-c', 'apt-get update 2>&1 | tail -n 5']],
            ['files', ['/bin/bash', '-c', 'cd /root/Files && echo hello from Linux > hello.txt && mkdir -p d && cp hello.txt d/copy.txt && mv hello.txt d/moved.txt && ls -la . d && cat d/copy.txt && wc -c d/moved.txt && python3 -c "open(\'/root/Files/big.bin\',\'wb\').write(bytes(range(256))*4096); print(len(open(\'/root/Files/big.bin\',\'rb\').read()))"']]]) {
            postMessage({ log: 'running ' + name });
            R[name] = await run(deb, args);
        }

        // write a file in Debian, save the whole root to OPFS for the next launch
        const last = await run(deb, ['/bin/bash', '-c', 'echo "written on the first launch: $(date -u +%H:%M:%S)" > /root/persist.txt; cat /root/persist.txt'], true);
        R.deb_write = { rc: last.rc, out: last.out };
        t = performance.now();
        const img = tarFS(last.M.FS, '/r');
        R.save_tar_mb = +(img.length / 1e6).toFixed(1);
        R.save_tar_s = +((performance.now() - t) / 1000).toFixed(2);
        t = performance.now();
        const w = await opfsFile(true);
        w.truncate(0); w.write(img, { at: 0 }); w.flush(); w.close();
        R.save_opfs_s = +((performance.now() - t) / 1000).toFixed(2);
    } catch (err) { R.error = String(err && err.stack || err); }
    postMessage(R);
};
