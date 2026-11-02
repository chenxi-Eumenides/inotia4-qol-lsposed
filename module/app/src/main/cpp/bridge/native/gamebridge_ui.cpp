#include "gamebridge_internal.h"

#include "game/access/ui_state.h"


extern "C" JNIEXPORT jstring JNICALL
Java_com_inotia4_qol_NativeBridge_nativeNpcDialogOptions(JNIEnv* env, jclass) {
    return env->NewStringUTF(qol::game::npc_dialog_options_json().c_str());
}
