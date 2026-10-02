package android.os;

public final class Parcel {
    public void writeNoException() {}
    public void setDataSize(int size) {}
    public void setDataPosition(int pos) {}
    public void enforceInterface(String descriptor) {}
    public int dataPosition() { return 0; }
    public String readString() { return null; }
    public int readInt() { return 0; }
    public void writeInt(int v) {}
    public void writeIntArray(int[] v) {}
}
