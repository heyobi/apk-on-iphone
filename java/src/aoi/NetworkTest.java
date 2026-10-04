package aoi;

import android.os.IBinder;
import android.os.ServiceManager;
import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.lang.reflect.Method;

/** The network in the app process: aoi.ConnectivityService through the framework's
 *  own IConnectivityManager proxy (a connected Wi-Fi network with INTERNET), then a
 *  name resolved through netd's dnsproxyd and an HTTP GET on the host's sockets
 *  (args: the port of an HTTP server on 127.0.0.1 serving /hello.txt). A network
 *  callback (listenForNetwork, as registerNetworkCallback) gets a request back and
 *  onAvailable's message on its Messenger. */
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
        final int[] what = { -1 };
        android.os.Looper.prepare();
        final android.os.Looper looper = android.os.Looper.myLooper();
        final Method quit = looper.getClass().getMethod("quit");
        Class<?> cbi = Class.forName("android.os.Handler$Callback");
        Object cb = java.lang.reflect.Proxy.newProxyInstance(cbi.getClassLoader(), new Class<?>[] { cbi },
                new java.lang.reflect.InvocationHandler() {
                    @Override public Object invoke(Object p, Method m, Object[] a) throws Throwable {
                        if (!m.getName().equals("handleMessage")) return null;
                        what[0] = a[0].getClass().getField("what").getInt(a[0]);
                        quit.invoke(looper);
                        return true;
                    }
                });
        android.os.Handler h = (android.os.Handler) android.os.Handler.class
                .getConstructor(android.os.Looper.class, cbi).newInstance(looper, cb);
        Object messenger = Class.forName("android.os.Messenger").getConstructor(android.os.Handler.class).newInstance(h);
        Object req = null;
        for (Method m : cm.getClass().getMethods())
            if (m.getName().equals("listenForNetwork"))
                req = m.invoke(cm, caps, messenger, new android.os.Binder(), 0, "aoi", null);
        h.postDelayed(new Runnable() {
            @Override public void run() { try { quit.invoke(looper); } catch (Exception e) { /* gone */ } }
        }, 5000);
        android.os.Looper.loop();
        String addr = java.net.InetAddress.getByName("localhost").getHostAddress();
        java.net.HttpURLConnection c = (java.net.HttpURLConnection)
                new java.net.URL("http://localhost:" + args[0] + "/hello.txt").openConnection();
        String body = new BufferedReader(new InputStreamReader(c.getInputStream())).readLine();
        System.out.println("network: connected " + connected + ", internet " + internet + ", " + net
                + ", callback " + (req != null) + " " + what[0] + ", localhost " + addr + ", http " + c.getResponseCode() + " \"" + body + "\"");
    }
}
