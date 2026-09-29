package com.andwayland.companion;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

/**
 * Immutable State Models representing the Wayland compositor runtime
 * and connected client sessions.
 */
public final class CompositorState {

    public static final class WaylandSession {
        public final int pid;
        public final String appId;
        public final String title;
        public final float ramMb;
        public final long startTimeMs;
        public final boolean isNativeTouch;

        public WaylandSession(int pid, String appId, String title, float ramMb, long startTimeMs, boolean isNativeTouch) {
            this.pid = pid;
            this.appId = appId;
            this.title = title;
            this.ramMb = ramMb;
            this.startTimeMs = startTimeMs;
            this.isNativeTouch = isNativeTouch;
        }

        public String getFormattedRuntime() {
            long now = System.currentTimeMillis();
            long diffSec = Math.max(0, (now - startTimeMs) / 1000);
            long mins = diffSec / 60;
            long secs = diffSec % 60;
            if (mins >= 60) {
                long hours = mins / 60;
                mins = mins % 60;
                return String.format("%02dh %02dm", hours, mins);
            }
            return String.format("%02dm %02ds", mins, secs);
        }
    }

    public final boolean isRunning;
    public final int pid;
    public final String socketPath;
    public final int screenWidth;
    public final int screenHeight;
    public final int activeSurfacesCount;
    public final float currentRamMb;
    public final float minRamMb;
    public final float peakRamMb;
    public final List<WaylandSession> sessions;
    public final List<Float> recentRamHistory;
    public final long timestamp;

    public CompositorState(
            boolean isRunning,
            int pid,
            String socketPath,
            int screenWidth,
            int screenHeight,
            int activeSurfacesCount,
            float currentRamMb,
            float minRamMb,
            float peakRamMb,
            List<WaylandSession> sessions,
            List<Float> recentRamHistory,
            long timestamp) {
        this.isRunning = isRunning;
        this.pid = pid;
        this.socketPath = socketPath;
        this.screenWidth = screenWidth;
        this.screenHeight = screenHeight;
        this.activeSurfacesCount = activeSurfacesCount;
        this.currentRamMb = currentRamMb;
        this.minRamMb = minRamMb;
        this.peakRamMb = peakRamMb;
        this.sessions = sessions != null ? Collections.unmodifiableList(new ArrayList<>(sessions)) : Collections.emptyList();
        this.recentRamHistory = recentRamHistory != null ? Collections.unmodifiableList(new ArrayList<>(recentRamHistory)) : Collections.emptyList();
        this.timestamp = timestamp;
    }

    public static CompositorState initial() {
        return new CompositorState(
                false,
                0,
                "/data/wayland/wayland-0",
                684,
                1520,
                0,
                0.0f,
                0.0f,
                0.0f,
                Collections.emptyList(),
                Collections.emptyList(),
                System.currentTimeMillis()
        );
    }
}
