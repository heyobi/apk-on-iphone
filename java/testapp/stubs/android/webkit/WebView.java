package android.webkit;

public class WebView extends android.view.View {
    public WebView(android.content.Context c) { super(c); }
    public WebSettings getSettings() { return null; }
    public void setWebViewClient(WebViewClient c) {}
    public void loadDataWithBaseURL(String base, String data, String mime, String enc, String history) {}
    public void evaluateJavascript(String script, ValueCallback<String> cb) {}
}
