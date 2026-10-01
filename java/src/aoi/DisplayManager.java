package aoi;

import android.graphics.Point;
import android.hardware.display.IDisplayManager;
import android.hardware.display.IDisplayManagerCallback;
import android.view.Display;
import android.view.DisplayInfo;

/** One display, 0: the iPhone's screen as Android sees it — 393 x 852 dp, the iPhone's
 *  points, at 2x: 786 x 1704 pixels, 320 dpi, 60 Hz, on. (The phone's panel is 3x, but
 *  every frame is drawn in software and Skia's rasterizing is ~90 % of a frame: 2x is
 *  2.25 times fewer pixels; the iPhone scales the frames up.) */
final class DisplayManager extends IDisplayManager.Stub {
    static final int WIDTH = 786, HEIGHT = 1704, DPI = 320;
    static final float HZ = 60f;

    DisplayManager() { super(GrantAll.INSTANCE); }

    private static Display.Mode mode() { return new Display.Mode(1, WIDTH, HEIGHT, HZ); }

    @Override
    public DisplayInfo getDisplayInfo(int id) {
        if (id != 0) return null;
        DisplayInfo d = new DisplayInfo();
        d.displayId = 0; d.layerStack = 0; d.displayGroupId = 0;
        d.type = 1;                                                /* TYPE_INTERNAL */
        d.flags = 0;
        d.name = "iPhone"; d.uniqueId = "local:0";
        d.appWidth = d.logicalWidth = WIDTH; d.appHeight = d.logicalHeight = HEIGHT;
        d.smallestNominalAppWidth = d.smallestNominalAppHeight = WIDTH;
        d.largestNominalAppWidth = d.largestNominalAppHeight = HEIGHT;
        d.rotation = 0; d.installOrientation = 0;
        d.modeId = d.defaultModeId = d.userPreferredModeId = 1;
        d.supportedModes = new Display.Mode[] { mode() };
        d.renderFrameRate = HZ; d.refreshRateOverride = 0f;
        d.colorMode = 0; d.supportedColorModes = new int[] { 0 }; d.userDisabledHdrTypes = new int[0];
        d.logicalDensityDpi = DPI; d.physicalXDpi = d.physicalYDpi = 460f;   /* the real panel */
        d.appVsyncOffsetNanos = 0; d.presentationDeadlineNanos = 16666667L;
        d.state = d.committedState = 2;                            /* STATE_ON */
        d.brightnessMinimum = 0f; d.brightnessMaximum = 1f; d.brightnessDefault = 0.5f;
        return d;
    }

    @Override public int[] getDisplayIds(boolean includeDisabled) { return new int[] { 0 }; }
    @Override public void registerCallback(IDisplayManagerCallback cb) {}
    @Override public void registerCallbackWithEventMask(IDisplayManagerCallback cb, long mask) {}
    @Override public Point getStableDisplaySize() { return new Point(WIDTH, HEIGHT); }
    @Override public int getPreferredWideGamutColorSpaceId() { return 0; }   /* sRGB */
    @Override public float getBrightness(int id) { return 0.5f; }
    @Override public int getRefreshRateSwitchingType() { return 0; }
    @Override public boolean shouldAlwaysRespectAppRequestedMode() { return false; }
    @Override public boolean isMinimalPostProcessingRequested(int id) { return false; }
    @Override public Display.Mode getUserPreferredDisplayMode(int id) { return mode(); }
    @Override public Display.Mode getSystemPreferredDisplayMode(int id) { return mode(); }
    @Override public int[] getSupportedHdrOutputTypes() { return new int[0]; }
    @Override public android.hardware.OverlayProperties getOverlaySupport() { return android.hardware.OverlayProperties.getDefault(); }
}
