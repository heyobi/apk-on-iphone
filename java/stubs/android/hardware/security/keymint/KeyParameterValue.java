package android.hardware.security.keymint;

public final class KeyParameterValue {
    public static final int invalid = 0, algorithm = 1, blockMode = 2, paddingMode = 3, digest = 4, ecCurve = 5,
            origin = 6, keyPurpose = 7, hardwareAuthenticatorType = 8, securityLevel = 9, boolValue = 10,
            integer = 11, longInteger = 12, dateTime = 13, blob = 14;
    public int getTag() { return 0; }
    public static KeyParameterValue invalid(int v) { return null; }
    public static KeyParameterValue algorithm(int v) { return null; }
    public static KeyParameterValue blockMode(int v) { return null; }
    public static KeyParameterValue paddingMode(int v) { return null; }
    public static KeyParameterValue digest(int v) { return null; }
    public static KeyParameterValue ecCurve(int v) { return null; }
    public static KeyParameterValue origin(int v) { return null; }
    public static KeyParameterValue keyPurpose(int v) { return null; }
    public static KeyParameterValue hardwareAuthenticatorType(int v) { return null; }
    public static KeyParameterValue securityLevel(int v) { return null; }
    public static KeyParameterValue boolValue(boolean v) { return null; }
    public static KeyParameterValue integer(int v) { return null; }
    public static KeyParameterValue longInteger(long v) { return null; }
    public static KeyParameterValue dateTime(long v) { return null; }
    public static KeyParameterValue blob(byte[] v) { return null; }
    public int getInvalid() { return 0; }
    public int getAlgorithm() { return 0; }
    public int getBlockMode() { return 0; }
    public int getPaddingMode() { return 0; }
    public int getDigest() { return 0; }
    public int getEcCurve() { return 0; }
    public int getOrigin() { return 0; }
    public int getKeyPurpose() { return 0; }
    public int getHardwareAuthenticatorType() { return 0; }
    public int getSecurityLevel() { return 0; }
    public boolean getBoolValue() { return false; }
    public int getInteger() { return 0; }
    public long getLongInteger() { return 0; }
    public long getDateTime() { return 0; }
    public byte[] getBlob() { return null; }
}
