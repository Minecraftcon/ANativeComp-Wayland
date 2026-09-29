# ANativeDrawer — Handover Notes

Session continuation from an Antigravity/Gemini transcript that died on API-overload
errors. Original log: `~/.gemini/antigravity-ide/brain/09f5c3a5-1499-48fa-967b-2424424c36c6/.system_generated/logs/`.

Read this before changing compositor or client code. Several bugs below were
**introduced and then fixed within this same session** — the notes explain why the
current design looks the way it does, so please don't "simplify" it back.

---

## Project

Native C++17 Wayland compositor for rooted Android. Each `wl_surface` becomes one
SurfaceFlinger `SurfaceControl` layer. No Java, no Android Framework. Binary: `andwayland`.

```
clients (wl_shm, CPU) → WaylandServer → SurfaceBridge → SurfaceFlingerBridge → /system/lib64 libgui.so
                                      ↓ evdev hit-test
                                 SeatManager (/dev/input/event*)
```

| Component | File | Role |
|---|---|---|
| Entry | `compositor/main.cpp` | `--socket`, `--test`; init order → input thread → event loop |
| Server | `compositor/WaylandServer.cpp` | `wl_display_create/add_socket`, globals, event loop |
| Surfaces | `compositor/SurfaceBridge.cpp` | wl_surface ↔ SF layer, shm blit, damage merge |
| Input | `compositor/SeatManager.cpp` | evdev → wl_touch/wl_pointer/wl_keyboard, hit-testing |
| SF backend | `platform/SurfaceFlingerBridge.cpp` | **only** file including `<gui/...>`; lock/unlockAndPost |
| Test client | `tools/interactive_window.c` | binds seat, renders window, counts taps |

Protocols in `protocols/`: `xdg_shell`, `viewporter`, `linux_dmabuf`, `layer_shell`
(codegen via `wayland-scanner`, `CMakeLists.txt:69`).

---

## Build & deploy

NDK is **not** in a standard location — it lives at `/home/shado/envs/android-ndk-r29`.

```bash
cd /home/shado/Documents/Android-Root/ANativeDrawer
NDK=/home/shado/envs/android-ndk-r29

# compositor
NDK=$NDK cmake --build build --parallel $(nproc)

# test client (NOT in CMakeLists.txt — compile it by hand)
$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android33-clang \
  -o build/interactive_window tools/interactive_window.c \
  -I third_party/wayland/include -L third_party/wayland/lib -lwayland-client
```

Deploy and run:

```bash
adb push build/andwayland /data/local/tmp/andwayland
adb push build/interactive_window /data/local/tmp/interactive_window

# NOTE: pkill -f andwayland does NOT match. Kill by PID.
for p in $(adb shell "ps -A | grep -E 'andwayland|interactive_window'" | awk '{print $2}'); do
  adb shell "su -c 'kill -9 $p'"
done

adb shell su -c "mkdir -p /data/local/tmp/wayland; chmod 777 /data/local/tmp/wayland"
adb shell su -c "XDG_RUNTIME_DIR=/data/local/tmp/wayland LD_LIBRARY_PATH=/data/local/tmp \
  nohup /data/local/tmp/andwayland --socket wayland-0 >/data/local/tmp/aw.log 2>&1 &"

adb shell su -c "XDG_RUNTIME_DIR=/data/local/tmp/wayland WAYLAND_DISPLAY=wayland-0 \
  LD_LIBRARY_PATH=/data/local/tmp nohup /data/local/tmp/interactive_window --fullscreen 300 \
  >/data/local/tmp/iw.log 2>&1 &"
```

Verify / debug:

```bash
adb shell su -c "dumpsys SurfaceFlinger --list | grep -i wl_surface"   # layers
adb shell su -c "logcat -d | grep andwayland"                          # compositor log
adb exec-out screencap -p > /tmp/s.png                                 # screenshot
```

### Gotchas that will waste your time

- **`input tap` does not work for testing.** It injects via InputManager and never
  reaches evdev, so the compositor sees nothing. Use `sendevent` on the raw node:
  ```bash
  adb shell su -c "sendevent /dev/input/event2 3 57 0; \
    sendevent /dev/input/event2 3 53 300; sendevent /dev/input/event2 3 54 400; \
    sendevent /dev/input/event2 3 58 900; sendevent /dev/input/event2 0 0 0; \
    sendevent /dev/input/event2 1 330 1; sendevent /dev/input/event2 0 0 0"
  ```
  Codes: `3`=EV_ABS, `57`=ABS_MT_SLOT, `53`=ABS_MT_TRACKING_ID,
  `54`=ABS_MT_POSITION_X, `58`=ABS_MT_POSITION_Y, `1`=EV_KEY, `330`=BTN_TOUCH, `0`=EV_SYN.
  Touchscreen is `/dev/input/event2` (`touchpanel`, X=[0..719] Y=[0..1599] on this device).
- **`su -c` quoting**: use `adb shell su -c "cmd with \$VAR"` — the outer double quotes
  let the device shell expand. `su -c` with separate args fails.
- **`/tmp` does not exist** on device; use `/data/local/tmp`.
- **`wl_display_flush_clients` returns `void`** in wayland 1.23 (not `int`).
- **Don't put `struct wl_event_source*` inside `namespace andwayland`** — it creates
  `andwayland::wl_event_source`, a *different type* from the global one, and fails to
  link against libwayland. Forward-declare outside the namespace.

---

## Changes made this session

### 1. Orphaned surfaces on client disconnect (real bug, fixed)

`surface_resource_destructor` (`compositor/SurfaceBridge.cpp`) was an empty stub, so
when a client disconnected, its `WaylandSurface` + SF layer survived in `mSurfaces`.
Hit-testing kept routing touches at those dead surfaces and libwayland dropped the
events. Symptoms: stale window frozen on screen, input going nowhere.

Fix: the destructor now calls `bridge->destroySurface(surface)`. `destroySurface`
removes the map entry first, so an explicit `wl_surface.destroy` still works — the
destructor is a no-op in that case (idempotent).

**Verified:** killing the client now leaves zero `wl_surface` layers in
`dumpsys SurfaceFlinger --list`.

### 2. `wl_buffer` double-free (`SurfaceBridge.cpp`)

`buffer_destroy` deleted the `ShmBuffer` itself, then called `wl_resource_destroy`,
which fired the destructor lambda that deleted it again. Removed the manual delete —
ownership belongs to the resource destroy handler.

### 3. Test client aborted on v5+ seat events (`tools/interactive_window.c`)

The listener structs only defined pre-v5 events, but the compositor binds `wl_seat` at
v7, so libwayland hit a NULL listener and killed the client on the first v5+ event
(`listener function for opcode 5 of wl_pointer is NULL`).

Added the missing handlers:
- `wl_pointer`: `frame`, `axis_source`, `axis_stop`, `axis_discrete`,
  `axis_value120`, `axis_relative_direction`
- `wl_touch`: `shape`, `orientation`

### 4. Input latency: blocking dispatch

- `WaylandServer::run()` used `wl_event_loop_dispatch(loop, 16)` — a 16ms poll that
  spins even when idle and adds up to 16ms latency to every input event. Now
  `dispatch(loop, -1)` (block until an event).
- The wake pipe was created but **never registered** with the event loop, so blocking
  dispatch would have made `stop()` hang. Now registered as `mWakeSource` and removed
  in the destructor. Note the ordering: `mEventLoop` must be assigned *before*
  registering, or `wl_event_loop_add_fd` gets `nullptr`.
- `interactive_window.c` had `wl_display_dispatch()` followed by `usleep(100000)`.
  The sleep was pure added latency (dispatch already blocks) and the `elapsed`
  counter counted *iterations*, not seconds, so the timeout wasn't real seconds.
  Now: no sleep, wall-clock `CLOCK_MONOTONIC` deadline.

Net latency: ~116ms → near zero. Idle blits: continuous → 0.

### 5. Fullscreen support (`tools/interactive_window.c`)

`W`/`H` were `static const` (compile-time). Now runtime-configurable:
`--fullscreen` resolves against the advertised `wl_output` mode (binds `wl_output` and
reads its size rather than hardcoding 720×1600), and `--size <w> <h>` for arbitrary
sizes. Positional timeout arg still works.

### 6. Damage tracking — read this before touching it

`surface_damage` / `surface_damage_buffer` were no-op stubs. They now accumulate a
union rect into `WaylandSurface::pending.damage` (a `Rect` with an `add()` that unions,
plus a `valid` flag; cleared after each commit). An empty/invalid rect means full repaint.

**The trap I hit, and the reason for the current design:** my first attempt copied only
the damaged rect straight into the buffer returned by `Surface::lock()`. That produced
flicker, random coloured squares and clipping. The cause:

> `Surface::lock()` dequeues a buffer from BufferQueue whose contents are **undefined**,
> and `unlockAndPost()` returns it. Partial-copying into it leaves the rest of that
> buffer as stale garbage from an arbitrary previous frame.

The passing of a dirty rect to `Surface::lock()` does *not* give you buffer-age
preservation on a dequeued buffer. I was wrong about that; don't reintroduce it.

**Current design (correct):** each surface owns a persistent `backBuffer`
(`std::vector<uint32_t>` + `backStride/Width/Height`, in `WaylandSurface`). Per commit:
1. `mergeDamage()` copies only the damaged rect from the client shm buffer into the
   back buffer (cheap — proportional to damage).
2. `presentBackBuffer()` copies the **whole** back buffer into the freshly-dequeued SF
   buffer and posts.

So the SF-facing buffer is always complete, damage tracking is still cheap, and there
are no artifacts. A resize invalidates the back buffer and forces a full repaint
(a resized image can't be patched).

`SurfaceFlingerBridge::lockBuffer()` gained an optional `const android_rect_t* dirty`
and a `DirtyRect` out-param that reports the damage clamped to the buffer actually
returned. It is currently called with `nullptr` (full) by `presentBackBuffer`. The
clamping logic is still worth keeping for future use.

`android_rect_t` was added to `SurfaceFlingerBridge.h` so AOSP types don't leak into
the header (it mirrors `android::Rect`).

Verified: `blitShmBuffer: 720x1600 src, partial damage`, no artifacts after 5 rapid taps.

### 7. Client repaint optimisation (`tools/interactive_window.c`)

- Split into `drawStaticChrome()` (once at init: background, border, titlebar, traffic
  lights, info-card frame) and `redraw()` (per event). The 1.15M-pixel border loop no
  longer runs on every touch.
- `redraw()` commits a bounded damage rect (`kDirtyX0=20, kDirtyY0=60` →
  `W-20, 380`) instead of the full surface. The client's own `g_pixels` mmap is
  persistent, so this rect is accurate.
- Every dynamic element clears its own footprint before redrawing (varying-length text
  would otherwise smear). The ripple erases its previous position, clamped to
  `y >= 258` so it can't punch a hole through the info card.
- **Known cosmetic issue:** the ripple erase is clamped to `y >= 258`, so a ring drawn
  above that line leaves a small sliver of magenta at the info-card edge. Not fixed;
  visible only in the top ~30px of the free area.

---

## Current state: what works

Verified on device `RKNNWK5LSWRO8TIJ` (720×1600 @60Hz):

- Compositor boots, connects to SurfaceFlinger, creates the socket.
- `wl_compositor` (v5), `wl_shm`, `xdg_wm_base` (v5), `wl_seat` (v7), `wl_output` (v4).
- 5 evdev devices enlisted; `/dev/input/event2` correctly detected as touchscreen.
- Full XKB keymap (60911 bytes) via `memfd`, repeat_info 33Hz/500ms.
- Touch → `wl_touch` **and** `wl_pointer` emulation (slot 0), keyboard → `wl_keyboard`.
- Hit-testing with correct local-coordinate translation.
- Fullscreen surface (720×1600) and windowed (560×400, centred).
- Damage-tracked partial blits, no flicker/clipping.
- Clean teardown — no orphaned layers.

---

## Known gaps (not yet done)

These are the real remaining work; several were noted in the original session too.

### GPU path is entirely absent — `ENABLE_DMABUF=ON` is a no-op

Despite the flag compiling, nothing implements it:
- `registerLinuxDmaBufGlobal()` (`WaylandServer.cpp`) has an **empty body**. No
  `wl_global_create` for dmabuf — only 4 exist (compositor, xdg_wm_base, seat, output).
- `SurfaceBridge::attachDmaBuf()` is declared but never defined or called.
- `SurfaceFlingerBridge::attachDmaBuf()` passes `nullptr` as the `native_handle_t` to
  `AHardwareBuffer_createFromHandle`, so it always fails.
- `AHardwareBufferHelper::importDmaBuf()` just logs `"not yet implemented (M5)"`.

Everything is CPU: client shm → `memcpy` → gralloc → GPU composite. There is **no GPU
rendering flag**; `--test` only draws a static pattern. `EGL`/`GLESv2` are linked
(`CMakeLists.txt`) but unused.

To implement, roughly in dependency order:
1. `zwp_linux_dmabuf_v1` — actually `wl_global_create` it, handle
   `create_params`/`create`/`destroy`.
2. Wrap the DMA-BUF fd in a `native_handle_t`. On modern Android this means the
   **gralloc4 mapper HAL**, *not* `AHardwareBuffer_createFromHandle` (the current call
   site is the wrong API for a raw fd).
3. `setBuffer` on the layer's `ANativeWindow` and queue it, replacing the memcpy path.
4. Advertise DRM format/modifier support matching the device's gralloc.

Caveat: DMA-BUF availability is per-device and driver-dependent. **Check what this
device actually advertises before building it** — some Mali/Adreno drivers restrict
gralloc4 import from a DMA-BUF fd.

### Other unfinished items

- **No resize support.** Layer size is fixed at first commit; `setSize`/`setParams` are
  declared in `SurfaceFlingerBridge.h` but never defined (link error if called).
- `Transaction::apply()` can't move a layer to (0,0): `if (p.x != 0 || p.y != 0)`.
  `LayerParams::width/height` are never applied.
- **Blits assume 4 bytes/pixel**, ignoring `WL_SHM_FORMAT`. XRGB8888 vs ARGB8888
  alpha handling is unverified.
- `xdg_toplevel.set_title` only logs; no back-pointer to the owning surface. Title,
  fullscreen, move/resize requests are all stubs. No window management at all — no
  focus-follows-click, no decorations, no keybindings.
- `wl_compositor.create_region` is a stub; `set_opaque_region` / `set_input_region` /
  `offset` / `set_buffer_transform` / `set_buffer_scale` are no-ops.
- `DisplayManager` is a stub — no rotation/display-change handling. `DisplayInfo`
  ignores the surface-space rect and only uses active mode width/height.
- `ExtensionRegistry` is a stub (no extension IPC socket).
- `registerViewporterGlobal` and `registerLayerShellGlobal` are also empty bodies.
- `SeatManager::run()` is just a `usleep` loop; real input runs on the event-loop fds.
  The dedicated input thread in `main.cpp` is therefore pointless.
- `SeatManager::stop()` removes event sources without synchronisation.
- Frame callbacks fire immediately on commit with timestamp `0`, not at vsync — so a
  client throttling on `wl_surface.frame` will spin as fast as it can commit.
- Single `wl_buffer` in the test client (safe today only because the compositor's copy
  is synchronous within commit).
- No client authentication, no `0777`-tightening; `/data/wayland` is world-writable.
- Binary is built `-fno-rtti` against AOSP headers — fragile across Android versions.
- `ARCHITECTURE.md` is a 7-line stub referring to "the conversation artifact", which
  no longer exists.

### Suggested priority

1. Verify the damage path holds up under fast drags (double-buffer the client if not).
2. Fix the ripple sliver (or drop the ripple).
3. Implement `setSize`/resize so window resizing works.
4. Investigate DMA-BUF feasibility on this device, then implement the GPU path — this is
   the single biggest perf win available.
5. Replace `ARCHITECTURE.md` with something real.
