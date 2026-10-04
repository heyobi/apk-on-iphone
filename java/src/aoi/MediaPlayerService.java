package aoi;

import android.os.Binder;
import android.os.Parcel;

/** "media.player" (native IMediaPlayerService), which MediaCodecList asks first for a
 *  codec list: it answers none (a null binder), so libstagefright builds the list in
 *  the app's process, from the software codecs registered there (Main.codecs,
 *  guest/media.c). The other calls answer nothing (MediaPlayer's setDataSource fails
 *  instead). Native parcels: no exception header, the interface token is not checked. */
final class MediaPlayerService extends Binder {
    static final String NAME = "media.player";
    private static final int GET_CODEC_LIST = 7;          /* IMediaPlayerService.cpp */

    @Override protected boolean onTransact(int code, Parcel data, Parcel reply, int flags) {
        if (code >= 0x00ffffff) return false;             /* PING, INTERFACE, DUMP...: Binder's */
        if (code == GET_CODEC_LIST && reply != null) reply.writeStrongBinder(null);
        return true;
    }
}
