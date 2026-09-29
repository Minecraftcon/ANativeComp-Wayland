package com.andwayland.companion;

import android.os.Handler;
import android.os.Looper;

import java.util.List;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.atomic.AtomicReference;

/**
 * Clean StateFlow implementation following Kotlin Coroutines StateFlow contract:
 * - Current, renderable state with synchronous getValue()
 * - Pure, atomic concurrent updates via update(Transform<T>)
 * - Direct observer dispatch to the Android Main Looper
 */
public class StateFlow<T> {

    public interface Observer<T> {
        void onStateChanged(T state);
    }

    public interface Transform<T> {
        T apply(T current);
    }

    private final AtomicReference<T> stateRef;
    private final List<Observer<T>> observers = new CopyOnWriteArrayList<>();
    private final Handler mainHandler = new Handler(Looper.getMainLooper());

    public StateFlow(T initialValue) {
        if (initialValue == null) {
            throw new IllegalArgumentException("StateFlow initial value cannot be null");
        }
        this.stateRef = new AtomicReference<>(initialValue);
    }

    public T getValue() {
        return stateRef.get();
    }

    public void update(Transform<T> transform) {
        T prev;
        T next;
        do {
            prev = stateRef.get();
            next = transform.apply(prev);
        } while (!stateRef.compareAndSet(prev, next));

        // Dispatch state change to main thread observers
        final T stateToDispatch = next;
        mainHandler.post(() -> {
            for (Observer<T> observer : observers) {
                observer.onStateChanged(stateToDispatch);
            }
        });
    }

    public Runnable subscribe(Observer<T> observer) {
        observers.add(observer);
        // Immediately dispatch current state to new subscriber
        final T current = stateRef.get();
        mainHandler.post(() -> observer.onStateChanged(current));
        return () -> observers.remove(observer);
    }
}
