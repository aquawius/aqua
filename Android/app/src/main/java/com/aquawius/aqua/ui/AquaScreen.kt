package com.aquawius.aqua.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.ArrowDropDown
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.aquawius.aqua.AquaClientState
import com.aquawius.aqua.AquaController
import com.aquawius.aqua.AquaDiagnostics
import java.util.Locale

private val presetPorts = listOf("50051", "50052", "50000", "8080", "9090")

/** 首页：地址 + 端口（下拉/可编辑）+ 状态/诊断 + 连接按钮。 */
@Composable
fun AquaScreen(controller: AquaController, modifier: Modifier = Modifier) {
    Column(
        modifier = modifier
            .fillMaxSize()
            .padding(16.dp),
    ) {
        Column(
            modifier = Modifier
                .weight(1f)
                .verticalScroll(rememberScrollState()),
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            OutlinedTextField(
                value = controller.serverIp,
                onValueChange = { controller.serverIp = it },
                label = { Text("服务器地址") },
                singleLine = true,
                modifier = Modifier.fillMaxWidth(),
            )

            PortField(
                value = controller.rpcPort,
                onValueChange = { controller.rpcPort = it },
            )

            StatusCard(controller)

            DiagnosticsCard(controller.diagnostics)
        }

        Button(
            onClick = { if (controller.isRunning) controller.disconnect() else controller.connect() },
            modifier = Modifier
                .fillMaxWidth()
                .padding(top = 12.dp),
        ) {
            Text(if (controller.isRunning) "断开" else "连接")
        }
    }
}

@Composable
private fun PortField(value: String, onValueChange: (String) -> Unit) {
    var expanded by remember { mutableStateOf(false) }

    OutlinedTextField(
        value = value,
        onValueChange = { onValueChange(it.filter { c -> c.isDigit() }) },
        label = { Text("端口") },
        singleLine = true,
        trailingIcon = {
            IconButton(onClick = { expanded = true }) {
                Icon(Icons.Filled.ArrowDropDown, contentDescription = "端口列表")
            }
        },
        modifier = Modifier.fillMaxWidth(),
    )

    DropdownMenu(expanded = expanded, onDismissRequest = { expanded = false }) {
        presetPorts.forEach { p ->
            DropdownMenuItem(
                text = { Text(p) },
                onClick = {
                    onValueChange(p)
                    expanded = false
                },
            )
        }
    }
}

@Composable
private fun StatusCard(controller: AquaController) {
    Card(modifier = Modifier.fillMaxWidth()) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
            Text("状态：${controller.state}", style = androidx.compose.material3.MaterialTheme.typography.titleMedium)
            if (controller.lastError.isNotEmpty()) {
                Text("错误：${controller.lastError}")
            }
        }
    }
}

@Composable
private fun DiagnosticsCard(d: AquaDiagnostics?) {
    Card(modifier = Modifier.fillMaxWidth()) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
            Text("诊断", style = androidx.compose.material3.MaterialTheme.typography.titleMedium)
            if (d == null) {
                Text("暂无诊断数据（未进入播放态）")
                return@Column
            }
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
                Text("RTT"); Text("${d.rttMs.f1()} ms")
            }
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
                Text("抖动"); Text("${d.interarrivalJitterMs.f1()} ms")
            }
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
                Text("端到端延迟"); Text("${d.endToEndMs.f1()} ms")
            }
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
                Text("时钟漂移"); Text("${d.driftPpm.f1()} ppm")
            }
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
                Text("JB 水位"); Text("${d.jbCurrentMs.f1()} / ${d.jbCapacityMs.f1()} ms")
            }
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
                Text("RB 水位"); Text("${d.rbCurrentMs.f1()} ms")
            }
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
                Text("丢包(lost/late/dup)"); Text("${d.packetsLost.f0()} / ${d.latePackets.f0()} / ${d.duplicates.f0()}")
            }
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
                Text("欠载"); Text("${d.underruns.f0()}")
            }
        }
    }
}

private fun Double.f1(): String = String.format(Locale.US, "%.1f", this)
private fun Double.f0(): String = String.format(Locale.US, "%.0f", this)
