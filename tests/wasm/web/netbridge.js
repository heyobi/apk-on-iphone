// The network for the core in a web view (linked with --post-js into build/web/lxweb.js):
// Emscripten's sockets (SOCKFS) normally speak WebSocket to a proxy; here every TCP socket
// is a connection the app opens and keeps (lx://host/net/...), reached with synchronous
// requests from the worker, as the terminal is. Names: Emscripten's getaddrinfo gives each
// host name a stand-in address (DNS.lookup_name); connect turns it back into the name, and
// the app resolves it. TLS stays in the guest (curl, apt, pip use their own OpenSSL).
(function () {
    if (typeof SOCKFS === 'undefined') return;
    function call(path, body) {
        var x = new XMLHttpRequest();
        x.open(body ? 'POST' : 'GET', 'lx://host/net/' + path, false);
        x.overrideMimeType('text/plain; charset=x-user-defined');
        x.send(body || null);
        var s = x.responseText, r = new Uint8Array(s.length);
        for (var i = 0; i < s.length; i++) r[i] = s.charCodeAt(i) & 255;
        return { status: x.getResponseHeader('X-Lx') || 'err', data: r };
    }
    function fail(errno) { throw new FS.ErrnoError(errno); }
    var ops = {
        poll: function (sock) {
            if (sock.lxId === undefined) return 0;
            var r = call('poll?id=' + sock.lxId);
            var m = 4;                                             /* POLLOUT: writes are queued by the app */
            if (r.status === 'data' || r.status === 'eof') m |= 1; /* POLLIN */
            if (r.status === 'eof') m |= 16;                       /* POLLHUP */
            if (r.status === 'err') m |= 8 | 16;                   /* POLLERR */
            return m;
        },
        ioctl: function (sock, request, arg) { return 0; },
        close: function (sock) { if (sock.lxId !== undefined) call('close?id=' + sock.lxId); sock.lxId = undefined; return 0; },
        bind: function () { fail(58); },                            /* ENOTSUP: no listening sockets through the bridge yet */
        listen: function () { fail(58); },
        accept: function () { fail(58); },
        connect: function (sock, addr, port) {
            if (sock.type !== 1) fail(5);                          /* TCP only: EAFNOSUPPORT */
            var host = DNS.lookup_addr(addr) || addr;
            var r = call('connect?host=' + encodeURIComponent(host) + '&port=' + port);
            if (r.status !== 'ok') fail(r.status === 'refused' ? 14 : r.status === 'timeout' ? 73 : 23);   /* ECONNREFUSED, ETIMEDOUT, EHOSTUNREACH (WASI numbers) */
            sock.lxId = new TextDecoder().decode(r.data);
            sock.daddr = addr; sock.dport = port;
        },
        getname: function (sock, peer) {
            return peer ? { addr: sock.daddr || '0.0.0.0', port: sock.dport || 0 } : { addr: '0.0.0.0', port: 0 };
        },
        sendmsg: function (sock, buffer, offset, length) {
            if (sock.lxId === undefined) fail(53);                 /* ENOTCONN */
            var r = call('send?id=' + sock.lxId, buffer.slice(offset, offset + length));
            if (r.status !== 'ok') fail(64);                       /* EPIPE */
            return length;
        },
        recvmsg: function (sock, length) {
            if (sock.lxId === undefined) fail(53);
            var r = call('recv?id=' + sock.lxId + '&max=' + length);
            if (r.status === 'data') return { buffer: r.data, addr: sock.daddr, port: sock.dport };
            if (r.status === 'eof') return null;
            if (r.status === 'again') fail(6);                     /* EAGAIN */
            fail(15);                                              /* ECONNRESET */
        },
    };
    SOCKFS.websocket_sock_ops = ops;
})();
