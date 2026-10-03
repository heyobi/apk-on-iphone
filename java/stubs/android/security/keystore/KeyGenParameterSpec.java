package android.security.keystore;

public final class KeyGenParameterSpec implements java.security.spec.AlgorithmParameterSpec {
    public static final class Builder {
        public Builder(String alias, int purposes) {}
        public Builder setKeySize(int bits) { return this; }
        public Builder setBlockModes(String... m) { return this; }
        public Builder setEncryptionPaddings(String... p) { return this; }
        public Builder setSignaturePaddings(String... p) { return this; }
        public Builder setDigests(String... d) { return this; }
        public Builder setAlgorithmParameterSpec(java.security.spec.AlgorithmParameterSpec s) { return this; }
        public KeyGenParameterSpec build() { return null; }
    }
}
