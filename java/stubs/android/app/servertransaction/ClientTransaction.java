package android.app.servertransaction;

import android.app.IApplicationThread;

public class ClientTransaction {
    public static ClientTransaction obtain(IApplicationThread client) { return null; }
    public void addTransactionItem(ClientTransactionItem item) {}
}
