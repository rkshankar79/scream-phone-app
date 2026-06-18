package com.spa.scream

import android.Manifest
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.widget.Toast
import androidx.activity.ComponentActivity
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.ColorScheme
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import androidx.core.content.ContextCompat
import com.spa.scream.ui.LineChart
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

class MainActivity : ComponentActivity() {

    private val requestNotif =
        registerForActivityResult(ActivityResultContracts.RequestPermission()) {}

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        maybeRequestNotificationPermission()
        setContent {
            MaterialTheme(colorScheme = appColors()) {
                AppScreen()
            }
        }
    }

    private fun maybeRequestNotificationPermission() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            val granted = ContextCompat.checkSelfPermission(
                this, Manifest.permission.POST_NOTIFICATIONS,
            ) == PackageManager.PERMISSION_GRANTED
            if (!granted) requestNotif.launch(Manifest.permission.POST_NOTIFICATIONS)
        }
    }
}

private fun appColors(): ColorScheme = darkColorScheme(
    primary = Color(0xFF4FC3F7),
    background = Color(0xFF101418),
    surface = Color(0xFF1A2026),
)

@Composable
private fun AppScreen() {
    val context = androidx.compose.ui.platform.LocalContext.current
    val running by ScreamEngine.running.collectAsState()
    val metrics by ScreamEngine.metrics.collectAsState()
    val error by ScreamEngine.lastError.collectAsState()
    val rxRunning by ScreamEngine.rxRunning.collectAsState()
    val rxMetrics by ScreamEngine.rxMetrics.collectAsState()
    val sampleCount by ScreamEngine.sampleCount.collectAsState()

    val scope = rememberCoroutineScope()
    val exportLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.CreateDocument("text/csv"),
    ) { uri ->
        if (uri == null) return@rememberLauncherForActivityResult
        scope.launch {
            val ok = withContext(Dispatchers.IO) {
                runCatching {
                    context.contentResolver.openOutputStream(uri)?.use { os ->
                        os.write(ScreamEngine.exportCsv().toByteArray())
                    } ?: error("could not open output stream")
                }.isSuccess
            }
            Toast.makeText(
                context,
                if (ok) "CSV exported" else "CSV export failed",
                Toast.LENGTH_SHORT,
            ).show()
        }
    }

    var serverIp by remember { mutableStateOf("10.0.2.2") }
    var serverPort by remember { mutableStateOf("30000") }
    var localPort by remember { mutableStateOf("0") }
    var startRate by remember { mutableStateOf("2000") }
    var maxRate by remember { mutableStateOf("50000") }
    var ect by remember { mutableStateOf("-1") }
    var listenPort by remember { mutableStateOf("30000") }

    Column(
        modifier = Modifier
            .fillMaxSize()
            .background(MaterialTheme.colorScheme.background)
            .verticalScroll(rememberScrollState())
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        Text(
            "SCReAM Phone App (SPA)",
            style = MaterialTheme.typography.headlineSmall,
            color = MaterialTheme.colorScheme.onBackground,
        )

        Card(modifier = Modifier.fillMaxWidth()) {
            Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    Field("Server IP", serverIp, Modifier.weight(2f)) { serverIp = it }
                    Field("Port", serverPort, Modifier.weight(1f), KeyboardType.Number) { serverPort = it }
                }
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    Field("Local port (0=auto)", localPort, Modifier.weight(1f), KeyboardType.Number) { localPort = it }
                }
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    Field("Start kbps", startRate, Modifier.weight(1f), KeyboardType.Number) { startRate = it }
                    Field("Max kbps", maxRate, Modifier.weight(1f), KeyboardType.Number) { maxRate = it }
                    Field("ECT(-1/0/1)", ect, Modifier.weight(1f), KeyboardType.Number) { ect = it }
                }
                Button(
                    onClick = {
                        if (running) {
                            ScreamService.stop(context)
                        } else {
                            val cfg = SessionConfig(
                                serverIp = serverIp.trim(),
                                serverPort = serverPort.toIntOrNull() ?: 30000,
                                localPort = localPort.toIntOrNull() ?: 0,
                                startRateKbps = startRate.toIntOrNull() ?: 2000,
                                maxRateKbps = maxRate.toIntOrNull() ?: 50000,
                                ect = ect.toIntOrNull() ?: -1,
                            )
                            ScreamService.start(context, cfg)
                        }
                    },
                    modifier = Modifier.fillMaxWidth(),
                ) {
                    Text(if (running) "Stop" else "Start")
                }
                error?.let {
                    Text("Error: $it", color = Color(0xFFEF5350))
                }
            }
        }

        Card(modifier = Modifier.fillMaxWidth()) {
            Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text("Receiver (loopback)", style = MaterialTheme.typography.titleMedium,
                    color = MaterialTheme.colorScheme.onSurface)
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    Field("Listen port", listenPort, Modifier.weight(1f), KeyboardType.Number) { listenPort = it }
                    Button(
                        onClick = {
                            if (rxRunning) ScreamEngine.stopReceiver()
                            else ScreamEngine.startReceiver(listenPort.toIntOrNull() ?: 30000)
                        },
                        modifier = Modifier.weight(1f),
                    ) { Text(if (rxRunning) "Stop RX" else "Start RX") }
                }
                Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
                    Stat("RTP rcvd", "${rxMetrics.rtpReceived}")
                    Stat("FB sent", "${rxMetrics.feedbackSent}")
                    Stat("Rcv rate", "%.0f kbps".format(rxMetrics.receivedRateBps / 1000))
                }
            }
        }

        StatGrid(metrics)

        Button(
            onClick = {
                val ts = SimpleDateFormat("yyyyMMdd_HHmmss", Locale.US).format(Date())
                exportLauncher.launch("scream_$ts.csv")
            },
            enabled = sampleCount > 0,
            modifier = Modifier.fillMaxWidth(),
        ) {
            Text(if (sampleCount > 0) "Export CSV ($sampleCount samples)" else "Export CSV (no data)")
        }

        val h = ScreamEngine.history
        LineChart("Congestion window", h.cwndBytes.toList(), Color(0xFF4FC3F7), "B")
        LineChart("RTT", h.sRttMs.toList(), Color(0xFF81C784), "ms")
        LineChart("TX bitrate", h.txBitrateKbps.toList(), Color(0xFFFFB74D), "kbps")
        LineChart("Target bitrate", h.targetKbps.toList(), Color(0xFFBA68C8), "kbps")
        LineChart("Queue delay", h.queueDelayMs.toList(), Color(0xFFE57373), "ms")
        LineChart("Pacing rate", h.pacingKbps.toList(), Color(0xFF4DD0E1), "kbps")
        LineChart("Packet loss", h.lossPercent.toList(), Color(0xFFFF8A65), "%")
    }
}

@Composable
private fun StatGrid(m: Metrics) {
    Card(modifier = Modifier.fillMaxWidth()) {
        Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
                Stat("CWND", "${m.cwndBytes} B")
                Stat("RTT", "%.1f ms".format(m.sRttSeconds * 1000))
                Stat("Loss", "%.2f %%".format(m.lossRatePercent))
            }
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
                Stat("TX", "%.0f kbps".format(m.txBitrateBps / 1000))
                Stat("Target", "%.0f kbps".format(m.targetBitrateBps / 1000))
                Stat("Pacing", "%.0f kbps".format(m.pacingRateBps / 1000))
            }
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
                Stat("Q delay", "%.1f ms".format(m.queueDelaySeconds * 1000))
                Stat("Pkts", "${m.packetsSent}")
                Stat("FB", "${m.feedbackPackets}")
            }
        }
    }
}

@Composable
private fun Stat(label: String, value: String) {
    Column {
        Text(label, style = MaterialTheme.typography.labelSmall, color = Color(0xFF90A4AE))
        Text(value, style = MaterialTheme.typography.bodyLarge, color = MaterialTheme.colorScheme.onSurface)
    }
}

@Composable
private fun Field(
    label: String,
    value: String,
    modifier: Modifier = Modifier,
    keyboard: KeyboardType = KeyboardType.Text,
    onChange: (String) -> Unit,
) {
    OutlinedTextField(
        value = value,
        onValueChange = onChange,
        label = { Text(label) },
        singleLine = true,
        keyboardOptions = KeyboardOptions(keyboardType = keyboard),
        modifier = modifier,
    )
}
