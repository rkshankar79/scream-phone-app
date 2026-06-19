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
 * Foreground service that keeps native sender and/or receiver sessions alive while
 * a test runs. Holds a partial wakelock and a persistent notification so UDP/RTCP
 * threads are not suspended when the app is backgrounded.
 */
class ScreamService : Service() {

    private var wakeLock: PowerManager.WakeLock? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_START -> {
                ensureForeground()
                ScreamEngine.start(intent.toSessionConfig())
                updateNotification()
            }
            ACTION_STOP -> {
                ScreamEngine.stop()
                maybeStopService()
            }
            ACTION_START_RX -> {
                ensureForeground()
                val port = intent.getIntExtra(EXTRA_LISTEN_PORT, 30000)
                ScreamEngine.startReceiver(port)
                updateNotification()
            }
            ACTION_STOP_RX -> {
                ScreamEngine.stopReceiver()
                maybeStopService()
            }
            ACTION_STOP_ALL -> {
                ScreamEngine.stop()
                ScreamEngine.stopReceiver()
                tearDownService()
            }
        }
        return START_NOT_STICKY
    }

    private fun ensureForeground() {
        startForeground(NOTIF_ID, buildNotification())
        acquireWakeLock()
    }

    private fun maybeStopService() {
        if (!ScreamEngine.isSenderRunning() && !ScreamEngine.isReceiverRunning()) {
            tearDownService()
        } else {
            updateNotification()
        }
    }

    private fun tearDownService() {
        releaseWakeLock()
        stopForeground(STOP_FOREGROUND_REMOVE)
        stopSelf()
    }

    private fun updateNotification() {
        val mgr = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        mgr.notify(NOTIF_ID, buildNotification())
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
        val sender = ScreamEngine.isSenderRunning()
        val receiver = ScreamEngine.isReceiverRunning()
        val text = when {
            sender && receiver -> "Sender + receiver active"
            sender -> "Sending RTP/UDP test traffic"
            receiver -> "Receiving RTP, sending RTCP feedback"
            else -> "SCReAM session"
        }
        return Notification.Builder(this, CHANNEL_ID)
            .setContentTitle("SCReAM Phone App")
            .setContentText(text)
            .setSmallIcon(
                if (receiver && !sender) android.R.drawable.stat_sys_download
                else android.R.drawable.stat_sys_upload,
            )
            .setOngoing(true)
            .build()
    }

    override fun onDestroy() {
        ScreamEngine.stop()
        ScreamEngine.stopReceiver()
        releaseWakeLock()
        super.onDestroy()
    }

    override fun onBind(intent: Intent?): IBinder? = null

    companion object {
        const val ACTION_START = "com.spa.scream.START"
        const val ACTION_STOP = "com.spa.scream.STOP"
        const val ACTION_START_RX = "com.spa.scream.START_RX"
        const val ACTION_STOP_RX = "com.spa.scream.STOP_RX"
        const val ACTION_STOP_ALL = "com.spa.scream.STOP_ALL"

        private const val EXTRA_LISTEN_PORT = "listenPort"
        private const val CHANNEL_ID = "scream_session"
        private const val NOTIF_ID = 1
        private const val MAX_SESSION_MS = 6 * 60 * 60 * 1000L  // 6h safety cap

        fun start(context: Context, cfg: SessionConfig) {
            val intent = Intent(context, ScreamService::class.java).apply {
                action = ACTION_START
                putConfig(cfg)
            }
            startServiceIntent(context, intent)
        }

        fun stop(context: Context) {
            context.startService(
                Intent(context, ScreamService::class.java).apply { action = ACTION_STOP },
            )
        }

        fun startReceiver(context: Context, listenPort: Int) {
            val intent = Intent(context, ScreamService::class.java).apply {
                action = ACTION_START_RX
                putExtra(EXTRA_LISTEN_PORT, listenPort)
            }
            startServiceIntent(context, intent)
        }

        fun stopReceiver(context: Context) {
            context.startService(
                Intent(context, ScreamService::class.java).apply { action = ACTION_STOP_RX },
            )
        }

        /** Stops sender and receiver and tears down the foreground service. */
        fun stopAll(context: Context) {
            context.startService(
                Intent(context, ScreamService::class.java).apply { action = ACTION_STOP_ALL },
            )
        }

        private fun startServiceIntent(context: Context, intent: Intent) {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                context.startForegroundService(intent)
            } else {
                context.startService(intent)
            }
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
