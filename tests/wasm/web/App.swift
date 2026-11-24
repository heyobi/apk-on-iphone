// LxWeb test app: the page (Resources/index.html) runs the wasm32 core in a worker; the same
// URL scheme handler and "done" message as tests/accel/bridge (BridgeBench), which it copies.
import UIKit
import WebKit

/// The terminal's side in this test: the guest's output arrives in the body of each /tty
/// request (written to Documents/tty.txt); the answer carries keys from a script, typed at
/// fixed times. With no key due the answer waits (up to ?wait= ms), so an idle guest costs
/// no busy polling.
final class Terminal {
    static let shared = Terminal()
    let start = Date()
    var transcript = Data()
    var requests = 0
    var script: [(Double, String)] = [
        (2, "echo hi from bash in WKWebView\r"), (4, "ls / | head -n 3\r"),
        (6, "sleep 30\r"), (8, "\u{03}"), (9.5, "echo after-ctrl-c $?\r"),
        (11, "for i in 1 2 3; do echo line $i; done | tac\r"), (13, "exit\r"),
    ]
    func due() -> Data {
        let t = Date().timeIntervalSince(start)
        var out = Data()
        while let first = script.first, first.0 <= t { out.append(first.1.data(using: .utf8)!); script.removeFirst() }
        return out
    }
    func nextIn() -> Double? { script.first.map { max(0, $0.0 - Date().timeIntervalSince(start)) } }
    func save() {
        let docs = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
        try? transcript.write(to: docs.appendingPathComponent("tty.txt"))
        try? "\(requests)".write(to: docs.appendingPathComponent("tty-requests.txt"), atomically: true, encoding: .utf8)
    }
}

final class Scheme: NSObject, WKURLSchemeHandler {
    func webView(_ webView: WKWebView, start task: WKURLSchemeTask) {
        guard let url = task.request.url else { return }
        if url.path == "/tty" {
            let term = Terminal.shared
            term.requests += 1
            if let b = task.request.httpBody, !b.isEmpty { term.transcript.append(b) }
            term.save()
            let wait = Double(URLComponents(url: url, resolvingAgainstBaseURL: false)?.queryItems?.first { $0.name == "wait" }?.value ?? "0") ?? 0
            let reply = {
                let keys = term.due()
                task.didReceive(HTTPURLResponse(url: url, statusCode: 200, httpVersion: "HTTP/1.1",
                                                headerFields: ["Content-Type": "application/octet-stream", "Content-Length": "\(keys.count)"])!)
                task.didReceive(keys)
                task.didFinish()
            }
            if wait <= 0 || (term.nextIn() ?? 1) <= 0 { reply(); return }
            let delay = min(wait / 1000, term.nextIn() ?? wait / 1000)
            DispatchQueue.main.asyncAfter(deadline: .now() + delay, execute: reply)
            return
        }
        var data = Data()
        var type = "application/octet-stream"
        if url.path == "/sys" {
            // a stand-in syscall: answer n bytes; the first 8 say how big the request body was (-1: none)
            let n = Int(URLComponents(url: url, resolvingAgainstBaseURL: false)?.queryItems?.first { $0.name == "n" }?.value ?? "8") ?? 8
            var body = Int64(task.request.httpBody?.count ?? -1)
            if body < 0, let s = task.request.httpBodyStream {
                s.open(); var buf = [UInt8](repeating: 0, count: 65536); var total = 0
                while s.hasBytesAvailable { let k = s.read(&buf, maxLength: buf.count); if k <= 0 { break }; total += k }
                s.close(); body = Int64(total)
            }
            data = Data(count: max(n, 8))
            withUnsafeBytes(of: body.littleEndian) { data.replaceSubrange(0..<8, with: $0) }
        } else {
            let name = String(url.path.dropFirst())
            if let u = Bundle.main.url(forResource: name, withExtension: nil), let d = try? Data(contentsOf: u) { data = d }
            if name.hasSuffix(".html") { type = "text/html" } else if name.hasSuffix(".js") { type = "text/javascript" }
            else if name.hasSuffix(".wasm") { type = "application/wasm" } else if name.hasSuffix(".json") { type = "application/json" }
        }
        let headers = ["Content-Type": type, "Content-Length": "\(data.count)",
                       "Cross-Origin-Opener-Policy": "same-origin", "Cross-Origin-Embedder-Policy": "require-corp",
                       "Cross-Origin-Resource-Policy": "same-origin", "Access-Control-Allow-Origin": "*"]
        task.didReceive(HTTPURLResponse(url: url, statusCode: 200, httpVersion: "HTTP/1.1", headerFields: headers)!)
        task.didReceive(data)
        task.didFinish()
    }
    func webView(_ webView: WKWebView, stop task: WKURLSchemeTask) {}
}

final class Bridge: NSObject, WKScriptMessageHandler, WKScriptMessageHandlerWithReply, WKUIDelegate {
    func userContentController(_ c: WKUserContentController, didReceive m: WKScriptMessage, replyHandler: @escaping (Any?, String?) -> Void) {
        replyHandler(m.body, nil)
    }
    func userContentController(_ c: WKUserContentController, didReceive m: WKScriptMessage) {
        let text = "\(m.body)"
        NSLog("BRIDGEBENCH %@", text)
        let docs = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
        try? text.write(to: docs.appendingPathComponent("result.json"), atomically: true, encoding: .utf8)
    }
    func webView(_ webView: WKWebView, runJavaScriptTextInputPanelWithPrompt prompt: String, defaultText: String?,
                 initiatedByFrame frame: WKFrameInfo, completionHandler: @escaping (String?) -> Void) {
        completionHandler(defaultText)
    }
}

import SwiftUI

struct Web: UIViewRepresentable {
    let bridge: Bridge
    func makeUIView(context: Context) -> WKWebView {
        let cfg = WKWebViewConfiguration()
        cfg.setURLSchemeHandler(Scheme(), forURLScheme: "lx")
        cfg.userContentController.addScriptMessageHandler(bridge, contentWorld: .page, name: "sys")
        cfg.userContentController.add(bridge, name: "done")
        let web = WKWebView(frame: .zero, configuration: cfg)
        web.uiDelegate = bridge
        if #available(iOS 16.4, *) { web.isInspectable = true }
        web.load(URLRequest(url: URL(string: "lx://host/index.html")!))
        return web
    }
    func updateUIView(_ v: WKWebView, context: Context) {}
}

@main
struct BridgeBenchApp: App {
    let bridge = Bridge()
    var body: some Scene { WindowGroup { Web(bridge: bridge).ignoresSafeArea() } }
}
