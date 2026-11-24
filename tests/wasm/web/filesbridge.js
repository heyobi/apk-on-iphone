// A folder shared with the iOS Files app (linked with --post-js into build/web/lxweb.js):
// an Emscripten file system whose files live in the app (Documents/Files), reached with
// synchronous requests to lx://host/fs/..., mounted by the worker at the guest's
// /root/Files. Everything else stays in the in-memory root.
(function () {
    if (typeof FS === 'undefined') return;
    var seq = 0;
    var ERR = { noent: 44, exist: 20, notdir: 54, isdir: 31, notempty: 55, acces: 2, inval: 28, io: 29 };
    function call(op, path, extra, body) {
        var x = new XMLHttpRequest();
        seq++;
        x.open(body ? 'POST' : 'GET', 'lx://host/fs/' + op + '?p=' + encodeURIComponent(path) + (extra || '') + '&n=' + seq, false);
        x.overrideMimeType('text/plain; charset=x-user-defined');
        x.send(body || null);
        var st = x.getResponseHeader('X-Lx') || 'io';
        if (st !== 'ok') throw new FS.ErrnoError(ERR[st] || 29);
        var s = x.responseText, r = new Uint8Array(s.length);
        for (var i = 0; i < s.length; i++) r[i] = s.charCodeAt(i) & 255;
        return r;
    }
    function text(r) { return new TextDecoder().decode(r); }
    function realPath(node) {
        var parts = [];
        while (node.parent !== node) { parts.push(node.name); node = node.parent; }
        parts.reverse();
        return '/' + parts.join('/');
    }
    function stat(path) { return JSON.parse(text(call('stat', path))); }   /* {mode, size, mtime} */
    var B = {
        mount: function (mount) { return B.createNode(null, '/', 16384 | 511, 0); },
        createNode: function (parent, name, mode, dev) {
            var node = FS.createNode(parent, name, mode, dev);
            node.node_ops = B.node_ops;
            node.stream_ops = B.stream_ops;
            return node;
        },
        node_ops: {
            getattr: function (node) {
                var st = stat(realPath(node)), t = new Date(st.mtime * 1000);
                return { dev: 1, ino: node.id, mode: st.mode, nlink: 1, uid: 0, gid: 0, rdev: 0, size: st.size,
                         atime: t, mtime: t, ctime: t, blksize: 4096, blocks: Math.ceil(st.size / 512) };
            },
            setattr: function (node, attr) {
                if (attr.size !== undefined) call('truncate', realPath(node), '&size=' + attr.size);
            },
            lookup: function (parent, name) {
                var path = realPath(parent) + (parent.parent === parent ? '' : '/') + name, st;
                try { st = stat(path); } catch (e) { throw new FS.ErrnoError(44); }
                return B.createNode(parent, name, st.mode, 0);
            },
            mknod: function (parent, name, mode, dev) {
                var path = realPath(parent) + (parent.parent === parent ? '' : '/') + name;
                call(FS.isDir(mode) ? 'mkdir' : 'create', path);
                return B.createNode(parent, name, mode, dev);
            },
            rename: function (old, newDir, newName) {
                var to = realPath(newDir) + (newDir.parent === newDir ? '' : '/') + newName;
                call('rename', realPath(old), '&to=' + encodeURIComponent(to));
                old.name = newName;
            },
            unlink: function (parent, name) { call('unlink', realPath(parent) + (parent.parent === parent ? '' : '/') + name); },
            rmdir: function (parent, name) { call('rmdir', realPath(parent) + (parent.parent === parent ? '' : '/') + name); },
            readdir: function (node) { return ['.', '..'].concat(text(call('readdir', realPath(node))).split('\n').filter(Boolean)); },
            symlink: function () { throw new FS.ErrnoError(63); },
            readlink: function () { throw new FS.ErrnoError(28); },
        },
        stream_ops: {
            open: function (stream) {},
            close: function (stream) {},
            read: function (stream, buffer, offset, length, position) {
                if (!length) return 0;
                var r = call('read', realPath(stream.node), '&off=' + position + '&len=' + length);
                buffer.set(r, offset);
                return r.length;
            },
            write: function (stream, buffer, offset, length, position) {
                if (!length) return 0;
                call('write', realPath(stream.node), '&off=' + position, new Uint8Array(buffer.buffer, buffer.byteOffset + offset, length).slice());
                return length;
            },
            llseek: function (stream, offset, whence) {
                var pos = offset;
                if (whence === 1) pos += stream.position;
                else if (whence === 2) pos += stat(realPath(stream.node)).size;
                if (pos < 0) throw new FS.ErrnoError(28);
                return pos;
            },
        },
    };
    Module.BRIDGEFS = B;
})();
