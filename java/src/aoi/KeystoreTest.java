package aoi;

import android.os.ServiceManager;
import android.security.keystore.KeyGenParameterSpec;
import java.security.KeyPair;
import java.security.KeyPairGenerator;
import java.security.KeyStore;
import java.security.PrivateKey;
import java.security.Signature;
import java.security.spec.ECGenParameterSpec;
import java.security.spec.MGF1ParameterSpec;
import javax.crypto.Cipher;
import javax.crypto.KeyAgreement;
import javax.crypto.KeyGenerator;
import javax.crypto.Mac;
import javax.crypto.SecretKey;
import javax.crypto.spec.GCMParameterSpec;
import javax.crypto.spec.OAEPParameterSpec;
import javax.crypto.spec.PSource;

/** AndroidKeyStore through the framework's own provider and aoi.Keystore (keystore2):
 *  an AES-GCM key encrypts and decrypts, an HMAC key signs, an EC key signs (checked
 *  with the public key of its certificate) and agrees with another EC key, an RSA key
 *  decrypts OAEP; then a fresh KeyStore lists the aliases and a key is deleted.
 *  (purposes: ENCRYPT 1, DECRYPT 2, SIGN 4, VERIFY 8, AGREE_KEY 64) */
public final class KeystoreTest {
    public static void main(String[] args) throws Exception {
        ServiceManager.addService(Keystore.NAME, new Keystore("/data/misc/keystore/aoitest"));
        Keystore.installProvider();
        StringBuilder r = new StringBuilder("keystore:");

        KeyGenerator kg = KeyGenerator.getInstance("AES", "AndroidKeyStore");
        kg.init(new KeyGenParameterSpec.Builder("aes", 1 | 2).setBlockModes("GCM").setEncryptionPaddings("NoPadding")
                .setKeySize(256).build());
        SecretKey aes = kg.generateKey();
        Cipher c = Cipher.getInstance("AES/GCM/NoPadding");
        c.init(Cipher.ENCRYPT_MODE, aes);
        byte[] iv = c.getIV();
        byte[] ct = c.doFinal("merhaba keystore".getBytes("UTF-8"));
        c = Cipher.getInstance("AES/GCM/NoPadding");
        c.init(Cipher.DECRYPT_MODE, aes, new GCMParameterSpec(128, iv));
        r.append(" aes ").append(new String(c.doFinal(ct), "UTF-8").equals("merhaba keystore") ? "ok" : "WRONG");

        kg = KeyGenerator.getInstance("HmacSHA256", "AndroidKeyStore");
        kg.init(new KeyGenParameterSpec.Builder("hmac", 4 | 8).build());
        Mac mac = Mac.getInstance("HmacSHA256");
        mac.init(kg.generateKey());
        r.append(", hmac ").append(mac.doFinal(new byte[] { 1, 2, 3 }).length);

        KeyPairGenerator g = KeyPairGenerator.getInstance("EC", "AndroidKeyStore");
        g.initialize(new KeyGenParameterSpec.Builder("ec", 4 | 8 | 64).setDigests("SHA-256")
                .setAlgorithmParameterSpec(new ECGenParameterSpec("secp256r1")).build());
        KeyPair ec = g.generateKeyPair();
        Signature s = Signature.getInstance("SHA256withECDSA");
        s.initSign(ec.getPrivate());
        s.update(new byte[] { 4, 5, 6 });
        byte[] sig = s.sign();
        s = Signature.getInstance("SHA256withECDSA");
        s.initVerify(ec.getPublic());
        s.update(new byte[] { 4, 5, 6 });
        r.append(", ec ").append(s.verify(sig) ? "ok" : "WRONG");
        KeyPairGenerator sw = KeyPairGenerator.getInstance("EC", "AndroidOpenSSL");
        sw.initialize(new ECGenParameterSpec("secp256r1"));
        KeyPair peer = sw.generateKeyPair();
        KeyAgreement ka = KeyAgreement.getInstance("ECDH", "AndroidKeyStore");
        ka.init(ec.getPrivate());
        ka.doPhase(peer.getPublic(), true);
        byte[] a = ka.generateSecret();
        KeyAgreement kb = KeyAgreement.getInstance("ECDH", "AndroidOpenSSL");
        kb.init(peer.getPrivate());
        kb.doPhase(ec.getPublic(), true);
        r.append(", ecdh ").append(java.util.Arrays.equals(a, kb.generateSecret()) ? "ok" : "WRONG");

        g = KeyPairGenerator.getInstance("RSA", "AndroidKeyStore");
        g.initialize(new KeyGenParameterSpec.Builder("rsa", 1 | 2).setKeySize(2048).setDigests("SHA-256")
                .setEncryptionPaddings("OAEPPadding").build());
        KeyPair rsa = g.generateKeyPair();
        OAEPParameterSpec oaep = new OAEPParameterSpec("SHA-256", "MGF1", MGF1ParameterSpec.SHA1, PSource.PSpecified.DEFAULT);
        c = Cipher.getInstance("RSA/ECB/OAEPPadding");
        c.init(Cipher.ENCRYPT_MODE, rsa.getPublic(), oaep);
        ct = c.doFinal("rsa".getBytes("UTF-8"));
        c = Cipher.getInstance("RSA/ECB/OAEPPadding");
        c.init(Cipher.DECRYPT_MODE, rsa.getPrivate(), oaep);
        r.append(", rsa ").append(new String(c.doFinal(ct), "UTF-8").equals("rsa") ? "ok" : "WRONG");

        KeyStore ks = KeyStore.getInstance("AndroidKeyStore");
        ks.load(null);
        java.util.List<String> aliases = java.util.Collections.list(ks.aliases());
        java.util.Collections.sort(aliases);
        r.append(", aliases ").append(aliases);
        r.append(", cert ").append(ks.getCertificate("ec") != null ? "ok" : "none");
        PrivateKey back = (PrivateKey) ks.getKey("ec", null);
        r.append(", reload ").append(back != null ? "ok" : "none");
        ks.deleteEntry("rsa");
        r.append(", deleted ").append(!ks.containsAlias("rsa"));
        System.out.println(r);
    }
}
