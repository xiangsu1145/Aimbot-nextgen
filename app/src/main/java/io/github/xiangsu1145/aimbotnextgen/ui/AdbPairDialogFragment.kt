package io.github.xiangsu1145.aimbotnextgen.ui

import android.app.Dialog
import android.content.Intent
import android.os.Bundle
import android.provider.Settings
import android.util.Log
import android.view.View
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.TextView
import androidx.appcompat.app.AlertDialog
import androidx.fragment.app.DialogFragment
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import io.github.xiangsu1145.aimbotnextgen.R
import io.github.xiangsu1145.aimbotnextgen.adb.*
import io.github.xiangsu1145.aimbotnextgen.ui.theme.AimbotColors
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch

class AdbPairDialogFragment : DialogFragment() {

    companion object {
        private const val TAG = "AdbPairDialog"
        fun newInstance(): AdbPairDialogFragment = AdbPairDialogFragment()
    }

    private var adbMdns: AdbMdns? = null
    private var discoveredPort = -1
    private var pairing = false

    override fun onCreateDialog(savedInstanceState: Bundle?): Dialog {
        val context = requireContext()
        val view = layoutInflater.inflate(R.layout.adb_pair_dialog, null)

        val textDiscovery = view.findViewById<TextView>(R.id.text_discovery)
        val inputContainer = view.findViewById<LinearLayout>(R.id.input_container)
        val portEdit = view.findViewById<EditText>(R.id.port)
        val codeEdit = view.findViewById<EditText>(R.id.pairing_code)

        val builder = MaterialAlertDialogBuilder(context).apply {
            setTitle("ADB 无线配对")
            setView(view)
            setPositiveButton("配对", null)
            setNegativeButton("取消", null)
            setNeutralButton("开发者选项", null)
        }
        val dialog = builder.create()
        dialog.setCanceledOnTouchOutside(false)

        adbMdns = AdbMdns(context, AdbMdns.TLS_PAIRING, onPortFound = { port ->
            activity?.runOnUiThread {
                discoveredPort = port
                textDiscovery.visibility = View.GONE
                inputContainer.visibility = View.VISIBLE
                portEdit.setText(port.toString())
                dialog.getButton(AlertDialog.BUTTON_POSITIVE).visibility = View.VISIBLE
                dialog.getButton(AlertDialog.BUTTON_NEUTRAL).visibility = View.GONE
                dialog.setTitle("ADB 无线配对")
            }
        })
        adbMdns?.start()

        dialog.setOnShowListener {
            dialog.getButton(AlertDialog.BUTTON_POSITIVE).visibility = View.GONE
            dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener {
                val port = portEdit.text.toString().toIntOrNull() ?: -1
                if (port < 1 || port > 65535) {
                    portEdit.error = "端口号无效"
                    return@setOnClickListener
                }
                val code = codeEdit.text.toString().trim()
                if (code.isEmpty()) {
                    codeEdit.error = "请输入配对码"
                    return@setOnClickListener
                }
                doPair(code, port)
                dialog.getButton(AlertDialog.BUTTON_POSITIVE).isEnabled = false
            }
            dialog.getButton(AlertDialog.BUTTON_NEUTRAL).setOnClickListener {
                try {
                    val intent = Intent(Settings.ACTION_APPLICATION_DEVELOPMENT_SETTINGS).apply {
                        flags = Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_CLEAR_TASK
                        putExtra(":settings:fragment_args_key", "toggle_adb_wireless")
                    }
                    startActivity(intent)
                } catch (_: Exception) {}
            }
        }

        return dialog
    }

    private fun doPair(code: String, port: Int) {
        val context = context ?: return
        if (pairing) return
        pairing = true

        // Give visible feedback: without this the dialog looks frozen from the
        // moment the button is pressed until the result arrives.
        val alert = dialog as? AlertDialog
        alert?.getButton(AlertDialog.BUTTON_POSITIVE)?.apply {
            isEnabled = false
            text = "配对中..."
        }
        alert?.getButton(AlertDialog.BUTTON_NEGATIVE)?.isEnabled = false

        Thread {
            Log.i(TAG, "Starting pairing on 127.0.0.1:$port")
            try {
                val key = AdbKey(context)
                val client = AdbPairingClient("127.0.0.1", port, code, key)
                val success = client.start()
                client.close()
                Log.i(TAG, "Pairing returned success=$success")
                AdbPairingState.setPaired(context, success)
                context.sendBroadcast(
                    Intent(AdbPairingState.ACTION_PAIRING_RESULT)
                        .setPackage(context.packageName)
                        .putExtra(AdbPairingState.EXTRA_SUCCESS, success)
                )
                activity?.runOnUiThread {
                    pairing = false
                    if (success) {
                        MaterialAlertDialogBuilder(context)
                            .setTitle("配对成功")
                            .setMessage("ADB 无线配对已完成，返回主界面启动 Shell 服务。")
                            .setPositiveButton("确定") { _, _ -> dismiss() }
                            .show()
                    } else {
                        MaterialAlertDialogBuilder(context)
                            .setTitle("配对失败")
                            .setMessage("adbd 拒绝了本次配对。")
                            .setPositiveButton("确定", null)
                            .show()
                    }
                }
            } catch (e: Exception) {
                Log.e(TAG, "Pairing failed", e)
                activity?.runOnUiThread {
                    pairing = false
                    alert?.getButton(AlertDialog.BUTTON_POSITIVE)?.apply { isEnabled = true; text = "配对" }
                    alert?.getButton(AlertDialog.BUTTON_NEGATIVE)?.isEnabled = true
                    val msg = when (e) {
                        is AdbInvalidPairingCodeException -> "配对码错误"
                        is AdbKeyException -> "密钥错误: ${e.message}"
                        else -> e.message ?: "配对失败"
                    }
                    MaterialAlertDialogBuilder(context)
                        .setTitle("配对失败")
                        .setMessage(msg)
                        .setPositiveButton("确定", null)
                        .show()
                }
            }
        }.start()
    }

    override fun onDestroyView() {
        super.onDestroyView()
        adbMdns?.stop()
    }
}