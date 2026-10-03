package aoi.webapp;

import android.app.Activity;
import android.os.Bundle;
import android.webkit.ValueCallback;
import android.webkit.WebView;
import android.webkit.WebViewClient;

/** The WebView test app (tools/mktestapk.py OUT.apk web): what an app's help page,
 *  login or ad does. A WebView loads a page from a string (no network), with
 *  JavaScript: the page computes 6*7 into its title and paints itself; when it has
 *  loaded, the activity asks JavaScript for the title and logs it. It asks for a
 *  permission first, as WhatsApp does in onCreate, then opens a web link (the host's
 *  browser has no activity here: iOS opens it). */
public final class WebActivity extends Activity {
    static final String PAGE = "<html><head><title>-</title></head>"
            + "<body style='background:#2a6;color:#fff;font:48px sans-serif'>"
            + "<h1>Merhaba WebView</h1><p id=r></p>"
            + "<script>var x = 6 * 7; document.title = 'web ' + x;"
            + "document.getElementById('r').textContent = 'JavaScript: ' + x;</script></body></html>";

    @Override public void onRequestPermissionsResult(int code, String[] names, int[] results) {
        System.out.println("webapp: permissions " + code + " " + names.length + " " + (results.length > 0 ? results[0] : -9));
        startActivity(new android.content.Intent("android.intent.action.VIEW",   /* a link: the host's browser */
                android.net.Uri.parse("https://example.org/aoi")));
        System.out.println("webapp: link handed on");
    }

    @Override protected void onCreate(Bundle b) {
        super.onCreate(b);
        requestPermissions(new String[] { "android.permission.CAMERA" }, 7);   /* WhatsApp does, in onCreate */
        System.out.println("webapp: creating a WebView");
        final WebView web = new WebView(this);
        web.getSettings().setJavaScriptEnabled(true);
        web.setWebViewClient(new WebViewClient() {
            @Override public void onPageFinished(WebView v, String url) {
                System.out.println("webapp: page finished " + url);
                v.evaluateJavascript("document.title", new ValueCallback<String>() {
                    @Override public void onReceiveValue(String value) {
                        System.out.println("webapp: title " + value);
                    }
                });
            }
        });
        setContentView(web);
        web.loadDataWithBaseURL("https://aoi.test/", PAGE, "text/html", "utf-8", null);
        System.out.println("webapp: loading");
    }
}
