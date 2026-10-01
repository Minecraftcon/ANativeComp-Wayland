package com.andwayland.companion;

import android.content.Context;
import android.graphics.PixelFormat;
import android.net.LocalSocket;
import android.net.LocalSocketAddress;
import android.os.Handler;
import android.os.Looper;
import android.text.InputType;
import android.util.Log;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.View;
import android.view.WindowManager;
import android.view.inputmethod.BaseInputConnection;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;
import android.view.inputmethod.InputMethodManager;

import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;

import android.view.MotionEvent;
import android.view.WindowInsets;

/**
 * AndroidImeBridge connects to the native andwayland compositor via abstract UNIX
 * domain socket @andwayland_ime.
 *
 * When Wayland requests an on-screen keyboard, this bridge displays a transparent,
 * minimal overlay view that requests the Android soft keyboard (e.g. Gboard).
 * All text commits and key events are streamed directly back to the Wayland window.
 */
public class AndroidImeBridge {

    private static final String TAG = "AndroidImeBridge";
    private static final String SOCKET_NAME = "andwayland_ime";

    private final Context mContext;
    private final Handler mMainHandler = new Handler(Looper.getMainLooper());
    private final WindowManager mWindowManager;
    private final InputMethodManager mImm;

    private volatile boolean mRunning = false;
    private Thread mWorkerThread;
    private LocalSocket mSocket;
    private OutputStream mOutputStream;

    private ImeForwarderView mInputView;
    private boolean mIsImeShowing = false;

    public AndroidImeBridge(Context context) {
        mContext = context.getApplicationContext();
        mWindowManager = (WindowManager) mContext.getSystemService(Context.WINDOW_SERVICE);
        mImm = (InputMethodManager) mContext.getSystemService(Context.INPUT_METHOD_SERVICE);
    }

    public void start() {
        if (mRunning) return;
        mRunning = true;
        mWorkerThread = new Thread(this::socketLoop, "ImeBridgeSocketThread");
        mWorkerThread.start();
        Log.i(TAG, "AndroidImeBridge started");
    }

    public void stop() {
        mRunning = false;
        disconnect();
        mMainHandler.post(this::hideIme);
    }

    public void toggleIme() {
        mMainHandler.post(() -> {
            if (mIsImeShowing) {
                hideIme();
                sendToCompositor("DISMISSED\n");
            } else {
                showIme();
            }
        });
    }

    private void socketLoop() {
        while (mRunning) {
            try {
                mSocket = new LocalSocket();
                mSocket.connect(new LocalSocketAddress(SOCKET_NAME, LocalSocketAddress.Namespace.ABSTRACT));
                synchronized (this) {
                    mOutputStream = mSocket.getOutputStream();
                }
                Log.i(TAG, "Connected to compositor @ " + SOCKET_NAME);

                BufferedReader reader = new BufferedReader(
                        new InputStreamReader(mSocket.getInputStream(), StandardCharsets.UTF_8));
                String line;
                while (mRunning && (line = reader.readLine()) != null) {
                    line = line.trim();
                    if ("SHOW".equals(line)) {
                        mMainHandler.post(this::showIme);
                    } else if ("HIDE".equals(line)) {
                        mMainHandler.post(this::hideIme);
                    }
                }
            } catch (Exception e) {
                // Daemon may not be running yet; retry after delay
            } finally {
                disconnect();
            }

            if (mRunning) {
                try {
                    Thread.sleep(1500);
                } catch (InterruptedException ignored) {}
            }
        }
    }

    private synchronized void disconnect() {
        if (mOutputStream != null) {
            try { mOutputStream.close(); } catch (Exception ignored) {}
            mOutputStream = null;
        }
        if (mSocket != null) {
            try { mSocket.close(); } catch (Exception ignored) {}
            mSocket = null;
        }
    }

    public synchronized void sendToCompositor(String msg) {
        if (mOutputStream != null) {
            try {
                mOutputStream.write(msg.getBytes(StandardCharsets.UTF_8));
                mOutputStream.flush();
            } catch (Exception e) {
                Log.e(TAG, "Error sending to compositor: " + e.getMessage());
            }
        }
    }

    private void showIme() {
        try {
            mIsImeShowing = true;
            if (mInputView == null) {
                mInputView = new ImeForwarderView(mContext);
            }
            if (mInputView.getParent() == null) {
                WindowManager.LayoutParams params = new WindowManager.LayoutParams(
                        1, 1,
                        WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY,
                        WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL |
                        WindowManager.LayoutParams.FLAG_WATCH_OUTSIDE_TOUCH,
                        PixelFormat.TRANSLUCENT
                );
                params.gravity = Gravity.TOP | Gravity.START;
                params.x = 0;
                params.y = 0;
                params.softInputMode = WindowManager.LayoutParams.SOFT_INPUT_STATE_ALWAYS_VISIBLE |
                                       WindowManager.LayoutParams.SOFT_INPUT_ADJUST_NOTHING;
                mWindowManager.addView(mInputView, params);
            } else {
                mInputView.requestFocus();
                if (mInputView.hasWindowFocus() && mImm != null) {
                    boolean ok = mImm.showSoftInput(mInputView, InputMethodManager.SHOW_FORCED);
                    Log.i(TAG, "showSoftInput direct result=" + ok);
                }
            }
            Log.i(TAG, "showIme initiated");
        } catch (Exception e) {
            Log.e(TAG, "Failed to show soft keyboard: " + e.getMessage());
        }
    }

    private void hideIme() {
        mIsImeShowing = false;
        try {
            if (mInputView != null) {
                if (mImm != null) {
                    mImm.hideSoftInputFromWindow(mInputView.getWindowToken(), 0);
                }
                if (mInputView.getParent() != null) {
                    mWindowManager.removeView(mInputView);
                }
                mInputView = null;
            }
            Log.i(TAG, "Soft keyboard closed");
        } catch (Exception e) {
            Log.e(TAG, "Failed to hide soft keyboard: " + e.getMessage());
        }
    }

    private static int androidToLinuxKeycode(int keyCode) {
        switch (keyCode) {
            case KeyEvent.KEYCODE_ENTER:       return 28;  // KEY_ENTER
            case KeyEvent.KEYCODE_DEL:         return 14;  // KEY_BACKSPACE
            case KeyEvent.KEYCODE_TAB:         return 15;  // KEY_TAB
            case KeyEvent.KEYCODE_SPACE:       return 57;  // KEY_SPACE
            case KeyEvent.KEYCODE_ESCAPE:      return 1;   // KEY_ESC
            case KeyEvent.KEYCODE_DPAD_UP:     return 103; // KEY_UP
            case KeyEvent.KEYCODE_DPAD_DOWN:   return 108; // KEY_DOWN
            case KeyEvent.KEYCODE_DPAD_LEFT:   return 105; // KEY_LEFT
            case KeyEvent.KEYCODE_DPAD_RIGHT:  return 106; // KEY_RIGHT
            case KeyEvent.KEYCODE_FORWARD_DEL: return 111; // KEY_DELETE
            case KeyEvent.KEYCODE_MOVE_HOME:   return 102; // KEY_HOME
            case KeyEvent.KEYCODE_MOVE_END:    return 107; // KEY_END
            default: return 0;
        }
    }

    private class ImeForwarderView extends View {

        public ImeForwarderView(Context context) {
            super(context);
            setFocusable(true);
            setFocusableInTouchMode(true);
        }

        @Override
        public boolean onCheckIsTextEditor() {
            return true;
        }

        @Override
        protected void onAttachedToWindow() {
            super.onAttachedToWindow();
            requestFocus();
        }

        @Override
        public void onWindowFocusChanged(boolean hasWindowFocus) {
            super.onWindowFocusChanged(hasWindowFocus);
            Log.d(TAG, "onWindowFocusChanged: " + hasWindowFocus + ", mIsImeShowing=" + mIsImeShowing);
            if (hasWindowFocus && mIsImeShowing) {
                requestFocus();
                post(() -> {
                    if (mImm != null && mIsImeShowing) {
                        boolean ok = mImm.showSoftInput(this, InputMethodManager.SHOW_FORCED);
                        Log.i(TAG, "showSoftInput in onWindowFocusChanged result=" + ok);
                    }
                });
            }
        }

        @Override
        public InputConnection onCreateInputConnection(EditorInfo outAttrs) {
            outAttrs.inputType = InputType.TYPE_CLASS_TEXT |
                                 InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD |
                                 InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS;
            outAttrs.imeOptions = EditorInfo.IME_ACTION_NONE | EditorInfo.IME_FLAG_NO_FULLSCREEN;
            outAttrs.initialSelStart = 0;
            outAttrs.initialSelEnd = 0;
            return new BaseInputConnection(this, true) {
                @Override
                public boolean commitText(CharSequence text, int newCursorPosition) {
                    Log.i(TAG, "commitText: '" + text + "'");
                    if (text != null && text.length() > 0) {
                        sendToCompositor("COMMIT " + text.toString() + "\n");
                    }
                    return super.commitText(text, newCursorPosition);
                }

                @Override
                public boolean setComposingText(CharSequence text, int newCursorPosition) {
                    Log.i(TAG, "setComposingText: '" + text + "'");
                    return super.setComposingText(text, newCursorPosition);
                }

                @Override
                public boolean deleteSurroundingText(int beforeLength, int afterLength) {
                    Log.i(TAG, "deleteSurroundingText: " + beforeLength);
                    for (int i = 0; i < beforeLength; i++) {
                        sendToCompositor("KEY 14 1\n");
                        sendToCompositor("KEY 14 0\n");
                    }
                    return super.deleteSurroundingText(beforeLength, afterLength);
                }

                @Override
                public boolean deleteSurroundingTextInCodePoints(int beforeLength, int afterLength) {
                    return deleteSurroundingText(beforeLength, afterLength);
                }

                @Override
                public boolean performEditorAction(int actionCode) {
                    Log.i(TAG, "performEditorAction: " + actionCode);
                    sendToCompositor("KEY 28 1\n");
                    sendToCompositor("KEY 28 0\n");
                    return true;
                }

                @Override
                public boolean sendKeyEvent(KeyEvent event) {
                    Log.i(TAG, "sendKeyEvent: keycode=" + event.getKeyCode() + " action=" + event.getAction());
                    int linuxKey = androidToLinuxKeycode(event.getKeyCode());
                    int state = (event.getAction() == KeyEvent.ACTION_DOWN) ? 1 : 0;
                    if (linuxKey > 0) {
                        sendToCompositor("KEY " + linuxKey + " " + state + "\n");
                    }
                    return super.sendKeyEvent(event);
                }
            };
        }

        @Override
        public boolean onKeyPreIme(int keyCode, KeyEvent event) {
            if (keyCode == KeyEvent.KEYCODE_BACK && event.getAction() == KeyEvent.ACTION_UP) {
                Log.i(TAG, "Back pressed, dismissing IME");
                hideIme();
                sendToCompositor("DISMISSED\n");
                return true;
            }
            return super.onKeyPreIme(keyCode, event);
        }
    }
}
