package io.github.xiangsu1145.aimbotnextgen.ui

import android.content.Intent
import android.net.Uri
import android.provider.Settings
import android.widget.LinearLayout
import android.widget.TextView
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import io.github.xiangsu1145.aimbotnextgen.adb.AdbPairingTutorialActivity
import io.github.xiangsu1145.aimbotnextgen.ui.theme.AimbotColors

/**
 * Modal dialogs + system-settings navigation used by the main screen. Kept out of
 * the Activity so the Activity only owns view construction and wiring.
 */
object AppDialogs {

    /** One-time explanation of how the shell privilege is obtained. */
    fun showShellInfo(activity: AppCompatActivity) {
        MaterialAlertDialogBuilder(activity)
            .setTitle("Shell 权限说明")
            .setMessage(
                "本应用通过无线调试 (ADB) 获取 Shell 权限 (UID 2000)。\n\n" +
                    "工作原理：\n" +
                    "1. 通过 mDNS 发现设备的 ADB 无线调试端口\n" +
                    "2. 使用 RSA 密钥进行 ADB 认证\n" +
                    "3. 通过 ADB 执行 shell 命令启动服务\n" +
                    "4. Shell 服务运行在 UID 2000 下，可授予应用系统权限\n\n" +
                    "配对说明：\n" +
                    "在设备的「设置 > 开发者选项 > 无线调试」中：\n" +
                    "1. 开启无线调试\n" +
                    "2. 点击「使用配对码配对设备」\n" +
                    "3. 输入6位配对码和端口号\n\n" +
                    "注意：每次重启手机后需要重新连接。"
            )
            .setPositiveButton("打开开发者选项") { _, _ ->
                try {
                    activity.startActivity(Intent(Settings.ACTION_APPLICATION_DEVELOPMENT_SETTINGS))
                } catch (_: Exception) {
                    Toast.makeText(activity, "未找到开发者选项", Toast.LENGTH_SHORT).show()
                }
            }
            .setNegativeButton("关闭", null)
            .show()
    }

    /** Shown once every time the app is opened. Free-software notice + repo link. */
    fun showUsageNotice(activity: AppCompatActivity) {
        MaterialAlertDialogBuilder(activity)
            .setTitle("使用须知")
            .setMessage(
                "欢迎使用 Aimbot-Nextgen。\n\n" +
                    "1. 本软件完全免费、开源 (AGPLv3)，任何人不得拿它收费。\n\n" +
                    "2. 如果你是花钱购买的，那你被骗了：请立即退款，并到官方仓库获取最新免费版本。\n\n" +
                    "3. 唯一官方仓库 / 更新地址：\n" +
                    "https://github.com/xiangsu1145/Aimbot-nextgen\n\n" +
                    "4. 非官方渠道的安装包可能被植入恶意代码，请只从上方仓库下载。"
            )
            .setPositiveButton("前往仓库") { _, _ ->
                try {
                    activity.startActivity(
                        Intent(
                            Intent.ACTION_VIEW,
                            Uri.parse("https://github.com/xiangsu1145/Aimbot-nextgen")
                        )
                    )
                } catch (_: Exception) {
                    Toast.makeText(activity, "无法打开浏览器", Toast.LENGTH_SHORT).show()
                }
            }
            .setNegativeButton("知道了", null)
            .show()
    }

    /**
     * Status card shown when the user taps 启动 before the device is paired.
     *
     * There is no overlay permission to explain any more — the menu is a layer the
     * shell daemon builds on SurfaceFlinger, so it needs no app-side grant — and the
     * only thing that can block the launch is the missing shell. [shellLabel] and
     * [menuLabel] are passed in so the dialog always shows the live state.
     */
    fun showPermissionHelp(
        activity: AppCompatActivity,
        shellLabel: String,
        menuLabel: String
    ) {
        val layout = LinearLayout(activity).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(activity.dp(24), activity.dp(16), activity.dp(24), activity.dp(8))

            addView(TextView(activity).apply {
                text = "Shell"
                textSize = 15f
                setTextColor(AimbotColors.ON_SURFACE)
            })
            addView(TextView(activity).apply {
                text = shellLabel
                textSize = 13f
                setTextColor(AimbotColors.ON_SURFACE_VARIANT)
                setPadding(0, activity.dp(4), 0, activity.dp(8))
            })

            addView(TextView(activity).apply {
                text = "Menu"
                textSize = 15f
                setTextColor(AimbotColors.ON_SURFACE)
                setPadding(0, activity.dp(8), 0, 0)
            })
            addView(TextView(activity).apply {
                text = menuLabel
                textSize = 13f
                setTextColor(AimbotColors.ON_SURFACE_VARIANT)
                setPadding(0, activity.dp(4), 0, 0)
            })
        }

        MaterialAlertDialogBuilder(activity)
            .setTitle("权限说明")
            .setView(layout)
            .setPositiveButton("前往配对") { _, _ ->
                activity.startActivity(
                    Intent(activity, AdbPairingTutorialActivity::class.java)
                )
            }
            .setNegativeButton("关闭", null)
            .show()
    }
}
