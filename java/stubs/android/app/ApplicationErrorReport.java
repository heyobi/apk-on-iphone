package android.app;

public class ApplicationErrorReport {
    public static class CrashInfo {
        public String exceptionClassName, exceptionMessage, throwClassName, throwMethodName, stackTrace;
        public int throwLineNumber;
    }

    public static class ParcelableCrashInfo extends CrashInfo {
    }
}
