# Eden SaveTrace v2 / RuntimeTrace diagnostic fork

Base: `eden-emulator/mirror` master, commit `10bcd2d849843b146a79c61a21df615e1bdcfcde` (downloaded 2026-10-07).

SaveTrace records save operations without changing the return value or implementation of `Commit` and `SaveDataFactory`. RuntimeTrace v2 also keeps a bounded history of guest SVC calls after SaveTrace arms; it never changes SVC arguments, return values, guest memory, or scheduling. The Android mainline flavor is labeled **Eden SaveTrace v1** and uses the separate package suffix `.savetrace` so it does not overwrite a normal Eden installation. Import a save into this installation before comparing behavior.

The in-game panel supports **ARM TRACE**, **STOP & SAVE LOG**, and **VIEW TRACE**. ARM offers Normal and Deep, with manual or Auto Trigger modes. Auto Trigger is enabled at startup and begins at the first save create/open. Deep additionally reads file contents for FNV-1a hashes and records available guest PC/LR. Unavailable values are reported as such; a zero PC/LR means no guest context was available on the servicing thread.

Trace files are under `logs/SaveTrace/`: `trace.jsonl`, `trace.txt`, `ring.jsonl`, `first_divergence.txt`, `runtime_ring.jsonl`, and `runtime_summary.txt`. Logs rotate at 8 MiB, keeping one `.old` file. The in-memory ring holds 256 events and is rewritten to `ring.jsonl` after each event. Android's **EXPORT DIAGNOSTIC PACKAGE** button shares a ZIP containing these files.

`first_divergence.txt` reports the first nonzero guest-visible or host result, or a mismatch between guest and host write counts. `runtime_ring.jsonl` retains up to the latest 512 SVC events and is snapshotted periodically and immediately for exit, break, exception-related, or unhandled SVCs. `runtime_summary.txt` records the snapshot reason and last SVC. Its conclusion is limited to captured events and cannot establish that a stubbed `Commit` persisted data.

Build on a machine with JDK 17, Android SDK, NDK `28.2.13676358`, and CMake `3.31.6`:

```text
cd src/android
gradlew.bat assembleMainlineRelWithDebInfo
```

For a hosted build, push this source to a GitHub repository you control, then open **Actions → Build SaveTrace v1 Android APK → Run workflow**. The included workflow installs the Android toolchain on a GitHub runner and publishes the ARM64 APK as a downloadable workflow artifact for 14 days.

The ARM64 ABI is selected by the mainline flavor. This source package has not been compiled because the preparation machine has no Java, Android SDK, or NDK. Gradle stopped with `JAVA_HOME is not set and no 'java' command could be found in your PATH.` No APK is included or claimed verified.
