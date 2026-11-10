// Android glue, see psp_android.cpp.
#pragma once
#include <string>
#ifdef __ANDROID__
std::string psp_android_workdir();   // <external storage>/Android/data/<package>/files/game
void psp_android_init();             // log redirect, chdir, default PSPRECOMP_* paths (call first in main)
#endif
