#include <jni.h>

#include "ReceiverSession.h"

// Receiver metrics array layout shared with Kotlin (ScreamBridge.RX_METRICS_SIZE).
namespace {
constexpr int kRxMetricsSize = 5;

screamtest::ReceiverSession* asReceiver(jlong handle) {
    return reinterpret_cast<screamtest::ReceiverSession*>(handle);
}
}  // namespace

extern "C" {

JNIEXPORT jlong JNICALL
Java_com_spa_scream_ScreamBridge_nativeCreateReceiver(JNIEnv*, jobject) {
    return reinterpret_cast<jlong>(new screamtest::ReceiverSession());
}

JNIEXPORT void JNICALL
Java_com_spa_scream_ScreamBridge_nativeDestroyReceiver(JNIEnv*, jobject,
                                                           jlong handle) {
    delete asReceiver(handle);
}

JNIEXPORT jstring JNICALL
Java_com_spa_scream_ScreamBridge_nativeStartReceiver(JNIEnv* env, jobject,
                                                         jlong handle,
                                                         jint listenPort,
                                                         jint ssrc) {
    auto* rx = asReceiver(handle);
    if (rx == nullptr) {
        return env->NewStringUTF("null receiver handle");
    }
    screamtest::ReceiverConfig cfg;
    cfg.listenPort = listenPort;
    cfg.ssrc = static_cast<uint32_t>(ssrc);

    std::string err;
    if (!rx->start(cfg, err)) {
        return env->NewStringUTF(err.c_str());
    }
    return nullptr;
}

JNIEXPORT void JNICALL
Java_com_spa_scream_ScreamBridge_nativeStopReceiver(JNIEnv*, jobject,
                                                        jlong handle) {
    auto* rx = asReceiver(handle);
    if (rx != nullptr) {
        rx->stop();
    }
}

JNIEXPORT jboolean JNICALL
Java_com_spa_scream_ScreamBridge_nativeIsReceiverRunning(JNIEnv*, jobject,
                                                             jlong handle) {
    auto* rx = asReceiver(handle);
    return (rx != nullptr && rx->isRunning()) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jdoubleArray JNICALL
Java_com_spa_scream_ScreamBridge_nativeGetReceiverMetrics(JNIEnv* env,
                                                              jobject,
                                                              jlong handle) {
    jdoubleArray arr = env->NewDoubleArray(kRxMetricsSize);
    if (arr == nullptr) {
        return nullptr;
    }
    auto* rx = asReceiver(handle);
    if (rx == nullptr) {
        return arr;
    }
    screamtest::ReceiverMetrics m = rx->snapshot();
    jdouble vals[kRxMetricsSize];
    vals[0] = static_cast<double>(m.rtpReceived);
    vals[1] = static_cast<double>(m.feedbackSent);
    vals[2] = static_cast<double>(m.ceMarked);
    vals[3] = m.receivedRateBps;
    vals[4] = m.sessionTimeSeconds;
    env->SetDoubleArrayRegion(arr, 0, kRxMetricsSize, vals);
    return arr;
}

}  // extern "C"
