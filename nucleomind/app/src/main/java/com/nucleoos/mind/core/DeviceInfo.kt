package com.nucleoos.mind.core

import android.app.ActivityManager
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.os.BatteryManager
import android.os.Build
import android.os.PowerManager

/** Lettura risorse del dispositivo per UI, raccomandazioni e guard. */
object DeviceInfo {

    data class Sample(
        val ramTotalMb: Int,
        val ramAvailMb: Int,
        val batteryPct: Int,
        val charging: Boolean,
        val thermal: String,
        val thermalSevere: Boolean,
    )

    fun sample(context: Context): Sample {
        val am = context.getSystemService(Context.ACTIVITY_SERVICE) as ActivityManager
        val mi = ActivityManager.MemoryInfo()
        am.getMemoryInfo(mi)
        val totalMb = (mi.totalMem / (1024 * 1024)).toInt()
        val availMb = (mi.availMem / (1024 * 1024)).toInt()

        val batt = context.registerReceiver(null, IntentFilter(Intent.ACTION_BATTERY_CHANGED))
        val level = batt?.getIntExtra(BatteryManager.EXTRA_LEVEL, -1) ?: -1
        val scale = batt?.getIntExtra(BatteryManager.EXTRA_SCALE, -1) ?: -1
        val pct = if (level >= 0 && scale > 0) (level * 100) / scale else -1
        val status = batt?.getIntExtra(BatteryManager.EXTRA_STATUS, -1) ?: -1
        val charging = status == BatteryManager.BATTERY_STATUS_CHARGING ||
            status == BatteryManager.BATTERY_STATUS_FULL

        var thermalName = "—"
        var severe = false
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            val pm = context.getSystemService(Context.POWER_SERVICE) as PowerManager
            val st = pm.currentThermalStatus
            thermalName = thermalLabel(st)
            severe = st >= PowerManager.THERMAL_STATUS_SEVERE
        }
        return Sample(totalMb, availMb, pct, charging, thermalName, severe)
    }

    private fun thermalLabel(status: Int): String = when (status) {
        PowerManager.THERMAL_STATUS_NONE -> "ok"
        PowerManager.THERMAL_STATUS_LIGHT -> "lieve"
        PowerManager.THERMAL_STATUS_MODERATE -> "moderato"
        PowerManager.THERMAL_STATUS_SEVERE -> "severo"
        PowerManager.THERMAL_STATUS_CRITICAL -> "critico"
        PowerManager.THERMAL_STATUS_EMERGENCY -> "emergenza"
        PowerManager.THERMAL_STATUS_SHUTDOWN -> "shutdown"
        else -> "—"
    }
}
