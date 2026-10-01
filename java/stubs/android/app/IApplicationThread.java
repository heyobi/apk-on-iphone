package android.app;

import android.content.AutofillOptions;
import android.content.ComponentName;
import android.content.ContentCaptureOptions;
import android.content.pm.ApplicationInfo;
import android.content.pm.ProviderInfoList;
import android.content.res.CompatibilityInfo;
import android.content.res.Configuration;
import android.os.Bundle;
import android.os.RemoteException;
import android.os.SharedMemory;
import java.util.Map;

/** Hidden; signatures from the guest's framework.jar (tools/dexsig.py). */
public interface IApplicationThread {
    void bindApplication(String packageName, ApplicationInfo info, String sdkSandboxClientAppVolumeUuid,
            String sdkSandboxClientAppPackage, boolean isSdkInSandbox, ProviderInfoList providerList,
            ComponentName testName, ProfilerInfo profilerInfo, Bundle testArguments,
            IInstrumentationWatcher testWatcher, IUiAutomationConnection uiAutomationConnection, int debugMode,
            boolean enableBinderTracking, boolean trackAllocation, boolean restrictedBackupMode, boolean persistent,
            Configuration config, CompatibilityInfo compatInfo, Map services, Bundle coreSettings,
            String buildSerial, AutofillOptions autofillOptions, ContentCaptureOptions contentCaptureOptions,
            long[] disabledCompatChanges, SharedMemory serializedSystemFontMap, long startRequestedElapsedTime,
            long startRequestedUptime) throws RemoteException;
    void scheduleTransaction(android.app.servertransaction.ClientTransaction transaction) throws RemoteException;
}
