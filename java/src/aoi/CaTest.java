package aoi;

import java.security.KeyStore;
import java.security.cert.X509Certificate;
import javax.net.ssl.TrustManagerFactory;
import javax.net.ssl.X509TrustManager;

/** The system's trust store as an app's TLS sees it (conscrypt's TrustManagerImpl on
 *  /apex/com.android.conscrypt/cacerts): the roots tools/cacerts-extra.pem adds are
 *  there, and a chain that ends in one of them (Sectigo R46) is trusted. */
public final class CaTest {
    public static void main(String[] args) throws Exception {
        TrustManagerFactory f = TrustManagerFactory.getInstance(TrustManagerFactory.getDefaultAlgorithm());
        f.init((KeyStore) null);
        X509TrustManager tm = (X509TrustManager) f.getTrustManagers()[0];
        X509Certificate r46 = null, ssl = null;
        for (X509Certificate c : tm.getAcceptedIssuers()) {
            String s = c.getSubjectX500Principal().getName();
            if (s.contains("Sectigo Public Server Authentication Root R46")) r46 = c;
            if (s.contains("SSL.com TLS RSA Root CA 2022")) ssl = c;
        }
        String trusted;
        try {
            tm.checkServerTrusted(new X509Certificate[] { r46 }, "RSA");
            trusted = "true";
        } catch (Exception e) {
            trusted = String.valueOf(e);
        }
        System.out.println("ca: Sectigo R46 " + (r46 != null) + ", SSL.com 2022 " + (ssl != null) + ", chain trusted " + trusted);
    }
}
