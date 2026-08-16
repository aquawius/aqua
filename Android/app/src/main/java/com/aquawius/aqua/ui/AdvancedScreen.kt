package com.aquawius.aqua.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.aquawius.aqua.AquaController

/** 高级：命令行版本的那几个可调参数 + 底部日志框。 */
@Composable
fun AdvancedScreen(controller: AquaController, modifier: Modifier = Modifier) {
    Column(
        modifier = modifier
            .fillMaxSize()
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        OutlinedTextField(
            value = controller.jitterLatencyMs,
            onValueChange = { controller.jitterLatencyMs = it.filter { c -> c.isDigit() } },
            label = { Text("JitterBuffer 目标延迟 (ms，0=默认 30)") },
            singleLine = true,
            modifier = Modifier.fillMaxWidth(),
        )
        OutlinedTextField(
            value = controller.driftThreshold,
            onValueChange = { controller.driftThreshold = it.filter { c -> c.isDigit() } },
            label = { Text("漂移 late 阈值 (包数，0=默认 15)") },
            singleLine = true,
            modifier = Modifier.fillMaxWidth(),
        )
        OutlinedTextField(
            value = controller.playbackBufferBytes,
            onValueChange = { controller.playbackBufferBytes = it.filter { c -> c.isDigit() } },
            label = { Text("播放缓冲 (字节，0=默认 16384)") },
            singleLine = true,
            modifier = Modifier.fillMaxWidth(),
        )

        Text("日志", style = androidx.compose.material3.MaterialTheme.typography.titleMedium)

        val listState = rememberLazyListState()
        LaunchedEffect(controller.log.size) {
            if (controller.log.isNotEmpty()) {
                listState.scrollToItem(controller.log.size - 1)
            }
        }
        LazyColumn(
            state = listState,
            modifier = Modifier
                .weight(1f)
                .fillMaxWidth(),
        ) {
            items(controller.log) { line ->
                Text(
                    text = line,
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(vertical = 2.dp),
                )
                HorizontalDivider()
            }
        }
    }
}
