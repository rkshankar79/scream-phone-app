package com.spa.scream.ui

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.unit.dp

/**
 * Minimal dependency-free line chart over a rolling series of samples.
 * Auto-scales the Y axis to the visible data; X is sample index.
 */
@Composable
fun LineChart(
    title: String,
    data: List<Float>,
    color: Color,
    unit: String = "",
    modifier: Modifier = Modifier,
) {
    val latest = data.lastOrNull() ?: 0f
    Column(modifier = modifier.fillMaxWidth().padding(vertical = 6.dp)) {
        Text(
            text = "$title:  ${formatValue(latest)} $unit",
            style = MaterialTheme.typography.labelLarge,
            color = MaterialTheme.colorScheme.onSurface,
        )
        Canvas(
            modifier = Modifier
                .fillMaxWidth()
                .height(110.dp)
                .padding(top = 4.dp),
        ) {
            val grid = Color(0xFF333333)
            drawLine(grid, start = androidx.compose.ui.geometry.Offset(0f, size.height),
                end = androidx.compose.ui.geometry.Offset(size.width, size.height), strokeWidth = 1.5f)
            drawLine(grid, start = androidx.compose.ui.geometry.Offset(0f, 0f),
                end = androidx.compose.ui.geometry.Offset(0f, size.height), strokeWidth = 1.5f)

            if (data.size < 2) return@Canvas

            val maxV = (data.maxOrNull() ?: 1f).coerceAtLeast(1e-3f)
            val minV = (data.minOrNull() ?: 0f).coerceAtMost(maxV)
            val range = (maxV - minV).coerceAtLeast(1e-3f)
            val dx = size.width / (data.size - 1).toFloat()

            val path = Path()
            data.forEachIndexed { i, v ->
                val x = i * dx
                val y = size.height - ((v - minV) / range) * size.height
                if (i == 0) path.moveTo(x, y) else path.lineTo(x, y)
            }
            drawPath(path, color = color, style = Stroke(width = 3f))
        }
    }
}

private fun formatValue(v: Float): String = when {
    v >= 1000f -> String.format("%,.0f", v)
    v >= 10f -> String.format("%.1f", v)
    else -> String.format("%.2f", v)
}
