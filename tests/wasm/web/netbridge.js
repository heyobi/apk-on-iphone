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
    function log(m) { (Module.lxNetLog = Module.lxNetLog || []); if (Module.lxNetLog.length < 120) Module.lxNetLog.push(m); }
    /* DNS over UDP (glibc's resolver talks to the nameserver itself): the query's name is
     * resolved by the app (its getaddrinfo: the phone's DNS and VPN settings) and the answer
     * built here as a DNS message. */
    function dnsAnswer(q) {
        if (q.length < 12) return null;
        var i = 12, labels = [];
        while (i < q.length && q[i]) { labels.push(new TextDecoder().decode(q.subarray(i + 1, i + 1 + q[i]))); i += q[i] + 1; }
        var qend = i + 5;                                          /* the 0 label, type, class */
        if (qend > q.length) return null;
        var qtype = (q[i + 1] << 8) | q[i + 2];
        log('dns id ' + ((q[0] << 8) | q[1]) + ' ' + labels.join('.') + ' type ' + qtype + ' qd ' + ((q[4] << 8) | q[5]) + ' ar ' + ((q[10] << 8) | q[11]) + ' len ' + q.length);
        var ips = [];
        if (qtype === 1 || qtype === 28) {
            var r = call('resolve?host=' + encodeURIComponent(labels.join('.')) + '&type=' + (qtype === 1 ? 'A' : 'AAAA'));
            if (r.status === 'ok') ips = new TextDecoder().decode(r.data).split('\n').filter(Boolean)
                .filter(function (ip) { return qtype === 1 ? ip.indexOf(':') < 0 : ip.indexOf('.') < 0; });   /* no v4-mapped */
        }
        var parts = [];
        ips.forEach(function (ip) {
            var raw = [];
            if (qtype === 1) raw = ip.split('.').map(Number);
            else {
                var h = ip.split('::'), a = h[0] ? h[0].split(':') : [], b = h.length > 1 && h[1] ? h[1].split(':') : [];
                var groups = a.concat(new Array(8 - a.length - b.length).fill('0'), b);
                groups.forEach(function (g) { var v = parseInt(g || '0', 16); raw.push(v >> 8, v & 255); });
            }
            parts.push([0xc0, 12, qtype >> 8, qtype & 255, 0, 1, 0, 0, 0, 60, 0, raw.length].concat(raw));
        });
        log('  answer ' + ips.length + ': ' + ips.slice(0, 2).join(' '));
        var out = [q[0], q[1], 0x81, ips.length || qtype === 28 ? 0x80 : 0x83, 0, 1, 0, ips.length, 0, 0, 0, 0];
        for (var k = 12; k < qend; k++) out.push(q[k]);
        parts.forEach(function (p) { out = out.concat(p); });
        return new Uint8Array(out);
    }
    var ops = {
        poll: function (sock) {
            if (sock.lxDns) return 4 | (sock.lxQ.length ? 1 : 0);
            if (sock.lxId === undefined) return 0;
            var r = call('poll?id=' + sock.lxId);
            if ((sock.lxPolls = (sock.lxPolls || 0) + 1) <= 5 || r.status !== 'again') log('poll ' + sock.lxId + ' ' + r.status);
            var m = 4;                                             /* POLLOUT: writes are queued by the app */
            if (r.status === 'data' || r.status === 'eof') m |= 1; /* POLLIN */
            if (r.status === 'eof') m |= 16;                       /* POLLHUP */
            if (r.status === 'err') m |= 8 | 16;                   /* POLLERR */
            return m;
        },
        ioctl: function (sock, request, arg) {
            if (request === 21531) {                               /* FIONREAD: glibc's resolver sizes its buffer with it */
                var n = 0;
                if (sock.lxDns) n = sock.lxQ.length ? sock.lxQ[0].length : 0;
                else if (sock.lxId !== undefined) { var r = call('avail?id=' + sock.lxId); n = parseInt(new TextDecoder().decode(r.data) || '0', 10) || 0; }
                HEAP32[arg >> 2] = n;
                return 0;
            }
            return 0;
        },
        close: function (sock) { if (sock.lxId !== undefined) call('close?id=' + sock.lxId); sock.lxId = undefined; return 0; },
        bind: function () { fail(58); },                            /* ENOTSUP: no listening sockets through the bridge yet */
        listen: function () { fail(58); },
        accept: function () { fail(58); },
        connect: function (sock, addr, port) {
            log('connect type ' + sock.type + ' ' + addr + ':' + port);
            if (sock.type === 2) {                                 /* UDP: only DNS (port 53), answered here */
                if (port !== 53) fail(5);
                sock.lxDns = true; sock.lxQ = sock.lxQ || []; sock.daddr = addr; sock.dport = port;
                return;
            }
            if (sock.type !== 1) fail(5);
            var host = DNS.lookup_addr(addr) || addr;
            var r = call('connect?host=' + encodeURIComponent(host) + '&port=' + port);
            log('tcp connect ' + host + ':' + port + ' -> ' + r.status + ' ' + new TextDecoder().decode(r.data));
            if (r.status !== 'ok') fail(r.status === 'refused' ? 14 : r.status === 'timeout' ? 73 : 23);   /* ECONNREFUSED, ETIMEDOUT, EHOSTUNREACH (WASI numbers) */
            sock.lxId = new TextDecoder().decode(r.data);
            sock.daddr = addr; sock.dport = port;
        },
        getname: function (sock, peer) {
            return peer ? { addr: sock.daddr || '0.0.0.0', port: sock.dport || 0 } : { addr: '0.0.0.0', port: 0 };
        },
        sendmsg: function (sock, buffer, offset, length, addr, port) {
            if (sock.type === 2) log('send udp ' + length + ' to ' + addr + ':' + port);
            if (sock.type === 2) {
                if (port !== undefined && port !== 53) fail(5);
                if (!sock.lxQ) { sock.lxQ = []; sock.lxDns = true; sock.daddr = addr; sock.dport = 53; }
                var ans = dnsAnswer(buffer.slice(offset, offset + length));
                if (ans) sock.lxQ.push(ans);
                return length;
            }
            if (sock.lxId === undefined) fail(53);                 /* ENOTCONN */
            var r = call('send?id=' + sock.lxId, new Uint8Array(buffer.buffer, buffer.byteOffset + offset, length).slice());
            log('tcp send ' + sock.lxId + ' ' + length + ' -> ' + r.status + ' ' + new TextDecoder().decode(r.data));
            if (r.status !== 'ok') fail(64);                       /* EPIPE */
            return length;
        },
        recvmsg: function (sock, length) {
            if (sock.lxDns) log('recv dns len ' + length + ', queued ' + sock.lxQ.length + (sock.lxQ.length ? ' id ' + ((sock.lxQ[0][0] << 8) | sock.lxQ[0][1]) : ''));
            if (sock.lxDns) {
                if (!sock.lxQ.length) fail(6);
                return { buffer: sock.lxQ.shift(), addr: sock.daddr, port: 53 };
            }
            if (sock.lxId === undefined) fail(53);
            var r = call('recv?id=' + sock.lxId + '&max=' + length);
            if (r.status !== 'again') log('tcp recv ' + sock.lxId + ' ' + r.status + ' ' + r.data.length);
            if (r.status === 'data') return { buffer: r.data, addr: sock.daddr, port: sock.dport };
            if (r.status === 'eof') return null;
            if (r.status === 'again') fail(6);                     /* EAGAIN */
            fail(15);                                              /* ECONNRESET */
        },
    };
    SOCKFS.websocket_sock_ops = ops;
})();
