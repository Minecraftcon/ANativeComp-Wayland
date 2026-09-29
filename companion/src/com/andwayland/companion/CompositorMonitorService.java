package com.andwayland.companion;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.os.Build;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.IBinder;
import android.util.Log;

/**
 * Foreground resident service displaying the always-on status notification
 * with quick control actions and maintaining active compositor telemetry.
 */
public class CompositorMonitorService extends Service {

    private static final String TAG = "CompositorMonitor";
    private static final String CHANNEL_ID = "wayland_status_channel";
    private static final int NOTIFICATION_ID = 1001;

    public static final String ACTION_RESTART = "com.andwayland.companion.ACTION_RESTART";
    public static final String ACTION_STOP = "com.andwayland.companion.ACTION_STOP";

    private HandlerThread workerThread;
    private Handler workerHandler;
    private final Runnable pollRunnable = new Runnable() {
        @Override
        public void run() {
            CompositorRepository.getInstance().pollSync();
            updateNotification();
            if (workerHandler != null) {
                workerHandler.postDelayed(this, 1500);
            }
        }
    };

    private final BroadcastReceiver actionReceiver = new BroadcastReceiver() {
        @Override
        public void onReceive(Context context, Intent intent) {
            String action = intent.getAction();
            if (ACTION_RESTART.equals(action)) {
                CompositorRepository.getInstance().restartDaemon();
            } else if (ACTION_STOP.equals(action)) {
                CompositorRepository.getInstance().stopDaemon();
            }
        }
    };

    @Override
    public void onCreate() {
        super.onCreate();
        createNotificationChannel();

        IntentFilter filter = new IntentFilter();
        filter.addAction(ACTION_RESTART);
        filter.addAction(ACTION_STOP);
        registerReceiver(actionReceiver, filter);

        workerThread = new HandlerThread("CompositorPoller");
        workerThread.start();
        workerHandler = new Handler(workerThread.getLooper());
        workerHandler.post(pollRunnable);

        startForeground(NOTIFICATION_ID, buildNotification());
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        startForeground(NOTIFICATION_ID, buildNotification());
        return START_STICKY;
    }

    private void createNotificationChannel() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            NotificationChannel channel = new NotificationChannel(
                    CHANNEL_ID,
                    "Wayland Compositor Status",
                    NotificationManager.IMPORTANCE_LOW
            );
            channel.setDescription("Resident status and controls for ANativeDrawer Wayland Compositor");
            channel.setShowBadge(false);
            NotificationManager nm = getSystemService(NotificationManager.class);
            if (nm != null) {
                nm.createNotificationChannel(channel);
            }
        }
    }

    private Notification buildNotification() {
        CompositorState state = CompositorRepository.getInstance().getStateFlow().getValue();
        boolean running = state.isRunning;

        Intent contentIntent = new Intent(this, ANativeDrawerActivity.class);
        contentIntent.setFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP | Intent.FLAG_ACTIVITY_CLEAR_TOP);
        PendingIntent pendingIntent = PendingIntent.getActivity(
                this, 0, contentIntent, PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE
        );

        Intent restartIntent = new Intent(ACTION_RESTART);
        PendingIntent restartPending = PendingIntent.getBroadcast(
                this, 1, restartIntent, PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE
        );

        Intent stopIntent = new Intent(ACTION_STOP);
        PendingIntent stopPending = PendingIntent.getBroadcast(
                this, 2, stopIntent, PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE
        );

        String title = "ANativeDrawer Wayland";
        String contentText = running
                ? "Wayland is running (Socket: wayland-0)"
                : "Compositor Stopped";
        String subText = "tap to open configuration";

        Notification.Builder builder;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            builder = new Notification.Builder(this, CHANNEL_ID);
        } else {
            builder = new Notification.Builder(this);
        }

        builder.setSmallIcon(R.drawable.ic_notification)
                .setContentTitle(title)
                .setContentText(contentText)
                .setSubText(subText)
                .setContentIntent(pendingIntent)
                .setOngoing(true)
                .setShowWhen(false)
                .addAction(new Notification.Action.Builder(
                        null, "Configure", pendingIntent).build())
                .addAction(new Notification.Action.Builder(
                        null, "Restart", restartPending).build())
                .addAction(new Notification.Action.Builder(
                        null, "Stop", stopPending).build());

        return builder.build();
    }

    private void updateNotification() {
        try {
            NotificationManager nm = (NotificationManager) getSystemService(Context.NOTIFICATION_SERVICE);
            if (nm != null) {
                nm.notify(NOTIFICATION_ID, buildNotification());
            }
        } catch (Exception e) {
            Log.e(TAG, "Failed to update notification", e);
        }
    }

    @Override
    public void onDestroy() {
        super.onDestroy();
        try {
            unregisterReceiver(actionReceiver);
        } catch (Exception ignored) {
        }
        if (workerHandler != null) {
            workerHandler.removeCallbacks(pollRunnable);
        }
        if (workerThread != null) {
            workerThread.quitSafely();
        }
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }
}
