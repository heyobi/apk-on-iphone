package android.system.keystore2;
public class KeyMetadata {
    public KeyDescriptor key; public int keySecurityLevel; public Authorization[] authorizations;
    public byte[] certificate; public byte[] certificateChain; public long modificationTimeMs;
}
