package org.psprecomp.setup;

/**
 * The install engine (setup/android_engine.cpp in libsetupcore.so): the same install logic as the desktop wizard (setup/install.cpp) for
 * the Java pages. Plans, process running (one step at a time), progress, install.ini, the updater's command lines. Options travel as
 * "key=value" lines (the install.ini format); plans, progress and update answers as JSON.
 */
final class Engine {
    static {
        System.loadLibrary("setupcore");
    }

    interface ProgressCb {
        void onProgress(double f);
    }

    static native String bootstrap(String nativeLibDir, String filesDir, ProgressCb cb);
    static native String layoutProblem();
    static native boolean feature(String name);
    static native String payloadFingerprint();
    static native String gameDir();
    /** The game's disc files are still on the device (the game folder, or an older install's work folder). */
    static native boolean discPresent(String work);
    static native String releaseRoot();
    static native String loadState(String file);
    static native boolean saveState(String file, String text);
    static native String plan(String kind, String iso, String workdir, String options, String installedOptions);
    static native String extrasJson();
    static native String[] extrasNeedingDisc(String options, String installedOptions);
    static native String discProblem(String extraKey, String path);
    static native String startStep(int i);
    static native String[] readLines();
    static native boolean busy();
    static native int exitCode();
    static native void kill();
    static native void noteDownload(double fraction, String detail);
    static native String progress();
    static native void clearError();
    static native String[] checkArgv(String manifest, String sig, String current);
    static native String[] verifyArgv(String file, String sha256, long size);
    static native String[] runTool(String[] argv, String workdir, int timeoutMs);
    static native String parseUpdate(String line);
    static native void inheritFd(int fd);
    static native long freeSpace(String path);
    // free space an install / the extras need (setup/install.cpp needed_bytes) and the failure text when `workdir` has less ("" = enough)
    static native long neededBytes(String options, String workdir, boolean extrasOnly);
    static native String spaceProblem(String workdir, long need);
    // why a step failed, in words: { kind, title, text } (setup/install.cpp explain_failure)
    static native String[] explainFailure(String step, String stage, String error, String tail, String workdir, int code, boolean cancelled);
    // the disc an extra needs, looked for in the folder of the first image (a real path): "" = none
    static native String findExtraDisc(String key, String nearFile);
    // Report an issue (the game's psp_report.cpp): files = rel, path, rel, path ...; facts = key, value ...
    static native String[] reportPreview(String version, String spaceDir, String[] files, String[] facts);
    static native String reportBuild(String version, String spaceDir, String[] files, String[] facts, String title, String desc, String outZip);
    static native String reportSign(String time, String zipFile);

    private Engine() {}
}
