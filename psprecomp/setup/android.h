// The Android side of the setup wizard (compiled on Android only): the few services the Java activity (android/setup/java/.../SetupActivity.java)
// offers to the native wizard through JNI. See docs/android.md "Building on the phone".
#pragma once
#ifdef __ANDROID__
#include <string>

namespace setup::android {
std::string native_lib_dir();          // where lib*.so (the compiler, recompiler, Python, the payload zip) were unpacked: the only place an app may run files from
std::string files_dir();               // the app's private folder (the unpacked payload and the work folder live there)
bool has_all_files_access();           // "all files access": reading the disc image from the SD card / Download
void request_all_files_access();       // opens Android's settings page for it
bool can_install_packages();           // "install unknown apps" for this app
void request_install_permission();
void keep_awake(bool on);              // wake lock + screen on during the long build
void install_apk(const std::string& path);   // PackageInstaller; Android shows its own confirmation
std::string install_status();
std::string app_version();             // the app's own version name (android:versionName)
void download(const std::string& url, const std::string& dest);   // HTTPS via Android's stack, on a thread
std::string download_status();         // "" | "working:<bytes>:<total>" | "ok" | "failed: ..."
void cancel_download();
void pick_file();
void clear_pick();                     // forget the last answer (so it is not taken twice)                      // Android's own file picker (no storage permission needed)
std::string pick_status();             // "" | "pending" | "cancelled" | "failed: ..." | "ok:<fd>:<file name>"          // "" (never started) | "working" | "ok" | "failed: ..."
}  // namespace setup::android
#endif
