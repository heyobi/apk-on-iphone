package aoi;

import android.os.Binder;
import android.os.Parcel;

/** "media.player" (native IMediaPlayerService), for its codec list: without the service
 *  libstagefright builds the list in the app's process from the Codec2 HALs, which ask
 *  hwservicemanager and abort without it (Chromium/WebView reads the list at start).
 *  Here the list is empty: no hardware codecs. The other calls answer nothing (a null
 *  binder: MediaPlayer's setDataSource fails instead). Native parcels: no exception
 *  header, the interface token is not checked. */
final class MediaPlayerService extends Binder {
    static final String NAME = "media.player";
    private static final int GET_CODEC_LIST = 7;          /* IMediaPlayerService.cpp */

    private final CodecList list = new CodecList();

    @Override protected boolean onTransact(int code, Parcel data, Parcel reply, int flags) {
        if (code >= 0x00ffffff) return false;             /* PING, INTERFACE, DUMP...: Binder's */
        if (code == GET_CODEC_LIST && reply != null) reply.writeStrongBinder(list);
        return true;
    }

    /** IMediaCodecList with no codecs. */
    static final class CodecList extends Binder {
        private static final int COUNT_CODECS = 2, GET_CODEC_INFO = 3, GET_GLOBAL_SETTINGS = 4,
                FIND_CODEC_BY_TYPE = 5, FIND_CODEC_BY_NAME = 6;
        private static final int NAME_NOT_FOUND = -2, UNKNOWN_ERROR = 0x80000000;

        @Override protected boolean onTransact(int code, Parcel data, Parcel reply, int flags) {
            if (code >= 0x00ffffff) return false;
            if (reply == null) return true;
            switch (code) {
                case COUNT_CODECS: reply.writeInt(0); break;
                case GET_CODEC_INFO: case GET_GLOBAL_SETTINGS: reply.writeInt(UNKNOWN_ERROR); break;
                case FIND_CODEC_BY_TYPE: case FIND_CODEC_BY_NAME: reply.writeInt(NAME_NOT_FOUND); break;
                default: break;
            }
            return true;
        }
    }
}
