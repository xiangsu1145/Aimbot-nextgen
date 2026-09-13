package io.github.xiangsu1145.aimbotnextgen.adb

import android.app.AppOpsManager
import android.app.ForegroundServiceStartNotAllowedException
import android.app.NotificationManager
import android.content.ActivityNotFoundException
import android.content.Intent
import android.os.Build
import android.os.Bundle
import android.provider.Settings
import android.util.Log
import android.view.View
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import androidx.core.view.isGone
import androidx.core.view.isVisible
import io.github.xiangsu1145.aimbotnextgen.R

class AdbPairingTutorialActivity : AppCompatActivity() {

    private var notificationEnabled = false

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.adb_pairing_tutorial)

        setSupportActionBar(findViewById(R.id.toolbar))
        supportActionBar?.setDisplayHomeAsUpEnabled(true)
        supportActionBar?.title = "ADB 无线配对"
        findViewById<com.google.android.material.appbar.MaterialToolbar>(R.id.toolbar).setNavigationOnClickListener { finish() }

        notificationEnabled = isNotificationEnabled()
        if (notificationEnabled) startPairingService()

        syncUI()

        findViewById<View>(R.id.btn_developer_options).setOnClickListener {
            try {
                val intent = Intent(Settings.ACTION_APPLICATION_DEVELOPMENT_SETTINGS).apply {
                    flags = Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_CLEAR_TASK
                    putExtra(":settings:fragment_args_key", "toggle_adb_wireless")
                }
                startActivity(intent)
            } catch (_: ActivityNotFoundException) {
                Toast.makeText(this, "未找到开发者选项", Toast.LENGTH_SHORT).show()
            }
        }

        findViewById<View>(R.id.btn_notification_settings).setOnClickListener {
            try {
                val intent = Intent(Settings.ACTION_APP_NOTIFICATION_SETTINGS).apply {
                    putExtra(Settings.EXTRA_APP_PACKAGE, packageName)
                }
                startActivity(intent)
            } catch (_: ActivityNotFoundException) {
            }
        }
    }

    override fun onResume() {
        super.onResume()
        val newEnabled = isNotificationEnabled()
        if (newEnabled != notificationEnabled) {
            notificationEnabled = newEnabled
            syncUI()
            if (newEnabled) startPairingService()
        }
    }

    private fun syncUI() {
        val stepsVisible = if (notificationEnabled) View.VISIBLE else View.GONE
        findViewById<View>(R.id.step1).isVisible = notificationEnabled
        findViewById<View>(R.id.step2).isVisible = notificationEnabled
        findViewById<View>(R.id.step3).isVisible = notificationEnabled
        findViewById<View>(R.id.card_notification).isVisible = notificationEnabled
        findViewById<View>(R.id.card_notification_disabled).isGone = notificationEnabled
    }

    private fun isNotificationEnabled(): Boolean {
        val nm = getSystemService(NotificationManager::class.java)
        val channel = nm.getNotificationChannel(AdbPairingService.CHANNEL_ID)
        return nm.areNotificationsEnabled() &&
                (channel == null || channel.importance != NotificationManager.IMPORTANCE_NONE)
    }

    private fun startPairingService() {
        val intent = Intent(this, AdbPairingService::class.java).setAction("start")
        try {
            startForegroundService(intent)
        } catch (e: Throwable) {
            Log.e("AdbPairingTutorial", "startForegroundService failed", e)
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S && e is ForegroundServiceStartNotAllowedException) {
                startService(intent)
            }
        }
    }
}
