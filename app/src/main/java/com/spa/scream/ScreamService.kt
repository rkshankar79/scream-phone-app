package com.spa.scream

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Context
import android.content.Intent
import android.os.Build
import android.os.IBinder
import android.os.PowerManager

/**
 * Foreground service that keeps the native sender alive (and the CPU awake for
 * sub-millisecond pacing) while a test session runs. The actual engine lives in
 * [ScreamEngine]; this service owns the lifecycle, notification and wakelock.
 */
class ScreamService : Service() {

    private var wakeLock: PowerManager.WakeLock? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_START -> {
                val cfg = intent.toSessionConfig()
                startForeground(NOTIF_ID, buildNotification())
                acquireWakeLock()
                ScreamEngine.start(cfg)
            }
            ACTION_STOP -> {
                stopSession()
            }
        }
        return START_NOT_STICKY
    }

    private fun stopSession() {
        ScreamEngine.stop()
        releaseWakeLock()
        stopForeground(STOP_FOREGROUND_REMOVE)
        stopSelf()
    }

    private fun acquireWakeLock() {
        if (wakeLock == null) {
            val pm = getSystemService(Context.POWER_SERVICE) as PowerManager
            wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "spa:session")
        }
        wakeLock?.takeIf { !it.isHeld }?.acquire(MAX_SESSION_MS)
    }

    private fun releaseWakeLock() {
        wakeLock?.takeIf { it.isHeld }?.release()
    }

    private fun buildNotification(): Notification {
        val mgr = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val channel = NotificationChannel(
                CHANNEL_ID,
                "SCReAM session",
                NotificationManager.IMPORTANCE_LOW,
            )
            mgr.createNotificationChannel(channel)
        }
        return Notification.Builder(this, CHANNEL_ID)
            .setContentTitle("SCReAM Phone App")
            .setContentText("Sending RTP/UDP test traffic")
            .setSmallIcon(android.R.drawable.stat_sys_upload)
            .setOngoing(true)
            .build()
    }

    override fun onDestroy() {
        releaseWakeLock()
        super.onDestroy()
    }

    override fun onBind(intent: Intent?): IBinder? = null

    companion object {
        const val ACTION_START = "com.spa.scream.START"
        const val ACTION_STOP = "com.spa.scream.STOP"
        private const val CHANNEL_ID = "scream_session"
        private const val NOTIF_ID = 1
        private const val MAX_SESSION_MS = 6 * 60 * 60 * 1000L  // 6h safety cap

        fun start(context: Context, cfg: SessionConfig) {
            val intent = Intent(context, ScreamService::class.java).apply {
                action = ACTION_START
                putConfig(cfg)
            }
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                context.startForegroundService(intent)
            } else {
                context.startService(intent)
            }
        }

        fun stop(context: Context) {
            val intent = Intent(context, ScreamService::class.java).apply {
                action = ACTION_STOP
            }
            context.startService(intent)
        }

        private fun Intent.putConfig(cfg: SessionConfig) {
            putExtra("serverIp", cfg.serverIp)
            putExtra("serverPort", cfg.serverPort)
            putExtra("localPort", cfg.localPort)
            putExtra("minRateKbps", cfg.minRateKbps)
            putExtra("startRateKbps", cfg.startRateKbps)
            putExtra("maxRateKbps", cfg.maxRateKbps)
            putExtra("fps", cfg.fps)
            putExtra("mtu", cfg.mtu)
            putExtra("ect", cfg.ect)
            putExtra("delayTargetSeconds", cfg.delayTargetSeconds)
            putExtra("ssrc", cfg.ssrc)
        }

        private fun Intent.toSessionConfig(): SessionConfig = SessionConfig(
            serverIp = getStringExtra("serverIp") ?: "127.0.0.1",
            serverPort = getIntExtra("serverPort", 30000),
            localPort = getIntExtra("localPort", 0),
            minRateKbps = getIntExtra("minRateKbps", 1000),
            startRateKbps = getIntExtra("startRateKbps", 2000),
            maxRateKbps = getIntExtra("maxRateKbps", 50000),
            fps = getIntExtra("fps", 50),
            mtu = getIntExtra("mtu", 1200),
            ect = getIntExtra("ect", -1),
            delayTargetSeconds = getFloatExtra("delayTargetSeconds", 0.06f),
            ssrc = getIntExtra("ssrc", 100),
        )
    }
}
