package com.andwayland.companion;

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
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.Switch;
import android.widget.TextView;
import android.widget.Toast;

import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.util.List;

/**
 * Main Dashboard Activity for ANativeDrawer Wayland Compositor.
 * Implements modern Android Design, Hooked UX engagement loops,
 * and reactive StateFlow observation.
 */
public class ANativeDrawerActivity extends Activity {

    private enum Tab {
        OVERVIEW, SESSIONS, PREFERENCES, TOOLS
    }

    private Tab currentTab = Tab.OVERVIEW;

    private TextView tvHeaderStatus;
    private Button btnTabOverview;
    private Button btnTabSessions;
    private Button btnTabPreferences;
    private Button btnTabTools;
    private FrameLayout tabContainer;

    // Cached tab views
    private View viewOverview;
    private View viewSessions;
    private View viewPreferences;
    private View viewTools;

    private Runnable unsubscribeStateFlow;
    private PreferencesManager prefsManager;
    private final Handler mainHandler = new Handler(Looper.getMainLooper());

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);

        prefsManager = new PreferencesManager(this);

        // Ensure resident monitor service is running
        Intent serviceIntent = new Intent(this, CompositorMonitorService.class);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            startForegroundService(serviceIntent);
        } else {
            startService(serviceIntent);
        }

        initViews();
        setupTabs();
        selectTab(Tab.OVERVIEW);

        // Subscribe to reactive StateFlow
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
        tvHeaderStatus = findViewById(R.id.tv_header_status);
        btnTabOverview = findViewById(R.id.btn_tab_overview);
        btnTabSessions = findViewById(R.id.btn_tab_sessions);
        btnTabPreferences = findViewById(R.id.btn_tab_preferences);
        btnTabTools = findViewById(R.id.btn_tab_tools);
        tabContainer = findViewById(R.id.tab_container);

        LayoutInflater inflater = LayoutInflater.from(this);
        viewOverview = inflater.inflate(R.layout.tab_overview, tabContainer, false);
        viewSessions = inflater.inflate(R.layout.tab_sessions, tabContainer, false);
        viewPreferences = inflater.inflate(R.layout.tab_preferences, tabContainer, false);
        viewTools = inflater.inflate(R.layout.tab_tools, tabContainer, false);

        initOverviewTab();
        initSessionsTab();
        initPreferencesTab();
        initToolsTab();
    }

    private void setupTabs() {
        btnTabOverview.setOnClickListener(v -> selectTab(Tab.OVERVIEW));
        btnTabSessions.setOnClickListener(v -> selectTab(Tab.SESSIONS));
        btnTabPreferences.setOnClickListener(v -> selectTab(Tab.PREFERENCES));
        btnTabTools.setOnClickListener(v -> selectTab(Tab.TOOLS));
    }

    private void selectTab(Tab tab) {
        currentTab = tab;
        tabContainer.removeAllViews();

        resetTabButton(btnTabOverview);
        resetTabButton(btnTabSessions);
        resetTabButton(btnTabPreferences);
        resetTabButton(btnTabTools);

        FrameLayout.LayoutParams matchParams = new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT);

        switch (tab) {
            case OVERVIEW:
                tabContainer.addView(viewOverview, matchParams);
                setActiveTabButton(btnTabOverview);
                break;
            case SESSIONS:
                tabContainer.addView(viewSessions, matchParams);
                setActiveTabButton(btnTabSessions);
                break;
            case PREFERENCES:
                tabContainer.addView(viewPreferences, matchParams);
                setActiveTabButton(btnTabPreferences);
                break;
            case TOOLS:
                tabContainer.addView(viewTools, matchParams);
                setActiveTabButton(btnTabTools);
                refreshLogs();
                break;
        }

        // Re-render current state for the newly selected tab
        renderState(CompositorRepository.getInstance().getStateFlow().getValue());
    }

    private void setActiveTabButton(Button btn) {
        btn.setBackgroundResource(R.drawable.tab_pill_active);
        btn.setTextColor(Color.parseColor("#EAEAEA"));
    }

    private void resetTabButton(Button btn) {
        btn.setBackgroundResource(R.drawable.tab_pill_inactive);
        btn.setTextColor(Color.parseColor("#8E8E93"));
    }

    private void triggerHaptic() {
        try {
            Vibrator v = (Vibrator) getSystemService(Context.VIBRATOR_SERVICE);
            if (v != null && v.hasVibrator()) {
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                    v.vibrate(VibrationEffect.createOneShot(25, VibrationEffect.DEFAULT_AMPLITUDE));
                } else {
                    v.vibrate(25);
                }
            }
        } catch (Exception ignored) {
        }
    }

    // ==========================================
    // 1. Overview Tab
    // ==========================================
    private TextView tvOverviewStatusBadge;
    private TextView tvOverviewSocket;
    private TextView tvOverviewPid;
    private TextView tvOverviewRes;
    private Button btnOverviewRestart;
    private Button btnOverviewTogglePower;
    private TextView tvOverviewTabCount;
    private TextView tvOverviewSurfaceSubtext;
    private TextView tvOverviewCurrentRam;
    private TextView tvOverviewMinRam;
    private TextView tvOverviewPeakRam;
    private RamGraphView ramGraphView;

    private void initOverviewTab() {
        tvOverviewStatusBadge = viewOverview.findViewById(R.id.tv_overview_status_badge);
        tvOverviewSocket = viewOverview.findViewById(R.id.tv_overview_socket);
        tvOverviewPid = viewOverview.findViewById(R.id.tv_overview_pid);
        tvOverviewRes = viewOverview.findViewById(R.id.tv_overview_res);
        btnOverviewRestart = viewOverview.findViewById(R.id.btn_overview_restart);
        btnOverviewTogglePower = viewOverview.findViewById(R.id.btn_overview_toggle_power);
        tvOverviewTabCount = viewOverview.findViewById(R.id.tv_overview_tab_count);
        tvOverviewSurfaceSubtext = viewOverview.findViewById(R.id.tv_overview_surface_subtext);
        tvOverviewCurrentRam = viewOverview.findViewById(R.id.tv_overview_current_ram);
        tvOverviewMinRam = viewOverview.findViewById(R.id.tv_ram_min);
        tvOverviewPeakRam = viewOverview.findViewById(R.id.tv_ram_peak);
        ramGraphView = viewOverview.findViewById(R.id.graph_ram_sparkline);

        btnOverviewRestart.setOnClickListener(v -> {
            triggerHaptic();
            CompositorRepository.getInstance().restartDaemon();
            Toast.makeText(this, "Restarting Wayland Compositor...", Toast.LENGTH_SHORT).show();
        });

        btnOverviewTogglePower.setOnClickListener(v -> {
            triggerHaptic();
            CompositorState state = CompositorRepository.getInstance().getStateFlow().getValue();
            if (state.isRunning) {
                CompositorRepository.getInstance().stopDaemon();
                Toast.makeText(this, "Stopping Wayland Compositor...", Toast.LENGTH_SHORT).show();
            } else {
                CompositorRepository.getInstance().restartDaemon();
                Toast.makeText(this, "Starting Wayland Compositor...", Toast.LENGTH_SHORT).show();
            }
        });
    }

    // ==========================================
    // 2. Sessions Tab
    // ==========================================
    private LinearLayout llSessionsList;
    private LinearLayout llSessionsEmpty;
    private Button btnSessionsRefresh;

    private void initSessionsTab() {
        llSessionsList = viewSessions.findViewById(R.id.ll_sessions_list);
        llSessionsEmpty = viewSessions.findViewById(R.id.ll_sessions_empty);
        btnSessionsRefresh = viewSessions.findViewById(R.id.btn_sessions_refresh);

        btnSessionsRefresh.setOnClickListener(v -> {
            triggerHaptic();
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
    private Button btnPrefSave;

    private void initPreferencesTab() {
        tvPrefHoldVal = viewPreferences.findViewById(R.id.tv_pref_hold_val);
        sbPrefHold = viewPreferences.findViewById(R.id.sb_pref_hold);
        tvPrefScrollVal = viewPreferences.findViewById(R.id.tv_pref_scroll_val);
        sbPrefScroll = viewPreferences.findViewById(R.id.sb_pref_scroll);
        tvPrefDoubleTapVal = viewPreferences.findViewById(R.id.tv_pref_doubletap_val);
        sbPrefDoubleTap = viewPreferences.findViewById(R.id.sb_pref_doubletap);
        swPrefInvertScroll = viewPreferences.findViewById(R.id.sw_pref_invert_scroll);
        swPrefSsd = viewPreferences.findViewById(R.id.sw_pref_ssd);
        swPrefNotification = viewPreferences.findViewById(R.id.sw_pref_notification);
        btnPrefSave = viewPreferences.findViewById(R.id.btn_pref_save);

        // Bind initial values from PreferencesManager
        // Hold threshold: range 200..1000 ms (offset 200, max 800)
        int holdOffset = Math.max(0, Math.min(800, prefsManager.holdThresholdMs - 200));
        sbPrefHold.setProgress(holdOffset);
        tvPrefHoldVal.setText(prefsManager.holdThresholdMs + " ms");
        sbPrefHold.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override
            public void onProgressChanged(SeekBar seekBar, int progress, boolean fromUser) {
                int val = 200 + progress;
                tvPrefHoldVal.setText(val + " ms");
                prefsManager.holdThresholdMs = val;
            }
            @Override public void onStartTrackingTouch(SeekBar seekBar) {}
            @Override public void onStopTrackingTouch(SeekBar seekBar) { triggerHaptic(); }
        });

        // Scroll sensitivity: range 50%..300% (offset 50, max 250)
        int scrollOffset = Math.max(0, Math.min(250, (int) (prefsManager.scrollSensitivity * 100) - 50));
        sbPrefScroll.setProgress(scrollOffset);
        tvPrefScrollVal.setText((int) (prefsManager.scrollSensitivity * 100) + " %");
        sbPrefScroll.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override
            public void onProgressChanged(SeekBar seekBar, int progress, boolean fromUser) {
                int pct = 50 + progress;
                tvPrefScrollVal.setText(pct + " %");
                prefsManager.scrollSensitivity = pct / 100.0f;
            }
            @Override public void onStartTrackingTouch(SeekBar seekBar) {}
            @Override public void onStopTrackingTouch(SeekBar seekBar) { triggerHaptic(); }
        });

        // Double-tap window: range 150..500 ms (offset 150, max 350)
        int doubleTapOffset = Math.max(0, Math.min(350, prefsManager.doubleTapMs - 150));
        sbPrefDoubleTap.setProgress(doubleTapOffset);
        tvPrefDoubleTapVal.setText(prefsManager.doubleTapMs + " ms");
        sbPrefDoubleTap.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override
            public void onProgressChanged(SeekBar seekBar, int progress, boolean fromUser) {
                int val = 150 + progress;
                tvPrefDoubleTapVal.setText(val + " ms");
                prefsManager.doubleTapMs = val;
            }
            @Override public void onStartTrackingTouch(SeekBar seekBar) {}
            @Override public void onStopTrackingTouch(SeekBar seekBar) { triggerHaptic(); }
        });

        swPrefInvertScroll.setChecked(prefsManager.invertScroll);
        swPrefInvertScroll.setOnCheckedChangeListener((b, isChecked) -> {
            triggerHaptic();
            prefsManager.invertScroll = isChecked;
        });

        swPrefSsd.setChecked(prefsManager.forceSsd);
        swPrefSsd.setOnCheckedChangeListener((b, isChecked) -> {
            triggerHaptic();
            prefsManager.forceSsd = isChecked;
        });

        swPrefNotification.setChecked(prefsManager.persistentNotification);
        swPrefNotification.setOnCheckedChangeListener((b, isChecked) -> {
            triggerHaptic();
            prefsManager.persistentNotification = isChecked;
        });

        btnPrefSave.setOnClickListener(v -> {
            triggerHaptic();
            prefsManager.save();
            Toast.makeText(this, "Preferences Saved & Applied to Compositor", Toast.LENGTH_SHORT).show();
        });
    }

    // ==========================================
    // 4. Tools & Logs Tab
    // ==========================================
    private TextView tvLogContent;
    private ScrollView svLogContainer;

    private void initToolsTab() {
        Button btnLaunchFoot = viewTools.findViewById(R.id.btn_launch_foot);
        Button btnLaunchThunar = viewTools.findViewById(R.id.btn_launch_thunar);
        Button btnLaunchGalculator = viewTools.findViewById(R.id.btn_launch_galculator);
        Button btnLogCopy = viewTools.findViewById(R.id.btn_log_copy);
        Button btnLogClear = viewTools.findViewById(R.id.btn_log_clear);
        tvLogContent = viewTools.findViewById(R.id.tv_log_content);
        svLogContainer = viewTools.findViewById(R.id.sv_log_container);

        btnLaunchFoot.setOnClickListener(v -> {
            triggerHaptic();
            CompositorRepository.getInstance().launchLinuxApp("foot");
            Toast.makeText(this, "Launching foot...", Toast.LENGTH_SHORT).show();
        });

        btnLaunchThunar.setOnClickListener(v -> {
            triggerHaptic();
            CompositorRepository.getInstance().launchLinuxApp("thunar");
            Toast.makeText(this, "Launching Thunar...", Toast.LENGTH_SHORT).show();
        });

        btnLaunchGalculator.setOnClickListener(v -> {
            triggerHaptic();
            CompositorRepository.getInstance().launchLinuxApp("galculator");
            Toast.makeText(this, "Launching galculator...", Toast.LENGTH_SHORT).show();
        });

        btnLogCopy.setOnClickListener(v -> {
            triggerHaptic();
            ClipboardManager cm = (ClipboardManager) getSystemService(Context.CLIPBOARD_SERVICE);
            if (cm != null) {
                ClipData clip = ClipData.newPlainText("ANativeDrawer Logs", tvLogContent.getText());
                cm.setPrimaryClip(clip);
                Toast.makeText(this, "Logs copied to clipboard", Toast.LENGTH_SHORT).show();
            }
        });

        btnLogClear.setOnClickListener(v -> {
            triggerHaptic();
            tvLogContent.setText("[Logs cleared]");
        });
    }

    private void refreshLogs() {
        new Thread(() -> {
            StringBuilder sb = new StringBuilder();
            try {
                Process p = Runtime.getRuntime().exec(new String[]{"logcat", "-d", "-t", "60", "-s", "andwayland:V", "ANativeDrawer:V"});
                BufferedReader br = new BufferedReader(new InputStreamReader(p.getInputStream()));
                String line;
                while ((line = br.readLine()) != null) {
                    sb.append(line).append("\n");
                }
                p.waitFor();
            } catch (Exception e) {
                sb.append("Failed to read logcat: ").append(e.getMessage());
            }

            final String logText = sb.length() > 0 ? sb.toString() : "[ANativeDrawer] Compositor daemon running actively.";
            mainHandler.post(() -> {
                if (tvLogContent != null) {
                    tvLogContent.setText(logText);
                    if (svLogContainer != null) {
                        svLogContainer.fullScroll(View.FOCUS_DOWN);
                    }
                }
            });
        }).start();
    }

    // ==========================================
    // Reactive State Rendering
    // ==========================================
    private void renderState(CompositorState state) {
        if (state == null) return;

        // Header Status Text
        if (state.isRunning) {
            tvHeaderStatus.setText("● Running");
            tvHeaderStatus.setTextColor(Color.parseColor("#34C759"));
            tvHeaderStatus.setBackground(null);
        } else {
            tvHeaderStatus.setText("● Stopped");
            tvHeaderStatus.setTextColor(Color.parseColor("#FF453A"));
            tvHeaderStatus.setBackground(null);
        }

        // Overview Tab Updates
        if (tvOverviewStatusBadge != null) {
            if (state.isRunning) {
                tvOverviewStatusBadge.setText("Active");
                tvOverviewStatusBadge.setTextColor(Color.parseColor("#34C759"));
                tvOverviewStatusBadge.setBackground(null);
                btnOverviewTogglePower.setText("Stop");
            } else {
                tvOverviewStatusBadge.setText("Inactive");
                tvOverviewStatusBadge.setTextColor(Color.parseColor("#FF453A"));
                tvOverviewStatusBadge.setBackground(null);
                btnOverviewTogglePower.setText("Start");
            }

            tvOverviewSocket.setText(state.socketPath);
            tvOverviewPid.setText(state.pid > 0 ? String.valueOf(state.pid) : "N/A");
            tvOverviewRes.setText(state.screenWidth + " x " + state.screenHeight + " (Android 12)");

            tvOverviewTabCount.setText(String.valueOf(state.activeSurfacesCount));
            tvOverviewSurfaceSubtext.setText("Wayland surfaces mapped to SurfaceFlinger: " + state.activeSurfacesCount);

            tvOverviewCurrentRam.setText(String.format("%.1f MB", state.currentRamMb));
            tvOverviewMinRam.setText(String.format("Min: %.1f MB", state.minRamMb));
            tvOverviewPeakRam.setText(String.format("Peak: %.1f MB", state.peakRamMb));

            if (ramGraphView != null) {
                if (!state.recentRamHistory.isEmpty()) {
                    ramGraphView.setDataPoints(state.recentRamHistory);
                } else if (state.currentRamMb > 0) {
                    ramGraphView.addDataPoint(state.currentRamMb);
                }
            }
        }

        // Sessions Tab Updates
        if (llSessionsList != null && llSessionsEmpty != null) {
            llSessionsList.removeAllViews();
            List<CompositorState.WaylandSession> sessions = state.sessions;
            if (sessions.isEmpty()) {
                llSessionsEmpty.setVisibility(View.VISIBLE);
                llSessionsList.setVisibility(View.GONE);
            } else {
                llSessionsEmpty.setVisibility(View.GONE);
                llSessionsList.setVisibility(View.VISIBLE);

                LayoutInflater inflater = LayoutInflater.from(this);
                for (CompositorState.WaylandSession session : sessions) {
                    View item = inflater.inflate(R.layout.item_session, llSessionsList, false);

                    TextView tvAppId = item.findViewById(R.id.tv_session_app_id);
                    TextView tvPid = item.findViewById(R.id.tv_session_pid);
                    TextView tvTitle = item.findViewById(R.id.tv_session_title);
                    TextView tvRam = item.findViewById(R.id.tv_session_ram);
                    TextView tvRuntime = item.findViewById(R.id.tv_session_runtime);
                    TextView tvMode = item.findViewById(R.id.tv_session_mode);
                    Button btnFocus = item.findViewById(R.id.btn_session_focus);
                    Button btnKill = item.findViewById(R.id.btn_session_kill);

                    tvAppId.setText(session.appId);
                    tvPid.setText("PID: " + session.pid);
                    tvTitle.setText(session.title);
                    tvRam.setText(String.format("%.1f MB", session.ramMb));
                    tvRuntime.setText(session.getFormattedRuntime());

                    if (session.isNativeTouch) {
                        tvMode.setText("Native Touch");
                        tvMode.setTextColor(Color.parseColor("#00E676"));
                    } else {
                        tvMode.setText("Gesture Engine");
                        tvMode.setTextColor(Color.parseColor("#4FACFE"));
                    }

                    btnFocus.setOnClickListener(v -> {
                        triggerHaptic();
                        Toast.makeText(this, "Focused: " + session.title, Toast.LENGTH_SHORT).show();
                    });

                    btnKill.setOnClickListener(v -> {
                        triggerHaptic();
                        CompositorRepository.getInstance().killClient(session.pid);
                        Toast.makeText(this, "Terminated " + session.appId, Toast.LENGTH_SHORT).show();
                    });

                    llSessionsList.addView(item);
                }
            }
        }
    }
}
