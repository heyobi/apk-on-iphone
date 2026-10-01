package android.view;

/** Hidden; only the fields our DisplayManager sets (all present in the guest's framework). */
public final class DisplayInfo {
    public int layerStack, flags, type, displayId, displayGroupId;
    public String name, uniqueId;
    public int appWidth, appHeight, smallestNominalAppWidth, smallestNominalAppHeight;
    public int largestNominalAppWidth, largestNominalAppHeight, logicalWidth, logicalHeight;
    public int rotation, modeId, defaultModeId, userPreferredModeId, colorMode, logicalDensityDpi;
    public int state, committedState, installOrientation;
    public float renderFrameRate, physicalXDpi, physicalYDpi, refreshRateOverride;
    public float brightnessMinimum, brightnessMaximum, brightnessDefault;
    public long appVsyncOffsetNanos, presentationDeadlineNanos;
    public Display.Mode[] supportedModes;
    public int[] supportedColorModes, userDisabledHdrTypes;
    public DisplayInfo() {}
}
