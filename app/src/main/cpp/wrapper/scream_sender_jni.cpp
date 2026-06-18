#include <jni.h>

#include <string>

#include "TestSession.h"

// Metrics array layout shared with Kotlin (ScreamBridge.METRICS_SIZE).
// Keep in sync with com.spa.scream.ScreamBridge.
namespace {
constexpr int kMetricsSize = 16;

screamtest::TestSession* asSession(jlong handle) {
    return reinterpret_cast<screamtest::TestSession*>(handle);
}
}  // namespace

extern "C" {

JNIEXPORT jlong JNICALL
Java_com_spa_scream_ScreamBridge_nativeCreate(JNIEnv*, jobject) {
    return reinterpret_cast<jlong>(new screamtest::TestSession());
}

JNIEXPORT void JNICALL
Java_com_spa_scream_ScreamBridge_nativeDestroy(JNIEnv*, jobject,
                                                   jlong handle) {
    delete asSession(handle);
}

JNIEXPORT jstring JNICALL Java_com_spa_scream_ScreamBridge_nativeStart(
    JNIEnv* env, jobject, jlong handle, jstring serverIp, jint serverPort,
    jint localPort, jint minRateKbps, jint startRateKbps, jint maxRateKbps,
    jint fps, jint mtu, jint ect, jfloat delayTarget, jint ssrc) {
    auto* session = asSession(handle);
    if (session == nullptr) {
        return env->NewStringUTF("null session handle");
    }

    const char* ipChars = env->GetStringUTFChars(serverIp, nullptr);
    screamtest::SessionConfig cfg;
    cfg.serverIp = ipChars ? ipChars : "127.0.0.1";
    if (ipChars) {
        env->ReleaseStringUTFChars(serverIp, ipChars);
    }
    cfg.serverPort = serverPort;
    cfg.localPort = localPort;
    cfg.minRateKbps = minRateKbps;
    cfg.startRateKbps = startRateKbps;
    cfg.maxRateKbps = maxRateKbps;
    cfg.fps = fps;
    cfg.mtu = mtu;
    cfg.ect = ect;
    cfg.delayTargetSeconds = delayTarget;
    cfg.ssrc = static_cast<uint32_t>(ssrc);

    std::string err;
    if (!session->start(cfg, err)) {
        return env->NewStringUTF(err.c_str());
    }
    return nullptr;  // success
}

JNIEXPORT void JNICALL
Java_com_spa_scream_ScreamBridge_nativeStop(JNIEnv*, jobject, jlong handle) {
    auto* session = asSession(handle);
    if (session != nullptr) {
        session->stop();
    }
}

JNIEXPORT jboolean JNICALL
Java_com_spa_scream_ScreamBridge_nativeIsRunning(JNIEnv*, jobject,
                                                     jlong handle) {
    auto* session = asSession(handle);
    return (session != nullptr && session->isRunning()) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jdoubleArray JNICALL
Java_com_spa_scream_ScreamBridge_nativeGetMetrics(JNIEnv* env, jobject,
                                                      jlong handle) {
    jdoubleArray arr = env->NewDoubleArray(kMetricsSize);
    if (arr == nullptr) {
        return nullptr;
    }
    auto* session = asSession(handle);
    if (session == nullptr) {
        return arr;
    }

    screamtest::MetricsSnapshot m = session->snapshot();
    jdouble vals[kMetricsSize];
    vals[0] = m.cwndBytes;
    vals[1] = m.bytesInFlight;
    vals[2] = m.sRttSeconds;
    vals[3] = m.queueDelaySeconds;
    vals[4] = m.rtpQueueDelaySeconds;
    vals[5] = m.txBitrateBps;
    vals[6] = m.targetBitrateBps;
    vals[7] = m.pacingRateBps;
    vals[8] = m.lossRatePercent;
    vals[9] = m.lossEpoch ? 1.0 : 0.0;
    vals[10] = m.ceMarkPercent;
    vals[11] = static_cast<double>(m.ceMarkCount);
    vals[12] = static_cast<double>(m.packetsSent);
    vals[13] = static_cast<double>(m.bytesSent);
    vals[14] = static_cast<double>(m.feedbackPackets);
    vals[15] = m.sessionTimeSeconds;
    env->SetDoubleArrayRegion(arr, 0, kMetricsSize, vals);
    return arr;
}

}  // extern "C"
