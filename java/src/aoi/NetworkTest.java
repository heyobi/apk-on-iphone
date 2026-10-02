package aoi;

import android.os.IBinder;
import android.os.ServiceManager;
import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.lang.reflect.Method;

/** The network in the app process: aoi.ConnectivityService through the framework's
 *  own IConnectivityManager proxy (a connected Wi-Fi network with INTERNET), then a
 *  name resolved through netd's dnsproxyd and an HTTP GET on the host's sockets
 *  (args: the port of an HTTP server on 127.0.0.1 serving /hello.txt). */
public final class NetworkTest {
    public static void main(String[] args) throws Exception {
        ServiceManager.addService("connectivity", new ConnectivityService());
        IBinder b = ServiceManager.getService("connectivity");
        Object cm = Class.forName("android.net.IConnectivityManager$Stub").getMethod("asInterface", IBinder.class).invoke(null, b);
        Object info = cm.getClass().getMethod("getActiveNetworkInfo").invoke(cm);
        boolean connected = (Boolean) info.getClass().getMethod("isConnected").invoke(info);
        Object net = cm.getClass().getMethod("getActiveNetwork").invoke(cm);
        Object caps = null;
        for (Method m : cm.getClass().getMethods())
            if (m.getName().equals("getNetworkCapabilities")) {
                Object[] a = new Object[m.getParameterTypes().length];
                a[0] = net;
                caps = m.invoke(cm, a);
            }
        boolean internet = caps != null && (Boolean) caps.getClass().getMethod("hasCapability", int.class).invoke(caps, 12);
        String addr = java.net.InetAddress.getByName("localhost").getHostAddress();
        java.net.HttpURLConnection c = (java.net.HttpURLConnection)
                new java.net.URL("http://localhost:" + args[0] + "/hello.txt").openConnection();
        String body = new BufferedReader(new InputStreamReader(c.getInputStream())).readLine();
        System.out.println("network: connected " + connected + ", internet " + internet + ", " + net
                + ", localhost " + addr + ", http " + c.getResponseCode() + " \"" + body + "\"");
    }
}
