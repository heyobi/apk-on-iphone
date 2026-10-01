package android.app.servertransaction;

import android.app.ActivityOptions;
import android.app.IActivityClientController;
import android.app.ProfilerInfo;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.res.Configuration;
import android.os.Bundle;
import android.os.IBinder;
import android.os.PersistableBundle;
import android.window.ActivityWindowInfo;
import com.android.internal.app.IVoiceInteractor;
import java.util.List;

/** Signature from the guest's framework.jar (tools/dexsig.py). */
public class LaunchActivityItem extends ClientTransactionItem {
    public static LaunchActivityItem obtain(IBinder activityToken, Intent intent, int ident, ActivityInfo info,
            Configuration curConfig, Configuration overrideConfig, int deviceId, String referrer,
            IVoiceInteractor voiceInteractor, int procState, Bundle state, PersistableBundle persistentState,
            List pendingResults, List pendingNewIntents, ActivityOptions.SceneTransitionInfo sceneTransitionInfo,
            boolean isForward, ProfilerInfo profilerInfo, IBinder assistToken,
            IActivityClientController activityClientController, IBinder shareableActivityToken,
            boolean launchedFromBubble, IBinder taskFragmentToken, IBinder initialCallerInfoAccessToken,
            ActivityWindowInfo activityWindowInfo) {
        return null;
    }
}
