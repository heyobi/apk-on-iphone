package android.os;

public interface Parcelable {
    void writeToParcel(Parcel dest, int flags);

    interface Creator<T> {
        T createFromParcel(Parcel source);
    }
}
