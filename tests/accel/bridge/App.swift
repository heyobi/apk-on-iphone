// Bridge benchmark for the accelerator (tests/accel): the JIT iOS gives ordinary apps runs
// in WKWebView's web content process, so a guest engine there would reach the app (files,
// network, the terminal) through a bridge. This app measures the bridge's round trip in a
// real WKWebView: synchronous XHR from a worker to a custom URL scheme, prompt() from the
// page, script messages with replies, and SharedArrayBuffer + Atomics.wait if the page is
// cross-origin isolated. It also runs the translated benchmark (bench.wasm) to check that
// the web view's JIT compiles it. Results: Documents/result.json (and the log).
import UIKit
import WebKit

final class Scheme: NSObject, WKURLSchemeHandler {
    func webView(_ webView: WKWebView, start task: WKURLSchemeTask) {
        guard let url = task.request.url else { return }
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

final class ViewController: UIViewController {
    let bridge = Bridge()
    var web: WKWebView!
    override func viewDidLoad() {
        super.viewDidLoad()
        let cfg = WKWebViewConfiguration()
        cfg.setURLSchemeHandler(Scheme(), forURLScheme: "lx")
        cfg.userContentController.addScriptMessageHandler(bridge, contentWorld: .page, name: "sys")
        cfg.userContentController.add(bridge, name: "done")
        web = WKWebView(frame: view.bounds, configuration: cfg)
        web.uiDelegate = bridge
        web.autoresizingMask = [.flexibleWidth, .flexibleHeight]
        if #available(iOS 16.4, *) { web.isInspectable = true }
        view.addSubview(web)
        web.load(URLRequest(url: URL(string: "lx://host/index.html")!))
    }
}

@main
final class AppDelegate: UIResponder, UIApplicationDelegate {
    var window: UIWindow?
    func application(_ application: UIApplication, didFinishLaunchingWithOptions o: [UIApplication.LaunchOptionsKey: Any]?) -> Bool {
        window = UIWindow(frame: UIScreen.main.bounds)
        window?.rootViewController = ViewController()
        window?.makeKeyAndVisible()
        return true
    }
}
