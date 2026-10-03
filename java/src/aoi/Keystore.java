package aoi;

import android.hardware.security.keymint.KeyParameter;
import android.hardware.security.keymint.KeyParameterValue;
import android.os.ServiceSpecificException;
import android.system.keystore2.Authorization;
import android.system.keystore2.AuthenticatorSpec;
import android.system.keystore2.CreateOperationResponse;
import android.system.keystore2.EphemeralStorageKeyResponse;
import android.system.keystore2.IKeystoreOperation;
import android.system.keystore2.IKeystoreSecurityLevel;
import android.system.keystore2.IKeystoreService;
import android.system.keystore2.KeyDescriptor;
import android.system.keystore2.KeyEntryResponse;
import android.system.keystore2.KeyMetadata;
import android.system.keystore2.KeyParameters;
import java.io.ByteArrayOutputStream;
import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.math.BigInteger;
import java.security.Key;
import java.security.KeyFactory;
import java.security.KeyPair;
import java.security.KeyPairGenerator;
import java.security.PrivateKey;
import java.security.PublicKey;
import java.security.SecureRandom;
import java.security.Signature;
import java.security.interfaces.RSAPrivateCrtKey;
import java.security.spec.ECGenParameterSpec;
import java.security.spec.MGF1ParameterSpec;
import java.security.spec.PKCS8EncodedKeySpec;
import java.security.spec.RSAKeyGenParameterSpec;
import java.security.spec.RSAPublicKeySpec;
import java.security.spec.X509EncodedKeySpec;
import java.util.ArrayList;
import java.util.Calendar;
import java.util.TimeZone;
import java.util.TreeMap;
import javax.crypto.Cipher;
import javax.crypto.KeyAgreement;
import javax.crypto.Mac;
import javax.crypto.spec.GCMParameterSpec;
import javax.crypto.spec.IvParameterSpec;
import javax.crypto.spec.OAEPParameterSpec;
import javax.crypto.spec.PSource;
import javax.crypto.spec.SecretKeySpec;

/** keystore2 ("android.system.keystore2.IKeystoreService/default"), the service behind
 *  the AndroidKeyStore JCA provider (aoi.Main installs the provider: the zygote would).
 *  A phone keeps the keys in its TEE (KeyMint); here they are software keys in the
 *  app's data (/data/misc/keystore/aoi, one file per alias), used with Conscrypt:
 *  AES (GCM, CBC, CTR, ECB), 3DES, HMAC, EC and RSA signing, RSA decryption, ECDH.
 *  Asymmetric keys get a self-signed X.509 certificate (KeyMint makes one when there is
 *  no attestation key): the framework takes the public key from it, and does public
 *  key operations (verify, encrypt) itself. The key's characteristics are its
 *  generation parameters, reported at TRUSTED_ENVIRONMENT level, as on a phone. */
final class Keystore extends IKeystoreService.Stub {
    static final String NAME = "android.system.keystore2.IKeystoreService/default";

    /* keymint Tag, Algorithm, ... (android.hardware.security.keymint) */
    static final int PURPOSE = 0x20000001, ALGORITHM = 0x10000002, KEY_SIZE = 0x30000003, BLOCK_MODE = 0x20000004,
            DIGEST = 0x20000005, PADDING = 0x20000006, CALLER_NONCE = 0x70000007, EC_CURVE = 0x1000000a,
            RSA_PUBLIC_EXPONENT = 0x500000c8, RSA_OAEP_MGF_DIGEST = 0x200000cb, USER_AUTH_TYPE = 0x100001f8,
            HARDWARE_TYPE = 0x10000130, ORIGIN = 0x100002be, CREATION_DATETIME = 0x600002bd,
            NONCE = 0x90000000 | 1001, MAC_LENGTH = 0x300003eb, CERTIFICATE_SERIAL = 0x80000000 | 1006,
            CERTIFICATE_SUBJECT = 0x90000000 | 1007, CERTIFICATE_NOT_BEFORE = 0x600003f0,
            CERTIFICATE_NOT_AFTER = 0x600003f1;
    static final int RSA = 1, EC = 3, AES = 32, TRIPLE_DES = 33, HMAC = 128;
    static final int ENCRYPT = 0, DECRYPT = 1, SIGN = 2, VERIFY = 3, AGREE_KEY = 6;
    static final int ECB = 1, CBC = 2, CTR = 3, GCM = 32;
    static final int PAD_NONE = 1, OAEP = 2, PSS = 3, PKCS1_ENCRYPT = 4, PKCS1_SIGN = 5, PKCS7 = 64;
    static final int TEE = 1, STRONGBOX = 2, KEYSTORE = 100;
    /* keystore2 ResponseCode and keymint ErrorCode, as ServiceSpecificException codes */
    static final int KEY_NOT_FOUND = 7, SYSTEM_ERROR = 4, INVALID_ARGUMENT = 20;
    static final int UNSUPPORTED_PURPOSE = -2, INCOMPATIBLE_PURPOSE = -3, UNSUPPORTED_ALGORITHM = -4,
            VERIFICATION_FAILED = -30, HARDWARE_TYPE_UNAVAILABLE = -68, UNIMPLEMENTED = -100;

    private final File dir;
    private final SecurityLevel tee = new SecurityLevel();
    private final SecureRandom random = new SecureRandom();
    private final TreeMap<String, Entry> byAlias = new TreeMap<String, Entry>();
    private long nextId = 1;

    /** A key (or a certificate only: algorithm 0) under an alias. */
    static final class Entry {
        long id;
        String alias;
        int algorithm;
        byte[] material;                 /* raw secret key, or PKCS#8 private key */
        byte[] cert, chain;
        KeyParameter[] params;
        long modified;
    }

    Keystore(String dataDir) {
        dir = new File(dataDir);
        dir.mkdirs();
        File[] fs = dir.listFiles();
        if (fs != null) for (File f : fs) {
            try { Entry e = read(f); byAlias.put(e.alias, e); if (e.id >= nextId) nextId = e.id + 1; }
            catch (Exception x) { System.out.println("aoi: keystore: " + f + ": " + x); }
        }
    }

    /** The AndroidKeyStore providers into the JCA, as ZygoteInit.warmUpJcaProviders does. */
    static void installProvider() {
        try {
            Class.forName("android.security.keystore2.AndroidKeyStoreProvider").getMethod("install").invoke(null);
        } catch (Throwable e) {
            System.out.println("aoi: keystore: no AndroidKeyStore provider: " + e);
        }
    }

    static ServiceSpecificException error(int code, String what) {
        return new ServiceSpecificException(code, what);
    }

    /* ---------- key parameters ---------- */

    static int type(int tag) { return tag >>> 28; }      /* 1 ENUM, 2 ENUM_REP, 3 UINT, 4 UINT_REP, 5 ULONG, 6 DATE, 7 BOOL, 8 BIGNUM, 9 BYTES, 10 ULONG_REP */

    static KeyParameter param(int tag, long v, byte[] b) {
        KeyParameter p = new KeyParameter();
        p.tag = tag;
        int i = (int) v;
        switch (tag) {
        case ALGORITHM: p.value = KeyParameterValue.algorithm(i); break;
        case BLOCK_MODE: p.value = KeyParameterValue.blockMode(i); break;
        case PADDING: p.value = KeyParameterValue.paddingMode(i); break;
        case DIGEST: case RSA_OAEP_MGF_DIGEST: p.value = KeyParameterValue.digest(i); break;
        case EC_CURVE: p.value = KeyParameterValue.ecCurve(i); break;
        case ORIGIN: p.value = KeyParameterValue.origin(i); break;
        case PURPOSE: p.value = KeyParameterValue.keyPurpose(i); break;
        case USER_AUTH_TYPE: p.value = KeyParameterValue.hardwareAuthenticatorType(i); break;
        case HARDWARE_TYPE: p.value = KeyParameterValue.securityLevel(i); break;
        default:
            switch (type(tag)) {
            case 1: case 2: case 3: case 4: p.value = KeyParameterValue.integer(i); break;
            case 5: case 10: p.value = KeyParameterValue.longInteger(v); break;
            case 6: p.value = KeyParameterValue.dateTime(v); break;
            case 7: p.value = KeyParameterValue.boolValue(true); break;
            default: p.value = KeyParameterValue.blob(b != null ? b : new byte[0]); break;
            }
        }
        return p;
    }

    static long num(KeyParameterValue v) {
        switch (v.getTag()) {
        case KeyParameterValue.algorithm: return v.getAlgorithm();
        case KeyParameterValue.blockMode: return v.getBlockMode();
        case KeyParameterValue.paddingMode: return v.getPaddingMode();
        case KeyParameterValue.digest: return v.getDigest();
        case KeyParameterValue.ecCurve: return v.getEcCurve();
        case KeyParameterValue.origin: return v.getOrigin();
        case KeyParameterValue.keyPurpose: return v.getKeyPurpose();
        case KeyParameterValue.hardwareAuthenticatorType: return v.getHardwareAuthenticatorType();
        case KeyParameterValue.securityLevel: return v.getSecurityLevel();
        case KeyParameterValue.boolValue: return v.getBoolValue() ? 1 : 0;
        case KeyParameterValue.integer: return v.getInteger();
        case KeyParameterValue.longInteger: return v.getLongInteger();
        case KeyParameterValue.dateTime: return v.getDateTime();
        default: return 0;
        }
    }

    static byte[] bytes(KeyParameterValue v) {
        return v.getTag() == KeyParameterValue.blob ? v.getBlob() : null;
    }

    static boolean has(KeyParameter[] ps, int tag) {
        if (ps != null) for (KeyParameter p : ps) if (p.tag == tag) return true;
        return false;
    }

    static long get(KeyParameter[] ps, int tag, long dflt) {
        if (ps != null) for (KeyParameter p : ps) if (p.tag == tag) return num(p.value);
        return dflt;
    }

    static byte[] blob(KeyParameter[] ps, int tag) {
        if (ps != null) for (KeyParameter p : ps) if (p.tag == tag) return bytes(p.value);
        return null;
    }

    static boolean hasValue(KeyParameter[] ps, int tag, long v) {
        if (ps != null) for (KeyParameter p : ps) if (p.tag == tag && num(p.value) == v) return true;
        return false;
    }

    /* ---------- storage ---------- */

    private File file(String alias) {
        StringBuilder b = new StringBuilder();
        for (byte c : alias.getBytes(java.nio.charset.StandardCharsets.UTF_8)) b.append(String.format("%02x", c & 0xff));
        return new File(dir, b.toString());
    }

    private void write(Entry e) {
        try {
            ByteArrayOutputStream bo = new ByteArrayOutputStream();
            DataOutputStream o = new DataOutputStream(bo);
            o.writeInt(1);
            o.writeLong(e.id); o.writeUTF(e.alias); o.writeInt(e.algorithm); o.writeLong(e.modified);
            writeBytes(o, e.material); writeBytes(o, e.cert); writeBytes(o, e.chain);
            o.writeInt(e.params == null ? 0 : e.params.length);
            if (e.params != null) for (KeyParameter p : e.params) {
                o.writeInt(p.tag); o.writeLong(num(p.value)); writeBytes(o, bytes(p.value));
            }
            o.flush();
            File t = new File(dir, ".tmp");
            FileOutputStream f = new FileOutputStream(t);
            f.write(bo.toByteArray());
            f.getFD().sync();
            f.close();
            if (!t.renameTo(file(e.alias))) throw new java.io.IOException("rename");
        } catch (Exception x) {
            throw error(SYSTEM_ERROR, "keystore: cannot store " + e.alias + ": " + x);
        }
    }

    private static void writeBytes(DataOutputStream o, byte[] b) throws java.io.IOException {
        o.writeInt(b == null ? -1 : b.length);
        if (b != null) o.write(b);
    }

    private static byte[] readBytes(DataInputStream in) throws java.io.IOException {
        int n = in.readInt();
        if (n < 0) return null;
        byte[] b = new byte[n];
        in.readFully(b);
        return b;
    }

    private static Entry read(File f) throws Exception {
        DataInputStream in = new DataInputStream(new FileInputStream(f));
        try {
            if (in.readInt() != 1) throw new java.io.IOException("unknown format");
            Entry e = new Entry();
            e.id = in.readLong(); e.alias = in.readUTF(); e.algorithm = in.readInt(); e.modified = in.readLong();
            e.material = readBytes(in); e.cert = readBytes(in); e.chain = readBytes(in);
            int n = in.readInt();
            e.params = new KeyParameter[n];
            for (int i = 0; i < n; i++) {
                int tag = in.readInt();
                long v = in.readLong();
                e.params[i] = param(tag, v, readBytes(in));
            }
            return e;
        } finally {
            in.close();
        }
    }

    /** The entry a descriptor names: Domain APP (0) by alias, KEY_ID (4) by id. */
    private Entry find(KeyDescriptor d) {
        if (d == null) throw error(INVALID_ARGUMENT, "keystore: no key");
        if (d.domain == 4) {
            for (Entry e : byAlias.values()) if (e.id == d.nspace) return e;
        } else if (d.alias != null) {
            Entry e = byAlias.get(d.alias);
            if (e != null) return e;
        }
        throw error(KEY_NOT_FOUND, "keystore: no key " + (d.alias != null ? d.alias : "#" + d.nspace));
    }

    private KeyMetadata metadata(Entry e) {
        KeyMetadata m = new KeyMetadata();
        m.key = new KeyDescriptor();
        m.key.domain = 4;                                          /* KEY_ID */
        m.key.nspace = e.id;
        m.keySecurityLevel = TEE;
        ArrayList<Authorization> a = new ArrayList<Authorization>();
        if (e.params != null) for (KeyParameter p : e.params) {
            Authorization z = new Authorization();
            z.keyParameter = p;
            z.securityLevel = p.tag == CREATION_DATETIME ? KEYSTORE : TEE;
            a.add(z);
        }
        m.authorizations = a.toArray(new Authorization[0]);
        m.certificate = e.cert;
        m.certificateChain = e.chain;
        m.modificationTimeMs = e.modified;
        return m;
    }

    /* ---------- IKeystoreService ---------- */

    @Override public synchronized IKeystoreSecurityLevel getSecurityLevel(int level) {
        if (level == STRONGBOX) throw error(HARDWARE_TYPE_UNAVAILABLE, "keystore: no StrongBox");
        return tee;
    }

    @Override public synchronized KeyEntryResponse getKeyEntry(KeyDescriptor key) {
        Entry e = find(key);
        KeyEntryResponse r = new KeyEntryResponse();
        r.iSecurityLevel = e.algorithm != 0 ? tee : null;
        r.metadata = metadata(e);
        return r;
    }

    @Override public synchronized void updateSubcomponent(KeyDescriptor key, byte[] cert, byte[] chain) {
        Entry e = key != null && key.domain == 0 && key.alias != null ? byAlias.get(key.alias) : find(key);
        if (e == null) {                                           /* a certificate entry (setCertificateEntry: the chain) */
            if (cert != null || chain == null) throw error(KEY_NOT_FOUND, "keystore: no key " + key.alias);
            e = new Entry();
            e.id = nextId++; e.alias = key.alias; e.params = new KeyParameter[0];
        }
        e.cert = cert; e.chain = chain;
        e.modified = System.currentTimeMillis();
        byAlias.put(e.alias, e);
        write(e);
    }

    @Override public synchronized KeyDescriptor[] listEntries(int domain, long nspace) {
        return listEntriesBatched(domain, nspace, null);
    }

    @Override public synchronized KeyDescriptor[] listEntriesBatched(int domain, long nspace, String past) {
        ArrayList<KeyDescriptor> l = new ArrayList<KeyDescriptor>();
        for (Entry e : (past == null ? byAlias : byAlias.tailMap(past, false)).values()) {
            KeyDescriptor d = new KeyDescriptor();
            d.domain = 0; d.nspace = nspace; d.alias = e.alias;
            l.add(d);
        }
        return l.toArray(new KeyDescriptor[0]);
    }

    @Override public synchronized int getNumberOfEntries(int domain, long nspace) { return byAlias.size(); }

    @Override public synchronized void deleteKey(KeyDescriptor key) {
        Entry e = find(key);
        byAlias.remove(e.alias);
        file(e.alias).delete();
    }

    @Override public KeyDescriptor grant(KeyDescriptor key, int uid, int access) {
        throw error(SYSTEM_ERROR, "keystore: grants are not supported");
    }

    @Override public void ungrant(KeyDescriptor key, int uid) {
        throw error(SYSTEM_ERROR, "keystore: grants are not supported");
    }

    @Override public int getInterfaceVersion() { return 3; }
    @Override public String getInterfaceHash() { return "4f1c704008e5687ed0d6f1590464aed39fc7f64e"; }

    /* ---------- keys ---------- */

    private static String curve(KeyParameter[] ps) {
        long c = get(ps, EC_CURVE, -1);
        long size = get(ps, KEY_SIZE, 256);
        if (c == 0 || (c < 0 && size == 224)) return "secp224r1";
        if (c == 2 || (c < 0 && size == 384)) return "secp384r1";
        if (c == 3 || (c < 0 && size == 521)) return "secp521r1";
        return "secp256r1";
    }

    static Object jca(String kind, String alg) throws Exception {
        String[] providers = { "AndroidOpenSSL", "BC", null };
        Exception last = null;
        for (String p : providers) {
            try {
                if (kind.equals("Cipher")) return p == null ? Cipher.getInstance(alg) : Cipher.getInstance(alg, p);
                if (kind.equals("Mac")) return p == null ? Mac.getInstance(alg) : Mac.getInstance(alg, p);
                if (kind.equals("Signature")) return p == null ? Signature.getInstance(alg) : Signature.getInstance(alg, p);
                if (kind.equals("KeyAgreement")) return p == null ? KeyAgreement.getInstance(alg) : KeyAgreement.getInstance(alg, p);
                if (kind.equals("KeyPairGenerator")) return p == null ? KeyPairGenerator.getInstance(alg) : KeyPairGenerator.getInstance(alg, p);
                if (kind.equals("KeyFactory")) return p == null ? KeyFactory.getInstance(alg) : KeyFactory.getInstance(alg, p);
            } catch (Exception e) {
                last = e;
            }
        }
        throw last;
    }

    static PrivateKey privateKey(Entry e) throws Exception {
        KeyFactory f = (KeyFactory) jca("KeyFactory", e.algorithm == EC ? "EC" : "RSA");
        return f.generatePrivate(new PKCS8EncodedKeySpec(e.material));
    }

    /** The parameters a key keeps: the request's, without what only concerns its creation. */
    private static KeyParameter[] characteristics(KeyParameter[] ps, int origin) {
        ArrayList<KeyParameter> l = new ArrayList<KeyParameter>();
        if (ps != null) for (KeyParameter p : ps) {
            int t = p.tag;
            if (t == CERTIFICATE_SERIAL || t == CERTIFICATE_SUBJECT || t == CERTIFICATE_NOT_BEFORE
                    || t == CERTIFICATE_NOT_AFTER || t == CREATION_DATETIME || t == ORIGIN
                    || (t & 0x0fffffff) >= 708 && (t & 0x0fffffff) <= 723) continue;   /* attestation challenge, ids */
            l.add(p);
        }
        l.add(param(ORIGIN, origin, null));
        l.add(param(CREATION_DATETIME, System.currentTimeMillis(), null));
        return l.toArray(new KeyParameter[0]);
    }

    private KeyMetadata store(KeyDescriptor d, Entry e) {
        if (d == null || d.alias == null) throw error(INVALID_ARGUMENT, "keystore: a key needs an alias");
        Entry old = byAlias.get(d.alias);
        e.id = old != null ? old.id : nextId++;
        e.alias = d.alias;
        e.modified = System.currentTimeMillis();
        byAlias.put(e.alias, e);
        write(e);
        return metadata(e);
    }

    final class SecurityLevel extends IKeystoreSecurityLevel.Stub {
        @Override public KeyMetadata generateKey(KeyDescriptor d, KeyDescriptor attest, KeyParameter[] ps, int flags, byte[] entropy) {
            synchronized (Keystore.this) {
                Entry e = new Entry();
                e.algorithm = (int) get(ps, ALGORITHM, -1);
                try {
                    switch (e.algorithm) {
                    case AES: case HMAC: {
                        e.material = new byte[(int) get(ps, KEY_SIZE, e.algorithm == AES ? 256 : 256) / 8];
                        random.nextBytes(e.material);
                        break;
                    }
                    case TRIPLE_DES:
                        e.material = new byte[24];
                        random.nextBytes(e.material);
                        break;
                    case EC: case RSA: {
                        KeyPairGenerator g = (KeyPairGenerator) jca("KeyPairGenerator", e.algorithm == EC ? "EC" : "RSA");
                        if (e.algorithm == EC) g.initialize(new ECGenParameterSpec(curve(ps)), random);
                        else g.initialize(new RSAKeyGenParameterSpec((int) get(ps, KEY_SIZE, 2048),
                                BigInteger.valueOf(get(ps, RSA_PUBLIC_EXPONENT, 65537))), random);
                        KeyPair kp = g.generateKeyPair();
                        e.material = kp.getPrivate().getEncoded();
                        e.cert = Cert.selfSigned(kp.getPublic(), kp.getPrivate(), e.algorithm, ps);
                        break;
                    }
                    default:
                        throw error(UNSUPPORTED_ALGORITHM, "keystore: algorithm " + e.algorithm);
                    }
                } catch (ServiceSpecificException x) {
                    throw x;
                } catch (Exception x) {
                    throw error(SYSTEM_ERROR, "keystore: generateKey: " + x);
                }
                e.params = characteristics(ps, 0);                 /* GENERATED */
                return store(d, e);
            }
        }

        @Override public KeyMetadata importKey(KeyDescriptor d, KeyDescriptor attest, KeyParameter[] ps, int flags, byte[] data) {
            synchronized (Keystore.this) {
                Entry e = new Entry();
                e.algorithm = (int) get(ps, ALGORITHM, -1);
                e.material = data;
                if (e.algorithm == RSA) {                          /* its public key is in it: a certificate */
                    try {
                        RSAPrivateCrtKey k = (RSAPrivateCrtKey) privateKey(e);
                        PublicKey pub = ((KeyFactory) jca("KeyFactory", "RSA")).generatePublic(
                                new RSAPublicKeySpec(k.getModulus(), k.getPublicExponent()));
                        e.cert = Cert.selfSigned(pub, k, RSA, ps);
                    } catch (Exception x) {
                        /* the caller sets the certificate (updateSubcomponent) */
                    }
                }
                e.params = characteristics(ps, 2);                 /* IMPORTED */
                return store(d, e);
            }
        }

        @Override public KeyMetadata importWrappedKey(KeyDescriptor d, KeyDescriptor w, byte[] mask, KeyParameter[] ps, AuthenticatorSpec[] a) {
            throw error(UNIMPLEMENTED, "keystore: wrapped keys are not supported");
        }

        @Override public EphemeralStorageKeyResponse convertStorageKeyToEphemeral(KeyDescriptor k) {
            throw error(UNIMPLEMENTED, "keystore: storage keys are not supported");
        }

        @Override public void deleteKey(KeyDescriptor key) { Keystore.this.deleteKey(key); }

        @Override public CreateOperationResponse createOperation(KeyDescriptor d, KeyParameter[] ps, boolean forced) {
            Entry e;
            synchronized (Keystore.this) { e = find(d); }
            CreateOperationResponse r = new CreateOperationResponse();
            Operation op = new Operation(e, ps);
            r.iOperation = op;
            if (op.nonce != null) {
                r.parameters = new KeyParameters();
                r.parameters.keyParameter = new KeyParameter[] { param(NONCE, 0, op.nonce) };
            }
            return r;
        }

        @Override public int getInterfaceVersion() { return 3; }
        @Override public String getInterfaceHash() { return "4f1c704008e5687ed0d6f1590464aed39fc7f64e"; }
    }

    static String digestName(long d) {
        switch ((int) d) {
        case 1: return "MD5";
        case 2: return "SHA1";
        case 3: return "SHA224";
        case 5: return "SHA384";
        case 6: return "SHA512";
        case 0: return "NONE";
        default: return "SHA256";
        }
    }

    static String mgfDigest(long d) {
        switch ((int) d) {
        case 1: return "MD5";
        case 3: return "SHA-224";
        case 4: return "SHA-256";
        case 5: return "SHA-384";
        case 6: return "SHA-512";
        default: return "SHA-1";
        }
    }

    /** One begun operation: a Cipher, Mac, Signature or key agreement on the key. */
    final class Operation extends IKeystoreOperation.Stub {
        final int purpose;
        byte[] nonce;
        private Cipher cipher;
        private Mac mac;
        private int macBytes;
        private Signature sig;
        private ByteArrayOutputStream buffered;                    /* raw RSA, key agreement */
        private PrivateKey priv;
        private boolean done;

        Operation(Entry e, KeyParameter[] ps) {
            purpose = (int) get(ps, PURPOSE, -1);
            try {
                switch (e.algorithm) {
                case AES: case TRIPLE_DES: begin(e, ps); break;
                case HMAC: {
                    if (purpose != SIGN && purpose != VERIFY) throw error(INCOMPATIBLE_PURPOSE, "keystore: HMAC purpose " + purpose);
                    long dg = get(ps, DIGEST, get(e.params, DIGEST, 4));
                    mac = (Mac) jca("Mac", "Hmac" + digestName(dg));
                    mac.init(new SecretKeySpec(e.material, "Hmac" + digestName(dg)));
                    macBytes = (int) get(ps, MAC_LENGTH, mac.getMacLength() * 8) / 8;
                    break;
                }
                case EC: case RSA: beginAsymmetric(e, ps); break;
                default: throw error(INCOMPATIBLE_PURPOSE, "keystore: no key material for this operation");
                }
            } catch (ServiceSpecificException x) {
                throw x;
            } catch (Exception x) {
                throw error(SYSTEM_ERROR, "keystore: createOperation: " + x);
            }
        }

        private void begin(Entry e, KeyParameter[] ps) throws Exception {
            if (purpose != ENCRYPT && purpose != DECRYPT) throw error(INCOMPATIBLE_PURPOSE, "keystore: cipher purpose " + purpose);
            boolean aes = e.algorithm == AES;
            int mode = (int) get(ps, BLOCK_MODE, aes ? GCM : ECB);
            int pad = (int) get(ps, PADDING, PAD_NONE);
            String m = mode == GCM ? "GCM" : mode == CBC ? "CBC" : mode == CTR ? "CTR" : "ECB";
            String alg = (aes ? "AES/" : "DESede/") + m + "/" + (pad == PKCS7 ? "PKCS5Padding" : "NoPadding");
            cipher = (Cipher) jca("Cipher", alg);
            SecretKeySpec k = new SecretKeySpec(e.material, aes ? "AES" : "DESede");
            int jmode = purpose == ENCRYPT ? Cipher.ENCRYPT_MODE : Cipher.DECRYPT_MODE;
            if (mode == ECB) { cipher.init(jmode, k); return; }
            nonce = blob(ps, NONCE);
            boolean ours = nonce == null;
            if (ours) {
                if (purpose == DECRYPT) throw error(INVALID_ARGUMENT, "keystore: decryption needs the nonce");
                nonce = new byte[mode == GCM ? 12 : aes ? 16 : 8];
                random.nextBytes(nonce);
            }
            if (mode == GCM) cipher.init(jmode, k, new GCMParameterSpec((int) get(ps, MAC_LENGTH, 128), nonce));
            else cipher.init(jmode, k, new IvParameterSpec(nonce));
            if (!ours) nonce = null;                               /* only a nonce made here goes back */
        }

        private void beginAsymmetric(Entry e, KeyParameter[] ps) throws Exception {
            priv = privateKey(e);
            long dg = get(ps, DIGEST, 0);
            int pad = (int) get(ps, PADDING, e.algorithm == RSA ? PAD_NONE : 0);
            if (purpose == SIGN) {
                if (e.algorithm == EC) {
                    sig = (Signature) jca("Signature", (dg == 0 ? "NONE" : digestName(dg)) + "withECDSA");
                } else if (pad == PAD_NONE && dg == 0) {
                    buffered = new ByteArrayOutputStream();        /* raw RSA */
                    return;
                } else if (pad == PSS) {
                    sig = (Signature) jca("Signature", digestName(dg) + "withRSA/PSS");
                } else {
                    sig = (Signature) jca("Signature", (dg == 0 ? "NONE" : digestName(dg)) + "withRSA");
                }
                sig.initSign(priv);
            } else if (purpose == DECRYPT && e.algorithm == RSA) {
                if (pad == OAEP) {
                    cipher = (Cipher) jca("Cipher", "RSA/ECB/OAEPPadding");
                    String md = mgfDigest(dg == 0 ? 2 : dg);
                    cipher.init(Cipher.DECRYPT_MODE, priv, new OAEPParameterSpec(md, "MGF1",
                            new MGF1ParameterSpec(mgfDigest(get(ps, RSA_OAEP_MGF_DIGEST, 2))), PSource.PSpecified.DEFAULT));
                } else {
                    cipher = (Cipher) jca("Cipher", pad == PKCS1_ENCRYPT ? "RSA/ECB/PKCS1Padding" : "RSA/ECB/NoPadding");
                    cipher.init(Cipher.DECRYPT_MODE, priv);
                }
            } else if (purpose == AGREE_KEY && e.algorithm == EC) {
                buffered = new ByteArrayOutputStream();
            } else {
                throw error(UNSUPPORTED_PURPOSE, "keystore: purpose " + purpose + " on algorithm " + e.algorithm);
            }
        }

        private void live() {
            if (done) throw error(-28, "keystore: operation finished");   /* INVALID_OPERATION_HANDLE */
        }

        private byte[] out(byte[] b) { return b == null || b.length == 0 ? null : b; }

        @Override public synchronized void updateAad(byte[] aad) {
            live();
            if (cipher == null) throw error(INVALID_ARGUMENT, "keystore: no AAD here");
            cipher.updateAAD(aad);
        }

        @Override public synchronized byte[] update(byte[] in) {
            live();
            try {
                if (in == null) return null;
                if (buffered != null) { buffered.write(in); return null; }
                if (mac != null) { mac.update(in); return null; }
                if (sig != null) { sig.update(in); return null; }
                return out(cipher.update(in));
            } catch (Exception x) {
                done = true;
                throw error(SYSTEM_ERROR, "keystore: update: " + x);
            }
        }

        @Override public synchronized byte[] finish(byte[] in, byte[] signature) {
            live();
            done = true;
            try {
                if (in == null) in = new byte[0];
                if (mac != null) {
                    mac.update(in);
                    byte[] m = java.util.Arrays.copyOf(mac.doFinal(), macBytes);
                    if (purpose == SIGN) return m;
                    if (signature == null || !java.security.MessageDigest.isEqual(m, signature))
                        throw error(VERIFICATION_FAILED, "keystore: HMAC verification failed");
                    return null;
                }
                if (sig != null) { sig.update(in); return sig.sign(); }
                if (buffered != null) {
                    buffered.write(in);
                    byte[] all = buffered.toByteArray();
                    if (purpose == AGREE_KEY) {
                        KeyAgreement ka = (KeyAgreement) jca("KeyAgreement", "ECDH");
                        ka.init(priv);
                        ka.doPhase(((KeyFactory) jca("KeyFactory", "EC")).generatePublic(new X509EncodedKeySpec(all)), true);
                        return ka.generateSecret();
                    }
                    Cipher raw = (Cipher) jca("Cipher", "RSA/ECB/NoPadding");   /* raw RSA signature */
                    raw.init(Cipher.DECRYPT_MODE, priv);
                    int n = (((RSAPrivateCrtKey) priv).getModulus().bitLength() + 7) / 8;
                    byte[] block = new byte[n];
                    System.arraycopy(all, 0, block, n - Math.min(n, all.length), Math.min(n, all.length));
                    return raw.doFinal(block);
                }
                return out(cipher.doFinal(in));
            } catch (ServiceSpecificException x) {
                throw x;
            } catch (javax.crypto.AEADBadTagException x) {
                throw error(VERIFICATION_FAILED, "keystore: " + x);
            } catch (Exception x) {
                throw error(-1000, "keystore: finish: " + x);      /* UNKNOWN_ERROR */
            }
        }

        @Override public synchronized void abort() { done = true; }

        @Override public int getInterfaceVersion() { return 3; }
        @Override public String getInterfaceHash() { return "4f1c704008e5687ed0d6f1590464aed39fc7f64e"; }
    }

    /** A self-signed X.509 v3 certificate in DER: what KeyMint gives a new key pair. */
    static final class Cert {
        static byte[] tlv(int tag, byte[]... parts) {
            ByteArrayOutputStream c = new ByteArrayOutputStream();
            for (byte[] p : parts) c.write(p, 0, p.length);
            int n = c.size();
            ByteArrayOutputStream o = new ByteArrayOutputStream();
            o.write(tag);
            if (n < 128) o.write(n);
            else if (n < 256) { o.write(0x81); o.write(n); }
            else if (n < 65536) { o.write(0x82); o.write(n >> 8); o.write(n); }
            else { o.write(0x83); o.write(n >> 16); o.write(n >> 8); o.write(n); }
            byte[] b = c.toByteArray();
            o.write(b, 0, b.length);
            return o.toByteArray();
        }

        static byte[] oid(int... arcs) {
            ByteArrayOutputStream o = new ByteArrayOutputStream();
            o.write(arcs[0] * 40 + arcs[1]);
            for (int i = 2; i < arcs.length; i++) {
                int v = arcs[i], k = 28;
                while (k > 0 && (v >>> k) == 0) k -= 7;
                for (; k > 0; k -= 7) o.write(0x80 | ((v >>> k) & 0x7f));
                o.write(v & 0x7f);
            }
            return tlv(0x06, o.toByteArray());
        }

        static byte[] time(long ms) {
            Calendar c = Calendar.getInstance(TimeZone.getTimeZone("UTC"));
            c.setTimeInMillis(ms);
            int y = c.get(Calendar.YEAR);
            String s = String.format(java.util.Locale.US, "%02d%02d%02d%02d%02dZ", c.get(Calendar.MONTH) + 1,
                    c.get(Calendar.DAY_OF_MONTH), c.get(Calendar.HOUR_OF_DAY), c.get(Calendar.MINUTE), c.get(Calendar.SECOND));
            if (y >= 1950 && y < 2050) return tlv(0x17, (String.format("%02d", y % 100) + s).getBytes());
            return tlv(0x18, (String.format("%04d", y) + s).getBytes());
        }

        static byte[] selfSigned(PublicKey pub, PrivateKey priv, int alg, KeyParameter[] ps) throws Exception {
            byte[] subject = blob(ps, CERTIFICATE_SUBJECT);
            if (subject == null || subject.length == 0)
                subject = new javax.security.auth.x500.X500Principal("CN=Android Keystore Key").getEncoded();
            byte[] serial = blob(ps, CERTIFICATE_SERIAL);
            BigInteger sn = serial != null && serial.length > 0 ? new BigInteger(1, serial) : BigInteger.ONE;
            long from = get(ps, CERTIFICATE_NOT_BEFORE, 0), to = get(ps, CERTIFICATE_NOT_AFTER, 2461449600000L);
            byte[] sigAlg = alg == EC ? tlv(0x30, oid(1, 2, 840, 10045, 4, 3, 2))
                                      : tlv(0x30, oid(1, 2, 840, 113549, 1, 1, 11), new byte[] { 5, 0 });
            byte[] tbs = tlv(0x30,
                    tlv(0xa0, tlv(0x02, new byte[] { 2 })),        /* v3 */
                    tlv(0x02, sn.toByteArray()),
                    sigAlg, subject,
                    tlv(0x30, time(from), time(to)),
                    subject, pub.getEncoded());
            Signature s = (Signature) jca("Signature", alg == EC ? "SHA256withECDSA" : "SHA256withRSA");
            s.initSign(priv);
            s.update(tbs);
            byte[] sig = s.sign();
            byte[] bits = new byte[sig.length + 1];
            System.arraycopy(sig, 0, bits, 1, sig.length);
            return tlv(0x30, tbs, sigAlg, tlv(0x03, bits));
        }
    }
}
