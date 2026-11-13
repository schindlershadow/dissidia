#ifdef __ANDROID__
#include "android.h"

#include <SDL.h>
#include <jni.h>

namespace setup::android {
namespace {
JNIEnv* env() { return static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv()); }

// The activity object is a SetupActivity, so its class has the static helpers.
jclass activity_class(JNIEnv* e) {
    jobject act = static_cast<jobject>(SDL_AndroidGetActivity());
    return act ? e->GetObjectClass(act) : nullptr;
}

bool call_bool(const char* name) {
    JNIEnv* e = env();
    jclass c = e ? activity_class(e) : nullptr;
    if (!c) return false;
    jmethodID m = e->GetStaticMethodID(c, name, "()Z");
    return m && e->CallStaticBooleanMethod(c, m);
}

void call_void(const char* name) {
    JNIEnv* e = env();
    jclass c = e ? activity_class(e) : nullptr;
    if (!c) return;
    if (jmethodID m = e->GetStaticMethodID(c, name, "()V")) e->CallStaticVoidMethod(c, m);
}

std::string call_string(const char* name) {
    JNIEnv* e = env();
    jclass c = e ? activity_class(e) : nullptr;
    if (!c) return "";
    jmethodID m = e->GetStaticMethodID(c, name, "()Ljava/lang/String;");
    if (!m) return "";
    jstring s = static_cast<jstring>(e->CallStaticObjectMethod(c, m));
    if (!s) return "";
    const char* u = e->GetStringUTFChars(s, nullptr);
    std::string r = u ? u : "";
    if (u) e->ReleaseStringUTFChars(s, u);
    e->DeleteLocalRef(s);
    return r;
}
}  // namespace

std::string native_lib_dir() { return call_string("nativeLibDir"); }
std::string files_dir() {
    const char* p = SDL_AndroidGetInternalStoragePath();
    return p ? p : "";
}
bool has_all_files_access() { return call_bool("hasAllFilesAccess"); }
void request_all_files_access() { call_void("requestAllFilesAccess"); }
bool can_install_packages() { return call_bool("canInstallPackages"); }
void request_install_permission() { call_void("requestInstallPermission"); }

void keep_awake(bool on) {
    JNIEnv* e = env();
    jclass c = e ? activity_class(e) : nullptr;
    if (!c) return;
    if (jmethodID m = e->GetStaticMethodID(c, "keepAwake", "(Z)V")) e->CallStaticVoidMethod(c, m, (jboolean)on);
}

void install_apk(const std::string& path) {
    JNIEnv* e = env();
    jclass c = e ? activity_class(e) : nullptr;
    if (!c) return;
    jmethodID m = e->GetStaticMethodID(c, "installApk", "(Ljava/lang/String;)V");
    if (!m) return;
    jstring s = e->NewStringUTF(path.c_str());
    e->CallStaticVoidMethod(c, m, s);
    e->DeleteLocalRef(s);
}

std::string install_status() { return call_string("installStatus"); }
std::string app_version() { return call_string("appVersion"); }
std::string download_status() { return call_string("downloadStatus"); }
void cancel_download() { call_void("cancelDownload"); }
void download(const std::string& url, const std::string& dest) {
    JNIEnv* e = env();
    jclass c = e ? activity_class(e) : nullptr;
    if (!c) return;
    jmethodID m = e->GetStaticMethodID(c, "download", "(Ljava/lang/String;Ljava/lang/String;)V");
    if (!m) return;
    jstring u = e->NewStringUTF(url.c_str()), d = e->NewStringUTF(dest.c_str());
    e->CallStaticVoidMethod(c, m, u, d);
    e->DeleteLocalRef(u);
    e->DeleteLocalRef(d);
}
void pick_file() { call_void("pickFile"); }
void clear_pick() { call_void("clearPick"); }
std::string pick_status() { return call_string("pickStatus"); }
}  // namespace setup::android
#endif
