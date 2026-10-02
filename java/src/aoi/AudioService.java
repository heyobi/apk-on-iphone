package aoi;

import android.media.IAudioService;

/** Audio, as far as views and AudioManager ask: no sound yet. A click's sound effect
 *  (View.performClick -> ViewRootImpl.playSoundEffect) asks it; without the service
 *  AudioManager dereferences null and the app dies on the first click of a text
 *  toolbar's Copy or Paste. */
final class AudioService extends IAudioService.Stub {
    /* Calls go through Stub.onTransact, and one we have not written gets the default
     * answer instead of an AbstractMethodError (aoi.Services). */
    @Override public android.os.IInterface queryLocalInterface(String descriptor) { return null; }
    @Override protected boolean onTransact(int code, android.os.Parcel data, android.os.Parcel reply, int flags)
            throws android.os.RemoteException {
        try { return super.onTransact(code, data, reply, flags); }
        catch (AbstractMethodError e) { return Services.missing(this, e, reply); }
    }

    AudioService() { super(GrantAll.INSTANCE); }

    @Override public boolean areNavigationRepeatSoundEffectsEnabled() { return false; }
    @Override public boolean isHomeSoundEffectEnabled() { return false; }
    @Override public void playSoundEffect(int effectType, int userId) {}
    @Override public void playSoundEffectVolume(int effectType, float volume) {}
    @Override public boolean loadSoundEffects() { return true; }
    @Override public void unloadSoundEffects() {}
    @Override public int getMode() { return 0; }                          /* MODE_NORMAL */
    @Override public int getRingerModeExternal() { return 2; }            /* RINGER_MODE_NORMAL */
    @Override public int getRingerModeInternal() { return 2; }
    @Override public int getStreamVolume(int s) { return 8; }
    @Override public int getStreamMaxVolume(int s) { return 15; }
    @Override public int getStreamMinVolume(int s) { return 0; }
    @Override public int getStreamTypeAlias(int s) { return s; }
    @Override public int getUiSoundsStreamType() { return 1; }           /* STREAM_SYSTEM */
    @Override public boolean isStreamMute(int s) { return false; }
    @Override public boolean isStreamAffectedByMute(int s) { return true; }
    @Override public boolean isStreamAffectedByRingerMode(int s) { return false; }
    @Override public boolean isMasterMute() { return false; }
    @Override public boolean isMicrophoneMuted() { return false; }
    @Override public boolean isMusicActive(boolean remotely) { return false; }
    @Override public boolean isSpeakerphoneOn() { return false; }
    @Override public boolean isCameraSoundForced() { return false; }
}
