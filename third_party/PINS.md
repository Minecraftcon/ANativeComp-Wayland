# third_party pins

`third_party/` is **git-ignored** — it holds ~26 MB of vendored upstream source,
and six of those directories are separate git repositories. Nesting them would
bloat every clone and complicate the build. This file records the exact
revisions so the tree is reproducible.

If you are setting up from a fresh clone, see **Setup** below.

## AOSP headers (private platform headers)

Partial clones (`--filter=blob:none`) of upstream Android trees, pinned to the
revisions that match the device's `libgui.so` / `libui.so` ABI. These are
**private** AOSP headers, not NDK headers — that is the whole reason they are
vendored.

| Directory | Upstream | Pinned commit |
|---|---|---|
| `aosp_frameworks_native` | https://github.com/LineageOS/android_frameworks_native.git | `d9596a35d391978fad41dc6ebad82750eabb7aa1` |
| `aosp_system_core` | https://github.com/LineageOS/android_system_core.git | `1dd6bab94356caa929a505fe5059954536b21a0a` |
| `aosp_system_libbase` | https://android.googlesource.com/platform/system/libbase | `5d87ea136b23dd152f00a81ecbb78e460b87ce94` |
| `aosp_system_logging` | https://android.googlesource.com/platform/system/logging | `37cb6ed458c92bc488b60963fd2cb8351ff30677` |
| `aosp_hardware_libhardware` | https://github.com/LineageOS/android_hardware_libhardware.git | `d05ce0065b8147d05adf0d68c8a182ebbe668d27` |
| `aosp_system_libhidl` | https://github.com/LineageOS/android_system_libhidl.git | `c147bfaed9b2f42afb1b5ab928c0b6719b3fe24a` |

To fetch one:

```bash
cd third_party
git clone --filter=blob:none https://github.com/LineageOS/android_frameworks_native.git aosp_frameworks_native
cd aosp_frameworks_native && git checkout d9596a35d391978fad41dc6ebad82750eabb7aa1
```

> **ABI warning.** These headers must match the `libgui.so` / `libui.so` on the
> target device. Newer or older AOSP revisions change private struct layouts
> and will produce crashes that look like memory corruption. If you move to a
> different Android version, re-pin against that branch
> (e.g. `android-13.0.0_r*` for Android 13).

## wayland 1.23.1

Downloaded and cross-compiled by `tools/build_wayland.sh` into
`third_party/wayland/`.

- Upstream: https://gitlab.freedesktop.org/wayland/wayland
- Version: **1.23.1**
- Source extracted to `third_party/wayland-src/`, built output installed to
  `third_party/wayland/{include,lib}/`

```bash
NDK=/path/to/ndk ./tools/build_wayland.sh
```

> **Scanner version gotcha.** The vendored `wayland-scanner` on the host must
> satisfy wayland 1.23.1's meson dependency. A host `wayland-scanner` that is
> *newer* than the wayland source is fine; one that is *older* fails with
> `Invalid version, need 'wayland-scanner' ['1.22.0']`.

## libffi

Tarball extract in `third_party/libffi-src/`, built into `third_party/libffi/`.
Required by the vendored wayland. Not currently automated by a script in this
repo — see the libffi version your wayland build expects before relying on it.

## device-sysroot/

Also git-ignored: `lib64` stubs pulled off a **rooted device**, so they are
device-specific. Every developer extracts their own.

```bash
adb shell su -c "ls /system/lib64"   # reference
# copy the .so files you need (libgui.so, libui.so, ...) into device-sysroot/lib64/
```

`CMakeLists.txt` links against these with `-Wl,--allow-shlib-undefined`; the
libraries are resolved from `/system/lib64` at runtime on the device, so the
stubs exist only to satisfy the linker.
