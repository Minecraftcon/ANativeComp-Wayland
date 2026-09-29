package com.andwayland.companion;

import android.animation.ValueAnimator;
import android.app.Activity;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.content.Intent;
import android.graphics.Color;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.VibrationEffect;
import android.os.Vibrator;
import android.view.LayoutInflater;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.animation.DecelerateInterpolator;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.Switch;
import android.widget.FrameLayout;
import android.widget.TextView;
import android.widget.Toast;

import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.util.List;

/**
 * Main Dashboard Activity for ANativeDrawer Wayland Compositor.
 */
public class ANativeDrawerActivity extends Activity {

    private enum Tab {
        OVERVIEW, SESSIONS, PREFERENCES, TOOLS
    }

    private Tab currentTab = Tab.OVERVIEW;

    // Header
    private View viewStatusDot;
    private TextView tvHeaderStatus;

    // Tab bar containers
    private LinearLayout tabOverviewContainer;
    private LinearLayout tabSessionsContainer;
    private LinearLayout tabPreferencesContainer;
    private LinearLayout tabToolsContainer;

    // Tab icon ImageViews
    private ImageView ivTabOverview, ivTabSessions, ivTabPreferences, ivTabTools;
    // Tab label TextViews
    private TextView tvTabOverview, tvTabSessions, tvTabPreferences, tvTabTools;
    // Tab indicator Views
    private View indicatorOverview, indicatorSessions, indicatorPreferences, indicatorTools;

    private FrameLayout tabContainer;

    // Cached tab views
    private View viewOverview;
    private View viewSessions;
    private View viewPreferences;
    private View viewTools;

    private Runnable unsubscribeStateFlow;
    private PreferencesManager prefsManager;
    private final Handler mainHandler = new Handler(Looper.getMainLooper());

    // Color constants (no hardcoded neon)
    private static final int COLOR_STATUS_GREEN   = 0xFF34C759;
    private static final int COLOR_STATUS_RED     = 0xFFFF453A;
    private static final int COLOR_TEXT_PRIMARY   = 0xFFEAEAEA;
    private static final int COLOR_TEXT_SECONDARY = 0xFF8E8E93;
    private static final int COLOR_TEXT_MUTED     = 0xFF545458;
    private static final int COLOR_ICON_DEFAULT   = 0xFF6E6E72;
    private static final int COLOR_ICON_ACTIVE    = 0xFFEAEAEA;
    private static final int COLOR_TRANSPARENT    = 0x00000000;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);

        prefsManager = new PreferencesManager(this);

        Intent serviceIntent = new Intent(this, CompositorMonitorService.class);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            startForegroundService(serviceIntent);
        } else {
            startService(serviceIntent);
        }

        initViews();
        setupTabs();
        selectTab(Tab.OVERVIEW, false);

        unsubscribeStateFlow = CompositorRepository.getInstance().getStateFlow().subscribe(this::renderState);
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        if (unsubscribeStateFlow != null) {
            unsubscribeStateFlow.run();
        }
    }

    private void initViews() {
        viewStatusDot   = findViewById(R.id.view_status_dot);
        tvHeaderStatus  = findViewById(R.id.tv_header_status);
        tabContainer    = findViewById(R.id.tab_container);

        tabOverviewContainer     = findViewById(R.id.btn_tab_overview);
        tabSessionsContainer     = findViewById(R.id.btn_tab_sessions);
        tabPreferencesContainer  = findViewById(R.id.btn_tab_preferences);
        tabToolsContainer        = findViewById(R.id.btn_tab_tools);

        ivTabOverview    = findViewById(R.id.iv_tab_overview);
        ivTabSessions    = findViewById(R.id.iv_tab_sessions);
        ivTabPreferences = findViewById(R.id.iv_tab_preferences);
        ivTabTools       = findViewById(R.id.iv_tab_tools);

        tvTabOverview    = findViewById(R.id.tv_tab_overview);
        tvTabSessions    = findViewById(R.id.tv_tab_sessions);
        tvTabPreferences = findViewById(R.id.tv_tab_preferences);
        tvTabTools       = findViewById(R.id.tv_tab_tools);

        indicatorOverview    = findViewById(R.id.indicator_overview);
        indicatorSessions    = findViewById(R.id.indicator_sessions);
        indicatorPreferences = findViewById(R.id.indicator_preferences);
        indicatorTools       = findViewById(R.id.indicator_tools);

        LayoutInflater inflater = LayoutInflater.from(this);
        viewOverview    = inflater.inflate(R.layout.tab_overview, tabContainer, false);
        viewSessions    = inflater.inflate(R.layout.tab_sessions, tabContainer, false);
        viewPreferences = inflater.inflate(R.layout.tab_preferences, tabContainer, false);
        viewTools       = inflater.inflate(R.layout.tab_tools, tabContainer, false);

        initOverviewTab();
        initSessionsTab();
        initPreferencesTab();
        initToolsTab();
    }

    private void setupTabs() {
        applyPressScale(tabOverviewContainer);
        applyPressScale(tabSessionsContainer);
        applyPressScale(tabPreferencesContainer);
        applyPressScale(tabToolsContainer);

        tabOverviewContainer.setOnClickListener(v -> selectTab(Tab.OVERVIEW, true));
        tabSessionsContainer.setOnClickListener(v -> selectTab(Tab.SESSIONS, true));
        tabPreferencesContainer.setOnClickListener(v -> selectTab(Tab.PREFERENCES, true));
        tabToolsContainer.setOnClickListener(v -> selectTab(Tab.TOOLS, true));
    }

    /** Animate tab content swap with a short cross-fade */
    private void selectTab(Tab tab, boolean animate) {
        currentTab = tab;

        // Update tab visual states
        setTabActive(Tab.OVERVIEW,     tab == Tab.OVERVIEW);
        setTabActive(Tab.SESSIONS,     tab == Tab.SESSIONS);
        setTabActive(Tab.PREFERENCES,  tab == Tab.PREFERENCES);
        setTabActive(Tab.TOOLS,        tab == Tab.TOOLS);

        final View newView;
        switch (tab) {
            case SESSIONS:    newView = viewSessions;    break;
            case PREFERENCES: newView = viewPreferences; break;
            case TOOLS:       newView = viewTools;       refreshLogs(); break;
            default:          newView = viewOverview;    break;
        }

        FrameLayout.LayoutParams matchParams = new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT);

        if (!animate || tabContainer.getChildCount() == 0) {
            tabContainer.removeAllViews();
            tabContainer.addView(newView, matchParams);
        } else {
            View outgoing = tabContainer.getChildAt(0);
            if (outgoing == newView) return;
            outgoing.animate()
                    .alpha(0f)
                    .setDuration(100)
                    .setInterpolator(new DecelerateInterpolator())
                    .withEndAction(() -> {
                        tabContainer.removeAllViews();
                        newView.setAlpha(0f);
                        tabContainer.addView(newView, matchParams);
                        newView.animate()
                                .alpha(1f)
                                .setDuration(150)
                                .setInterpolator(new DecelerateInterpolator())
                                .start();
                    }).start();
        }

        renderState(CompositorRepository.getInstance().getStateFlow().getValue());
    }

    private void setTabActive(Tab tab, boolean active) {
        ImageView iv;
        TextView tv;
        View indicator;
        switch (tab) {
            case SESSIONS:    iv = ivTabSessions;    tv = tvTabSessions;    indicator = indicatorSessions;    break;
            case PREFERENCES: iv = ivTabPreferences; tv = tvTabPreferences; indicator = indicatorPreferences; break;
            case TOOLS:       iv = ivTabTools;       tv = tvTabTools;       indicator = indicatorTools;       break;
            default:          iv = ivTabOverview;    tv = tvTabOverview;    indicator = indicatorOverview;    break;
        }
        if (active) {
            iv.setColorFilter(COLOR_ICON_ACTIVE);
            tv.setTextColor(COLOR_TEXT_PRIMARY);
            indicator.setBackgroundResource(R.drawable.tab_indicator);
        } else {
            iv.setColorFilter(COLOR_ICON_DEFAULT);
            tv.setTextColor(COLOR_TEXT_MUTED);
            indicator.setBackgroundColor(COLOR_TRANSPARENT);
        }
    }

    /** Attach a subtle scale-down on press to any view */
    private void applyPressScale(View v) {
        v.setOnTouchListener((view, event) -> {
            switch (event.getAction()) {
                case MotionEvent.ACTION_DOWN:
                    view.animate().scaleX(0.94f).scaleY(0.94f).setDuration(80).start();
                    break;
                case MotionEvent.ACTION_UP:
                case MotionEvent.ACTION_CANCEL:
                    view.animate().scaleX(1f).scaleY(1f).setDuration(120).start();
                    break;
            }
            return false;
        });
    }

    private void triggerHaptic() {
        try {
            Vibrator vib = (Vibrator) getSystemService(Context.VIBRATOR_SERVICE);
            if (vib != null && vib.hasVibrator()) {
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                    vib.vibrate(VibrationEffect.createOneShot(22, VibrationEffect.DEFAULT_AMPLITUDE));
                } else {
                    vib.vibrate(22);
                }
            }
        } catch (Exception ignored) {}
    }

    // ==========================================
    // 1. Overview Tab
    // ==========================================
    private TextView tvOverviewStatusBadge;
    private TextView tvOverviewSocket;
    private TextView tvOverviewPid;
    private TextView tvOverviewRes;
    private LinearLayout btnOverviewRestart;
    private LinearLayout btnOverviewTogglePower;
    private TextView tvOverviewTabCount;
    private TextView tvOverviewSurfaceSubtext;
    private TextView tvOverviewCurrentRam;
    private TextView tvOverviewMinRam;
    private TextView tvOverviewPeakRam;
    private TextView tvPowerLabel;
    private ImageView ivPowerIcon;
    private RamGraphView ramGraphView;
    private float lastRamValue = 0f;

    private void initOverviewTab() {
        tvOverviewStatusBadge  = viewOverview.findViewById(R.id.tv_overview_status_badge);
        tvOverviewSocket       = viewOverview.findViewById(R.id.tv_overview_socket);
        tvOverviewPid          = viewOverview.findViewById(R.id.tv_overview_pid);
        tvOverviewRes          = viewOverview.findViewById(R.id.tv_overview_res);
        btnOverviewRestart     = viewOverview.findViewById(R.id.btn_overview_restart);
        btnOverviewTogglePower = viewOverview.findViewById(R.id.btn_overview_toggle_power);
        tvOverviewTabCount     = viewOverview.findViewById(R.id.tv_overview_tab_count);
        tvOverviewSurfaceSubtext = viewOverview.findViewById(R.id.tv_overview_surface_subtext);
        tvOverviewCurrentRam   = viewOverview.findViewById(R.id.tv_overview_current_ram);
        tvOverviewMinRam       = viewOverview.findViewById(R.id.tv_ram_min);
        tvOverviewPeakRam      = viewOverview.findViewById(R.id.tv_ram_peak);
        tvPowerLabel           = viewOverview.findViewById(R.id.tv_power_label);
        ivPowerIcon            = viewOverview.findViewById(R.id.iv_power_icon);
        ramGraphView           = viewOverview.findViewById(R.id.graph_ram_sparkline);

        applyPressScale(btnOverviewRestart);
        applyPressScale(btnOverviewTogglePower);

        btnOverviewRestart.setOnClickListener(v -> {
            triggerHaptic();
            CompositorRepository.getInstance().restartDaemon();
            Toast.makeText(this, "Restarting…", Toast.LENGTH_SHORT).show();
        });

        btnOverviewTogglePower.setOnClickListener(v -> {
            triggerHaptic();
            CompositorState state = CompositorRepository.getInstance().getStateFlow().getValue();
            if (state.isRunning) {
                CompositorRepository.getInstance().stopDaemon();
                Toast.makeText(this, "Stopping compositor…", Toast.LENGTH_SHORT).show();
            } else {
                CompositorRepository.getInstance().restartDaemon();
                Toast.makeText(this, "Starting compositor…", Toast.LENGTH_SHORT).show();
            }
        });
    }

    // ==========================================
    // 2. Sessions Tab
    // ==========================================
    private LinearLayout llSessionsList;
    private LinearLayout llSessionsEmpty;
    private LinearLayout btnSessionsRefresh;
    private TextView tvSessionsCount;

    private void initSessionsTab() {
        llSessionsList    = viewSessions.findViewById(R.id.ll_sessions_list);
        llSessionsEmpty   = viewSessions.findViewById(R.id.ll_sessions_empty);
        btnSessionsRefresh = viewSessions.findViewById(R.id.btn_sessions_refresh);
        tvSessionsCount   = viewSessions.findViewById(R.id.tv_sessions_count);

        applyPressScale(btnSessionsRefresh);
        btnSessionsRefresh.setOnClickListener(v -> {
            triggerHaptic();
            btnSessionsRefresh.animate().rotation(360f).setDuration(400).withEndAction(
                    () -> btnSessionsRefresh.setRotation(0f)).start();
            CompositorRepository.getInstance().pollSync();
        });
    }

    // ==========================================
    // 3. Preferences Tab
    // ==========================================
    private TextView tvPrefHoldVal;
    private SeekBar sbPrefHold;
    private TextView tvPrefScrollVal;
    private SeekBar sbPrefScroll;
    private TextView tvPrefDoubleTapVal;
    private SeekBar sbPrefDoubleTap;
    private Switch swPrefInvertScroll;
    private Switch swPrefSsd;
    private Switch swPrefNotification;
    private LinearLayout btnPrefSave;

    private void initPreferencesTab() {
        tvPrefHoldVal      = viewPreferences.findViewById(R.id.tv_pref_hold_val);
        sbPrefHold         = viewPreferences.findViewById(R.id.sb_pref_hold);
        tvPrefScrollVal    = viewPreferences.findViewById(R.id.tv_pref_scroll_val);
        sbPrefScroll       = viewPreferences.findViewById(R.id.sb_pref_scroll);
        tvPrefDoubleTapVal = viewPreferences.findViewById(R.id.tv_pref_doubletap_val);
        sbPrefDoubleTap    = viewPreferences.findViewById(R.id.sb_pref_doubletap);
        swPrefInvertScroll = viewPreferences.findViewById(R.id.sw_pref_invert_scroll);
        swPrefSsd          = viewPreferences.findViewById(R.id.sw_pref_ssd);
        swPrefNotification = viewPreferences.findViewById(R.id.sw_pref_notification);
        btnPrefSave        = viewPreferences.findViewById(R.id.btn_pref_save);

        int holdOffset = Math.max(0, Math.min(800, prefsManager.holdThresholdMs - 200));
        sbPrefHold.setProgress(holdOffset);
        tvPrefHoldVal.setText(prefsManager.holdThresholdMs + " ms");
        sbPrefHold.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(SeekBar s, int p, boolean f) {
                int val = 200 + p;
                tvPrefHoldVal.setText(val + " ms");
                prefsManager.holdThresholdMs = val;
            }
            @Override public void onStartTrackingTouch(SeekBar s) {}
            @Override public void onStopTrackingTouch(SeekBar s) { triggerHaptic(); }
        });

        int scrollOffset = Math.max(0, Math.min(250, (int)(prefsManager.scrollSensitivity * 100) - 50));
        sbPrefScroll.setProgress(scrollOffset);
        tvPrefScrollVal.setText((int)(prefsManager.scrollSensitivity * 100) + " %");
        sbPrefScroll.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(SeekBar s, int p, boolean f) {
                int pct = 50 + p;
                tvPrefScrollVal.setText(pct + " %");
                prefsManager.scrollSensitivity = pct / 100.0f;
            }
            @Override public void onStartTrackingTouch(SeekBar s) {}
            @Override public void onStopTrackingTouch(SeekBar s) { triggerHaptic(); }
        });

        int doubleTapOffset = Math.max(0, Math.min(350, prefsManager.doubleTapMs - 150));
        sbPrefDoubleTap.setProgress(doubleTapOffset);
        tvPrefDoubleTapVal.setText(prefsManager.doubleTapMs + " ms");
        sbPrefDoubleTap.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(SeekBar s, int p, boolean f) {
                int val = 150 + p;
                tvPrefDoubleTapVal.setText(val + " ms");
                prefsManager.doubleTapMs = val;
            }
            @Override public void onStartTrackingTouch(SeekBar s) {}
            @Override public void onStopTrackingTouch(SeekBar s) { triggerHaptic(); }
        });

        swPrefInvertScroll.setChecked(prefsManager.invertScroll);
        swPrefInvertScroll.setOnCheckedChangeListener((b, checked) -> {
            triggerHaptic();
            prefsManager.invertScroll = checked;
        });

        swPrefSsd.setChecked(prefsManager.forceSsd);
        swPrefSsd.setOnCheckedChangeListener((b, checked) -> {
            triggerHaptic();
            prefsManager.forceSsd = checked;
        });

        swPrefNotification.setChecked(prefsManager.persistentNotification);
        swPrefNotification.setOnCheckedChangeListener((b, checked) -> {
            triggerHaptic();
            prefsManager.persistentNotification = checked;
        });

        applyPressScale(btnPrefSave);
        btnPrefSave.setOnClickListener(v -> {
            triggerHaptic();
            prefsManager.save();
            Toast.makeText(this, "Changes applied", Toast.LENGTH_SHORT).show();
        });
    }

    // ==========================================
    // 4. Tools & Logs Tab
    // ==========================================
    private TextView tvLogContent;
    private ScrollView svLogContainer;

    private void initToolsTab() {
        LinearLayout btnLaunchFoot       = viewTools.findViewById(R.id.btn_launch_foot);
        LinearLayout btnLaunchThunar     = viewTools.findViewById(R.id.btn_launch_thunar);
        LinearLayout btnLaunchGalculator = viewTools.findViewById(R.id.btn_launch_galculator);
        LinearLayout btnLogCopy          = viewTools.findViewById(R.id.btn_log_copy);
        LinearLayout btnLogClear         = viewTools.findViewById(R.id.btn_log_clear);
        tvLogContent   = viewTools.findViewById(R.id.tv_log_content);
        svLogContainer = viewTools.findViewById(R.id.sv_log_container);

        applyPressScale(btnLaunchFoot);
        applyPressScale(btnLaunchThunar);
        applyPressScale(btnLaunchGalculator);

        btnLaunchFoot.setOnClickListener(v -> {
            triggerHaptic();
            CompositorRepository.getInstance().launchLinuxApp("foot");
            Toast.makeText(this, "Launching foot…", Toast.LENGTH_SHORT).show();
        });
        btnLaunchThunar.setOnClickListener(v -> {
            triggerHaptic();
            CompositorRepository.getInstance().launchLinuxApp("thunar");
            Toast.makeText(this, "Launching Thunar…", Toast.LENGTH_SHORT).show();
        });
        btnLaunchGalculator.setOnClickListener(v -> {
            triggerHaptic();
            CompositorRepository.getInstance().launchLinuxApp("galculator");
            Toast.makeText(this, "Launching galculator…", Toast.LENGTH_SHORT).show();
        });

        btnLogCopy.setOnClickListener(v -> {
            triggerHaptic();
            ClipboardManager cm = (ClipboardManager) getSystemService(Context.CLIPBOARD_SERVICE);
            if (cm != null) {
                cm.setPrimaryClip(ClipData.newPlainText("ANativeDrawer Logs", tvLogContent.getText()));
                Toast.makeText(this, "Copied to clipboard", Toast.LENGTH_SHORT).show();
            }
        });
        btnLogClear.setOnClickListener(v -> {
            triggerHaptic();
            tvLogContent.setText("[cleared]");
        });
    }

    private void refreshLogs() {
        new Thread(() -> {
            StringBuilder sb = new StringBuilder();
            try {
                Process p = Runtime.getRuntime().exec(
                        new String[]{"logcat", "-d", "-t", "60", "-s", "andwayland:V", "ANativeDrawer:V"});
                BufferedReader br = new BufferedReader(new InputStreamReader(p.getInputStream()));
                String line;
                while ((line = br.readLine()) != null) sb.append(line).append("\n");
                p.waitFor();
            } catch (Exception e) {
                sb.append("logcat error: ").append(e.getMessage());
            }
            final String logText = sb.length() > 0
                    ? sb.toString()
                    : "[ANativeDrawer] Compositor running.";
            mainHandler.post(() -> {
                if (tvLogContent != null) {
                    tvLogContent.setText(logText);
                    if (svLogContainer != null) svLogContainer.fullScroll(View.FOCUS_DOWN);
                }
            });
        }).start();
    }

    // ==========================================
    // Reactive State Rendering
    // ==========================================
    private void renderState(CompositorState state) {
        if (state == null) return;

        // ── Header status dot + label ──────────────────────────────────────
        if (state.isRunning) {
            if (viewStatusDot != null) viewStatusDot.setBackgroundResource(R.drawable.status_dot);
            tvHeaderStatus.setText("running");
            tvHeaderStatus.setTextColor(COLOR_STATUS_GREEN);
        } else {
            tvHeaderStatus.setText("stopped");
            tvHeaderStatus.setTextColor(COLOR_STATUS_RED);
            if (viewStatusDot != null) {
                // Tint the dot red by wrapping in a color-filter-compatible drawable
                viewStatusDot.setBackgroundResource(R.drawable.status_dot);
                viewStatusDot.getBackground().setTint(COLOR_STATUS_RED);
            }
        }

        // ── Overview Tab ───────────────────────────────────────────────────
        if (tvOverviewStatusBadge != null) {
            if (state.isRunning) {
                tvOverviewStatusBadge.setText("Active");
                tvOverviewStatusBadge.setTextColor(COLOR_STATUS_GREEN);
                tvPowerLabel.setText("Stop");
                tvPowerLabel.setTextColor(COLOR_STATUS_RED);
                ivPowerIcon.setColorFilter(COLOR_STATUS_RED);
            } else {
                tvOverviewStatusBadge.setText("Inactive");
                tvOverviewStatusBadge.setTextColor(COLOR_STATUS_RED);
                tvPowerLabel.setText("Start");
                tvPowerLabel.setTextColor(COLOR_STATUS_GREEN);
                ivPowerIcon.setColorFilter(COLOR_STATUS_GREEN);
            }

            // Socket: show only the last segment (wayland-0)
            String socket = state.socketPath;
            if (socket != null && socket.contains("/")) {
                socket = socket.substring(socket.lastIndexOf('/') + 1);
            }
            tvOverviewSocket.setText(socket != null ? socket : "—");
            tvOverviewPid.setText(state.pid > 0 ? "pid " + state.pid : "—");
            tvOverviewRes.setText(state.screenWidth + " × " + state.screenHeight);
            tvOverviewTabCount.setText(String.valueOf(state.activeSurfacesCount));
            tvOverviewSurfaceSubtext.setText("Active windows");

            // RAM count-up animation
            float newRam = state.currentRamMb;
            if (tvOverviewCurrentRam != null && newRam != lastRamValue) {
                animateRam(lastRamValue, newRam);
                lastRamValue = newRam;
            } else if (tvOverviewCurrentRam != null) {
                tvOverviewCurrentRam.setText(String.format("%.1f MB", newRam));
            }

            tvOverviewMinRam.setText(String.format("Min: %.1f", state.minRamMb));
            tvOverviewPeakRam.setText(String.format("Peak: %.1f", state.peakRamMb));

            if (ramGraphView != null) {
                if (!state.recentRamHistory.isEmpty()) {
                    ramGraphView.setDataPoints(state.recentRamHistory);
                } else if (state.currentRamMb > 0) {
                    ramGraphView.addDataPoint(state.currentRamMb);
                }
            }
        }

        // ── Sessions Tab ───────────────────────────────────────────────────
        if (llSessionsList != null && llSessionsEmpty != null) {
            llSessionsList.removeAllViews();
            List<CompositorState.WaylandSession> sessions = state.sessions;

            if (tvSessionsCount != null) {
                int n = sessions.size();
                tvSessionsCount.setText(n == 0 ? "No clients" : n + " client" + (n == 1 ? "" : "s"));
            }

            if (sessions.isEmpty()) {
                llSessionsEmpty.setVisibility(View.VISIBLE);
                llSessionsList.setVisibility(View.GONE);
            } else {
                llSessionsEmpty.setVisibility(View.GONE);
                llSessionsList.setVisibility(View.VISIBLE);

                LayoutInflater inflater = LayoutInflater.from(this);
                for (CompositorState.WaylandSession session : sessions) {
                    View item = inflater.inflate(R.layout.item_session, llSessionsList, false);

                    TextView tvAppId   = item.findViewById(R.id.tv_session_app_id);
                    TextView tvPid     = item.findViewById(R.id.tv_session_pid);
                    TextView tvTitle   = item.findViewById(R.id.tv_session_title);
                    TextView tvRam     = item.findViewById(R.id.tv_session_ram);
                    TextView tvRuntime = item.findViewById(R.id.tv_session_runtime);
                    TextView tvMode    = item.findViewById(R.id.tv_session_mode);
                    LinearLayout btnFocus = item.findViewById(R.id.btn_session_focus);
                    LinearLayout btnKill  = item.findViewById(R.id.btn_session_kill);

                    tvAppId.setText(session.appId);
                    tvPid.setText(String.valueOf(session.pid));
                    tvTitle.setText(session.title);
                    tvRam.setText(String.format("%.1f MB", session.ramMb));
                    tvRuntime.setText(session.getFormattedRuntime());

                    // Calm mode badge — no neon
                    if (session.isNativeTouch) {
                        tvMode.setText("touch");
                        tvMode.setTextColor(COLOR_STATUS_GREEN);
                        tvMode.setBackgroundResource(R.drawable.mode_badge_native);
                    } else {
                        tvMode.setText("gesture");
                        tvMode.setTextColor(COLOR_TEXT_SECONDARY);
                        tvMode.setBackgroundResource(R.drawable.mode_badge_gesture);
                    }

                    applyPressScale(btnFocus);
                    applyPressScale(btnKill);

                    btnFocus.setOnClickListener(v -> {
                        triggerHaptic();
                        Toast.makeText(this, "Focused: " + session.title, Toast.LENGTH_SHORT).show();
                    });
                    btnKill.setOnClickListener(v -> {
                        triggerHaptic();
                        CompositorRepository.getInstance().killClient(session.pid);
                        Toast.makeText(this, "Closed " + session.appId, Toast.LENGTH_SHORT).show();
                    });

                    llSessionsList.addView(item);
                }
            }
        }
    }

    /** Smooth count-up animation for the RAM display */
    private void animateRam(float from, float to) {
        ValueAnimator anim = ValueAnimator.ofFloat(from, to);
        anim.setDuration(400);
        anim.setInterpolator(new DecelerateInterpolator());
        anim.addUpdateListener(a -> {
            float val = (float) a.getAnimatedValue();
            if (tvOverviewCurrentRam != null) {
                tvOverviewCurrentRam.setText(String.format("%.1f MB", val));
            }
        });
        anim.start();
    }
}
