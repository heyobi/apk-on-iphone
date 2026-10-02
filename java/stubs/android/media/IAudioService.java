package android.media;

import android.os.Binder;
import android.os.RemoteException;

/** Hidden; signatures from the guest's framework.jar (tools/dexsig.py). Only the calls
 *  aoi.AudioService answers. */
public interface IAudioService {
    boolean areNavigationRepeatSoundEffectsEnabled() throws RemoteException;
    boolean isHomeSoundEffectEnabled() throws RemoteException;
    void playSoundEffect(int effectType, int userId) throws RemoteException;
    void playSoundEffectVolume(int effectType, float volume) throws RemoteException;
    boolean loadSoundEffects() throws RemoteException;
    void unloadSoundEffects() throws RemoteException;
    int getMode() throws RemoteException;
    int getRingerModeExternal() throws RemoteException;
    int getRingerModeInternal() throws RemoteException;
    int getStreamVolume(int streamType) throws RemoteException;
    int getStreamMaxVolume(int streamType) throws RemoteException;
    int getStreamMinVolume(int streamType) throws RemoteException;
    int getStreamTypeAlias(int streamType) throws RemoteException;
    int getUiSoundsStreamType() throws RemoteException;
    boolean isStreamMute(int streamType) throws RemoteException;
    boolean isStreamAffectedByMute(int streamType) throws RemoteException;
    boolean isStreamAffectedByRingerMode(int streamType) throws RemoteException;
    boolean isMasterMute() throws RemoteException;
    boolean isMicrophoneMuted() throws RemoteException;
    boolean isMusicActive(boolean remotely) throws RemoteException;
    boolean isSpeakerphoneOn() throws RemoteException;
    boolean isCameraSoundForced() throws RemoteException;

    abstract class Stub extends Binder implements IAudioService {
        public Stub(android.os.PermissionEnforcer enforcer) {}
    }
}
