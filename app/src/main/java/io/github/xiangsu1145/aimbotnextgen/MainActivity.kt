package io.github.xiangsu1145.aimbotnextgen

import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.ServiceConnection
import android.graphics.Color
import android.graphics.Typeface
import android.net.Uri
import android.os.Bundle
import android.os.IBinder
import android.os.PowerManager
import android.provider.Settings
import android.util.Log
import android.view.Gravity
import android.view.View
import android.view.animation.AccelerateDecelerateInterpolator
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.TextView
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.updatePadding
import androidx.core.widget.NestedScrollView
import androidx.recyclerview.widget.LinearLayoutManager
import androidx.recyclerview.widget.RecyclerView
import com.google.android.material.appbar.MaterialToolbar
import com.google.android.material.bottomnavigation.BottomNavigationView
import com.google.android.material.button.MaterialButton
import com.google.android.material.card.MaterialCardView
import io.github.xiangsu1145.aimbotnextgen.adb.AdbPairingState
import io.github.xiangsu1145.aimbotnextgen.adb.AdbPairingTutorialActivity
import io.github.xiangsu1145.aimbotnextgen.overlay.ImguiController
import io.github.xiangsu1145.aimbotnextgen.shell.ShellController
import io.github.xiangsu1145.aimbotnextgen.shell.RequestRelay
import io.github.xiangsu1145.aimbotnextgen.shell.ShellDaemonService
import io.github.xiangsu1145.aimbotnextgen.shell.ShellManager
import io.github.xiangsu1145.aimbotnextgen.shell.ShellOutputAdapter
import io.github.xiangsu1145.aimbotnextgen.ui.AppDialogs
import io.github.xiangsu1145.aimbotnextgen.ui.dp
import io.github.xiangsu1145.aimbotnextgen.ui.gap
import io.github.xiangsu1145.aimbotnextgen.ui.matchParent
import io.github.xiangsu1145.aimbotnextgen.ui.matchParentWrapContent
import io.github.xiangsu1145.aimbotnextgen.ui.menuStatus
import io.github.xiangsu1145.aimbotnextgen.ui.shellStatusLabel
import io.github.xiangsu1145.aimbotnextgen.ui.spacer
import io.github.xiangsu1145.aimbotnextgen.ui.theme.AimbotColors
import io.github.xiangsu1145.aimbotnextgen.ui.wrapContent

/**
 * Main screen — view construction + wiring only.
 *
 * Shell orchestration lives in [ShellController], the ImGui overlay lifecycle in
 * [ImguiController], dialogs / permission navigation in [AppDialogs], and shared
 * view helpers in `ui/UiKit.kt` + `ui/StatusText.kt`.
 */
class MainActivity : AppCompatActivity() {

    private enum class Screen { MAIN, SETTINGS }

    companion object {
        /** Set by AdbPairingService on a successful pairing result; tells MainActivity
         *  to launch the Shell server when the user taps the "前往启动 Shell 服务" notice. */
        const val EXTRA_START_SHELL = "io.github.xiangsu1145.aimbotnextgen.start_shell"
    }

    // ── Views ─────────────────────────────────────────────────────────────
    private var rootLayout: LinearLayout? = null
    private var toolbar: MaterialToolbar? = null
    private var contentContainer: FrameLayout? = null
    private var statusText: TextView? = null
    private var startButton: MaterialButton? = null

    private var shellBtnRow: LinearLayout? = null
    private var shellStatusText: TextView? = null
    private var shellStatusIndicator: View? = null
    private var shellStartButton: MaterialButton? = null
    private var shellPairButton: MaterialButton? = null
    private var shellOutputAdapter: ShellOutputAdapter? = null
    private var shellOutputRecycler: RecyclerView? = null

    /** Dynamic value labels in the status card. */
    private var shellRowValue: TextView? = null
    private var menuRowValue: TextView? = null

    // ── State ─────────────────────────────────────────────────────────────
    /** Whether ADB pairing has completed. Gates the "启动" (start shell) button. */
    private var paired = false
    private var startAnimating = false
    /** Set while we are waiting for the shell service to come up so we can finish
     *  launching the menu automatically. */
    private var pendingLaunch = false

    // ── Controllers ───────────────────────────────────────────────────────
    private lateinit var shell: ShellController
    private lateinit var imgui: ImguiController

    // ── Service binding ────────────────────────────────────────────────────
    private var daemonService: ShellDaemonService? = null
    private var serviceBound = false

    /**
     * Wires the [ShellController] proxy to the [ShellManager] hosted by
     * [ShellDaemonService]. The same callback fires on every rebind across an
     * Activity recreation, so the ShellController listener swaps cleanly each
     * time we come back to the foreground.
     */
    private val serviceConnection = object : ServiceConnection {
        override fun onServiceConnected(name: ComponentName?, binder: IBinder?) {
            val local = binder as? ShellDaemonService.LocalBinder ?: return
            daemonService = local.getService()
            shell.bind(local.getManager())
        }

        override fun onServiceDisconnected(name: ComponentName?) {
            // The system killed the service; the next bind will reconnect.
            daemonService = null
            serviceBound = false
        }
    }

    /** Holds the battery-optimization warning card so it can be removed/added
     *  on rebind. Set in [buildMainScreen], removed in [refreshBatteryCard]. */
    private var batteryCardHost: LinearLayout? = null
    private var batteryCardView: MaterialCardView? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        shell = ShellController.get(this)
        imgui = ImguiController.get(this)
        paired = AdbPairingState.isPaired(this)
        setContentView(buildRoot())
        setupShellCallbacks()
        setupImguiCallbacks()
        // Re-attached to a possibly still-running shell: render its current state so
        // the UI matches the device after an Activity recreation (rotation, config change).
        applyShellState(shell.state, shell.lastMessage)

        ViewCompat.setOnApplyWindowInsetsListener(findViewById(android.R.id.content)) { v, insets ->
            val bars = insets.getInsets(WindowInsetsCompat.Type.systemBars())
            v.updatePadding(left = bars.left, right = bars.right)
            toolbar?.setPadding(
                toolbar?.paddingLeft ?: 0,
                bars.top,
                toolbar?.paddingRight ?: 0,
                toolbar?.paddingBottom ?: 0
            )
            insets
        }

        handleStartShellIntent(intent)

        // Usage notice on every fresh app open (not on rotation / recreation).
        if (savedInstanceState == null) AppDialogs.showUsageNotice(this)
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        handleStartShellIntent(intent)
    }

    private fun handleStartShellIntent(intent: Intent?) {
        if (intent?.getBooleanExtra(EXTRA_START_SHELL, false) == true) {
            Log.i("MainActivity", "EXTRA_START_SHELL received, starting shell if needed")
            shell.startIfIdle()
        }
    }

    override fun onStart() {
        super.onStart()
        // Bind to the daemon service so the ShellController has a manager to
        // talk to. BIND_AUTO_CREATE brings the service up if it is not already
        // running, which is what we want when the user reopens the app after
        // the system killed our process.
        bindService(
            Intent(this, ShellDaemonService::class.java),
            serviceConnection,
            Context.BIND_AUTO_CREATE
        )
        serviceBound = true
    }

    override fun onStop() {
        if (serviceBound) {
            runCatching { unbindService(serviceConnection) }
            serviceBound = false
        }
        super.onStop()
    }

    override fun onResume() {
        super.onResume()
        // Detect the not-paired -> paired transition (covers every path that
        // completes pairing: the "前往启动 Shell 服务" notice, returning from the
        // tutorial activity, or the pairing broadcast) and play the reveal anim.
        val nowPaired = AdbPairingState.isPaired(this)
        if (nowPaired != paired) {
            paired = nowPaired
            if (paired) playPairingRevealAnimation() else hideStartButton()
        }
        refreshStatusRows()
        // The battery-optimisation warning only matters while the user is on
        // this screen, but the user's decision (in Settings) can flip at any
        // time — checking on every resume catches the "I just granted it" case
        // without us having to listen for the Settings app's result broadcast.
        refreshBatteryCard()
        // The file-picker / IME relay is retired. Drop any leftover JSON or
        // response file on disk so an old session's residual request can't
        // re-launch something on the next start.
        RequestRelay.clearStaleRequests()
        RequestRelay.clearStaleResponses()
    }

    override fun onDestroy() {
        super.onDestroy()
        // The controllers are process-wide and keep running; only detach the views
        // so a recreated Activity does not leak this (dead) listener.
        shell.detach()
        imgui.detach()
    }

    // ── Root ──────────────────────────────────────────────────────────────

    private fun buildRoot(): View {
        return LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundColor(AimbotColors.SURFACE)
            rootLayout = this

            toolbar = buildToolbar()
            addView(toolbar)

            contentContainer = FrameLayout(this@MainActivity).apply {
                layoutParams = LinearLayout.LayoutParams(matchParent(), 0, 1f)
            }
            addView(contentContainer)

            addView(buildBottomNav())
            showScreen(Screen.MAIN)
        }
    }

    private fun buildToolbar(): MaterialToolbar {
        return MaterialToolbar(this).apply {
            title = "Aimbot-Nextgen"
            setTitleTextColor(AimbotColors.ON_SURFACE)
            setBackgroundColor(AimbotColors.SURFACE)
            elevation = 0f
            visibility = View.VISIBLE
        }
    }

    private fun buildBottomNav(): BottomNavigationView {
        return BottomNavigationView(this).apply {
            backgroundTintList = android.content.res.ColorStateList.valueOf(AimbotColors.SURFACE)
            val tint = android.content.res.ColorStateList.valueOf(AimbotColors.ON_SURFACE_VARIANT)
            itemIconTintList = tint
            setItemTextColor(tint)
            menu.clear()
            menu.add(0, Screen.MAIN.ordinal, 0, "主页")
                .setIcon(android.R.drawable.ic_menu_info_details)
            menu.add(0, Screen.SETTINGS.ordinal, 1, "设置")
                .setIcon(android.R.drawable.ic_menu_preferences)
            selectedItemId = Screen.MAIN.ordinal
            setOnItemSelectedListener {
                showScreen(
                    when (it.itemId) {
                        Screen.SETTINGS.ordinal -> Screen.SETTINGS
                        else -> Screen.MAIN
                    }
                ); true
            }
        }
    }

    private fun showScreen(screen: Screen) {
        toolbar?.visibility = if (screen == Screen.MAIN) View.VISIBLE else View.GONE
        contentContainer?.let {
            it.removeAllViews()
            it.addView(
                when (screen) {
                    Screen.MAIN -> buildMainScreen()
                    Screen.SETTINGS -> buildSettingsScreen()
                },
                matchParent(), matchParent()
            )
        }
    }

    // ── Main Screen ───────────────────────────────────────────────────────

    private fun buildMainScreen(): View {
        return NestedScrollView(this).apply {
            isFillViewport = true
            ViewCompat.setOnApplyWindowInsetsListener(this) { v, insets ->
                v.updatePadding(
                    bottom = insets.getInsets(WindowInsetsCompat.Type.navigationBars()).bottom
                ); insets
            }
            addView(LinearLayout(this@MainActivity).apply {
                orientation = LinearLayout.VERTICAL
                setPadding(dp(20), dp(8), dp(20), dp(20))
                gravity = Gravity.CENTER_HORIZONTAL

                addView(buildStatusCard())
                addView(gap(20))
                addView(buildShellPermissionCard())
                // Battery-optimisation warning. Empty LinearLayout that
                // refreshBatteryCard() populates only when the user has NOT
                // yet whitelisted the app from Settings; otherwise it stays
                // empty and the card disappears from the layout entirely.
                batteryCardHost = LinearLayout(this@MainActivity).apply {
                    orientation = LinearLayout.VERTICAL
                    layoutParams = matchParentWrapContent()
                }
                addView(batteryCardHost)
                addView(gap(28))
                addView(buildLaunchButton())
            })
        }
    }

    private fun buildStatusCard(): MaterialCardView {
        val card = MaterialCardView(this).apply {
            setCardBackgroundColor(AimbotColors.SURFACE_CONTAINER)
            radius = dp(20).toFloat()
            cardElevation = 0f
            strokeWidth = 0
            layoutParams = matchParentWrapContent()
        }

        val inner = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(24), dp(24), dp(24), dp(20))
        }

        statusText = TextView(this).apply {
            text = "待机中"
            textSize = 26f
            setTextColor(AimbotColors.ON_SURFACE)
            typeface = Typeface.DEFAULT_BOLD
        }
        inner.addView(statusText)

        inner.addView(gap(16))

        inner.addView(buildInfoRow("Version", "1.0"))
        inner.addView(gap(6))
        // "Privilege" -> "Shell". The value reflects the live shell state and is
        // captured as shellRowValue inside buildInfoRow.
        inner.addView(buildInfoRow("Shell", shellLabel()))
        inner.addView(gap(6))
        inner.addView(buildInfoRow("Menu", menuLabel()))

        card.addView(inner)
        card.setOnClickListener { AppDialogs.showShellInfo(this) }
        return card
    }

    // ── Shell Permission Card ─────────────────────────────────────────────

    private fun buildShellPermissionCard(): MaterialCardView {
        val card = MaterialCardView(this).apply {
            setCardBackgroundColor(AimbotColors.SHELL_CARD_BG)
            radius = dp(20).toFloat()
            cardElevation = 0f
            strokeWidth = 0
            layoutParams = matchParentWrapContent()
        }

        val outer = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(20), dp(20), dp(20), dp(16))
        }

        // ── Header row ──
        val headerRow = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
        }

        shellStatusIndicator = View(this).apply {
            layoutParams = LinearLayout.LayoutParams(dp(10), dp(10))
            background = ContextCompat.getDrawable(this@MainActivity, android.R.drawable.presence_online)
            background?.setTint(AimbotColors.SHELL_STATUS_IDLE)
        }
        headerRow.addView(shellStatusIndicator)
        headerRow.addView(spacer(dp(10)))

        shellStatusText = TextView(this).apply {
            text = "Shell 服务: 待机中"
            textSize = 18f
            setTextColor(AimbotColors.ON_SURFACE)
            typeface = Typeface.DEFAULT_BOLD
            layoutParams = LinearLayout.LayoutParams(0, wrapContent(), 1f)
        }
        headerRow.addView(shellStatusText)
        outer.addView(headerRow)

        outer.addView(gap(12))

        // ── Description ──
        outer.addView(TextView(this).apply {
            text = "通过无线调试获取 Shell 权限 (UID 2000)。\n启动后可用于授予应用系统权限。"
            textSize = 13f
            setTextColor(AimbotColors.ON_SURFACE_VARIANT)
            setLineSpacing(dp(4).toFloat(), 1f)
        })

        outer.addView(gap(12))

        // ── Buttons row ──
        val btnRow = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
        }

        shellStartButton = MaterialButton(this).apply {
            text = "启动"
            textSize = 13f
            typeface = Typeface.create("sans-serif-medium", Typeface.NORMAL)
            setTextColor(AimbotColors.ON_PRIMARY_CONTAINER)
            setBackgroundColor(AimbotColors.SHELL_BTN_START)
            cornerRadius = dp(20)
            minimumHeight = dp(36)
            minWidth = dp(80)
            isAllCaps = false
            setPadding(dp(16), 0, dp(16), 0)
            // Before pairing completes the start button is hidden (alpha 0) and
            // cannot be used; pairing success reveals it with a fade-in animation.
            visibility = if (paired) View.VISIBLE else View.GONE
            alpha = if (paired) 1f else 0f
            isEnabled = paired
            setOnClickListener { shell.startOrStop() }
        }
        btnRow.addView(shellStartButton)
        shellBtnRow = btnRow
        btnRow.addView(spacer(dp(8)))

        shellPairButton = MaterialButton(this).apply {
            text = "配对"
            textSize = 13f
            typeface = Typeface.create("sans-serif-medium", Typeface.NORMAL)
            setTextColor(Color.parseColor("#5D4037"))
            setBackgroundColor(AimbotColors.SHELL_BTN_PAIR)
            cornerRadius = dp(20)
            minimumHeight = dp(36)
            minWidth = dp(80)
            isAllCaps = false
            setPadding(dp(16), 0, dp(16), 0)
            visibility = View.VISIBLE
            setOnClickListener {
                startActivity(Intent(this@MainActivity, AdbPairingTutorialActivity::class.java))
            }
        }
        btnRow.addView(shellPairButton)

        btnRow.addView(View(this).apply {
            layoutParams = LinearLayout.LayoutParams(0, 1, 1f)
        })

        val infoBtn = MaterialButton(this).apply {
            text = "?"
            textSize = 13f
            typeface = Typeface.DEFAULT_BOLD
            setTextColor(AimbotColors.PRIMARY)
            setBackgroundColor(Color.TRANSPARENT)
            cornerRadius = dp(18)
            minimumHeight = dp(36)
            minimumWidth = dp(36)
            setPadding(0, 0, 0, 0)
            setOnClickListener { AppDialogs.showShellInfo(this@MainActivity) }
        }
        btnRow.addView(infoBtn)

        outer.addView(btnRow)
        outer.addView(gap(12))

        // ── Terminal output ──
        outer.addView(TextView(this).apply {
            text = "输出日志"
            textSize = 12f
            setTextColor(AimbotColors.ON_SURFACE_VARIANT)
            letterSpacing = 0.04f
        })
        outer.addView(gap(6))

        val termCard = MaterialCardView(this).apply {
            setCardBackgroundColor(AimbotColors.TERMINAL_BG)
            radius = dp(12).toFloat()
            cardElevation = 0f
            strokeWidth = 0
            layoutParams = matchParentWrapContent()
        }

        shellOutputAdapter = ShellOutputAdapter()
        shellOutputRecycler = RecyclerView(this@MainActivity).apply {
            layoutManager = LinearLayoutManager(this@MainActivity).apply {
                stackFromEnd = true
            }
            adapter = shellOutputAdapter
            layoutParams = LinearLayout.LayoutParams(matchParent(), dp(160))
            setPadding(dp(4), dp(8), dp(4), dp(8))
            clipToPadding = false
            overScrollMode = RecyclerView.OVER_SCROLL_NEVER
        }
        termCard.addView(shellOutputRecycler)

        outer.addView(termCard)

        card.addView(outer)
        return card
    }

    // ── Battery-optimisation warning card ─────────────────────────────────
    //
    // The shell daemon is held alive by a foreground service, but on top of
    // that we need the OS to leave us alone in deep doze too. On stock
    // Android the user has to whitelist the app explicitly from
    // Settings → Battery → Unrestricted apps, and there is no callback that
    // tells us when they did it — so we just check on every onResume and
    // collapse the card if it is no longer needed.
    //
    // The card sits right under the shell card so the user sees it the
    // moment they enable the shell: that is also when the daemon becomes
    // critical, and when an early freeze would do the most damage.

    private fun buildBatteryCard(): MaterialCardView {
        val card = MaterialCardView(this).apply {
            setCardBackgroundColor(AimbotColors.WARNING_BG)
            // A 1dp left-edge stripe so the warning reads as a warning even at
            // a glance — the colour alone can blend into the shell card above.
            setStrokeColor(AimbotColors.WARNING_BORDER)
            strokeWidth = dp(1)
            radius = dp(20).toFloat()
            cardElevation = 0f
            layoutParams = matchParentWrapContent()
        }

        val inner = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(20), dp(16), dp(20), dp(16))
        }

        inner.addView(TextView(this).apply {
            text = getString(R.string.battery_card_title)
            textSize = 15f
            setTextColor(AimbotColors.ON_WARNING)
            typeface = Typeface.DEFAULT_BOLD
        })

        inner.addView(gap(8))

        inner.addView(TextView(this).apply {
            text = getString(R.string.battery_card_message)
            textSize = 13f
            setTextColor(AimbotColors.ON_WARNING)
            setLineSpacing(dp(3).toFloat(), 1f)
        })

        inner.addView(gap(12))

        inner.addView(MaterialButton(this).apply {
            text = getString(R.string.battery_card_button)
            textSize = 13f
            typeface = Typeface.create("sans-serif-medium", Typeface.NORMAL)
            isAllCaps = false
            setTextColor(AimbotColors.ON_WARNING_BTN)
            setBackgroundColor(AimbotColors.WARNING_BTN_BG)
            cornerRadius = dp(20)
            minimumHeight = dp(36)
            minWidth = dp(96)
            setPadding(dp(16), 0, dp(16), 0)
            setOnClickListener { openBatteryWhitelistSettings() }
        })

        card.addView(inner)
        return card
    }

    /**
     * Toggles the battery warning card on or off based on the current OS state.
     * Cheap to call repeatedly: the PowerManager lookup is one IPC and we do
     * not rebuild the card unless its visibility actually changes.
     */
    private fun refreshBatteryCard() {
        val host = batteryCardHost ?: return
        val pm = getSystemService(Context.POWER_SERVICE) as? PowerManager ?: return
        val ignoring = pm.isIgnoringBatteryOptimizations(packageName)
        val hasCard = batteryCardView != null
        if (ignoring && hasCard) {
            // User just granted the permission: drop the card.
            host.removeAllViews()
            batteryCardView = null
        } else if (!ignoring && !hasCard) {
            val card = buildBatteryCard()
            batteryCardView = card
            host.removeAllViews()
            host.addView(card)
        }
    }

    /**
     * Opens the per-app "ignore battery optimizations" dialog.
     *
     * The list view (`ACTION_IGNORE_BATTERY_OPTIMIZATION_SETTINGS`) on stock
     * Android lets you tap an app and flip its toggle, but on OPPO ColorOS
     * the same list is read-only — it shows the current state but tapping
     * the row does nothing. The per-app intent
     * (`ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS` with a `package:` URI)
     * opens a system dialog with Yes/No, which works on stock, MIUI, EMUI,
     * OneUI, and modern ColorOS. We try that first; if anything throws
     * (older OEM ROMs that intercept the intent, or stripped-down images
     * without the dialog activity), we fall back to the app's details page
     * where the user can navigate manually to Battery.
     */
    private fun openBatteryWhitelistSettings() {
        val pkg = packageName
        // 1. The per-app dialog (the right entry on stock / ColorOS / MIUI).
        val direct = Intent(Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS).apply {
            data = Uri.parse("package:$pkg")
            addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        }
        if (tryStart(direct)) return
        Log.w("MainActivity", "per-app battery dialog missing, falling back to app details")
        // 2. The app's settings page — works on every ROM, but the user has
        //    to find the battery entry themselves.
        val details = Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS).apply {
            data = Uri.parse("package:$pkg")
            addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        }
        if (tryStart(details)) return
        // 3. The list view — on OPPO this is read-only but at least shows the
        //    state. Better than nothing.
        val list = Intent(Settings.ACTION_IGNORE_BATTERY_OPTIMIZATION_SETTINGS).apply {
            addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        }
        if (tryStart(list)) return
        Log.e("MainActivity", "all battery settings intents failed")
        Toast.makeText(this,
            "无法打开电池设置。请手动进入 设置 → 应用管理 → Aimbot-Nextgen → 耗电管理 → 改为\"不限制\"",
            Toast.LENGTH_LONG).show()
    }

    private fun tryStart(intent: Intent): Boolean {
        return try {
            startActivity(intent)
            Log.i("MainActivity", "startActivity ok action=${intent.action}")
            true
        } catch (e: Exception) {
            Log.w("MainActivity", "startActivity ${intent.action} failed: ${e.message}")
            false
        }
    }

    // ── Shell wiring ──────────────────────────────────────────────────────

    private fun setupShellCallbacks() {
        shell.listener = object : ShellController.Listener {
            override fun onShellState(state: ShellManager.ShellState, message: String) {
                runOnUiThread { applyShellState(state, message) }
            }

            override fun onShellOutput(line: String) {
                runOnUiThread {
                    shellOutputAdapter?.addLine(line)
                    shellOutputRecycler?.scrollToPosition((shellOutputAdapter?.itemCount ?: 1) - 1)
                }
            }
        }
    }

    /** Renders a shell state transition into the shell card + main status card. */
    private fun applyShellState(state: ShellManager.ShellState, message: String) {
        when (state) {
            ShellManager.ShellState.IDLE -> {
                shellStatusText?.text = "Shell 服务: 待机中"
                shellStatusIndicator?.background?.setTint(AimbotColors.SHELL_STATUS_IDLE)
                updateStartButton("启动", enabled = true)
                statusText?.text = "待机中"
                // The menu layer belongs to the daemon's process, so it died with
                // it — do not keep claiming the menu is up.
                imgui.reset()
            }
            ShellManager.ShellState.DISCOVERING -> {
                shellStatusText?.text = "Shell 服务: $message"
                shellStatusIndicator?.background?.setTint(AimbotColors.TERMINAL_HEADER)
                updateStartButton("停止", enabled = true)
            }
            ShellManager.ShellState.CONNECTING -> {
                shellStatusText?.text = "Shell 服务: $message"
                shellStatusIndicator?.background?.setTint(AimbotColors.TERMINAL_HEADER)
                updateStartButton("停止", enabled = false)
            }
            ShellManager.ShellState.RUNNING -> {
                shellStatusText?.text = "Shell 服务: 运行中"
                shellStatusIndicator?.background?.setTint(AimbotColors.SHELL_STATUS_RUNNING)
                updateStartButton("停止", enabled = true)
                statusText?.text = "运行中"
                // If the ImGui launch was waiting for the shell to come up, finish
                // it now (overlay permission was already granted before queuing).
                if (pendingLaunch) proceedLaunchImgui()
            }
            ShellManager.ShellState.ERROR -> {
                shellStatusText?.text = "Shell 服务: 错误"
                shellStatusIndicator?.background?.setTint(AimbotColors.SHELL_STATUS_ERROR)
                updateStartButton("重试", enabled = true)
                imgui.reset()
            }
        }
        refreshStatusRows()
    }

    /**
     * Drives the "启动/停止" button. Before pairing the button is hidden (GONE) and
     * disabled; once paired it is shown at full alpha and its enabled state follows
     * the shell lifecycle. Alpha is only forced to 1 here in steady (paired) states —
     * the pairing reveal animation owns alpha during its 320ms run.
     */
    private fun updateStartButton(text: String, enabled: Boolean) {
        shellStartButton?.apply {
            if (paired) {
                visibility = View.VISIBLE
                if (!startAnimating) alpha = 1f
                isEnabled = enabled
                this.text = text
            } else {
                visibility = View.GONE
                alpha = 0f
                isEnabled = false
            }
        }
    }

    // ── ImGui launcher ────────────────────────────────────────────────────

    private fun buildLaunchButton(): MaterialButton {
        return MaterialButton(this).apply {
            text = if (imgui.isRunning) "停止 ImGui" else "启动 ImGui"
            textSize = 16f
            typeface = Typeface.create("sans-serif-medium", Typeface.NORMAL)
            setTextColor(AimbotColors.ON_PRIMARY_CONTAINER)
            setBackgroundColor(if (imgui.isRunning) AimbotColors.STOP_BG else AimbotColors.START_BG)
            cornerRadius = dp(16)
            minimumHeight = dp(56)
            layoutParams = matchParentWrapContent()
            setOnClickListener { onLaunchClicked() }
            startButton = this
        }
    }

    private fun setupImguiCallbacks() {
        imgui.listener = object : ImguiController.Listener {
            override fun onImguiRunningChanged(running: Boolean) {
                startButton?.apply {
                    text = if (running) "停止 ImGui" else "启动 ImGui"
                    setBackgroundColor(if (running) AimbotColors.STOP_BG else AimbotColors.START_BG)
                    setTextColor(
                        if (running) Color.parseColor("#B71C1C") else AimbotColors.ON_PRIMARY_CONTAINER
                    )
                }
                Toast.makeText(
                    this@MainActivity,
                    if (running) "ImGui 悬浮层已开启" else "ImGui 悬浮层已关闭",
                    Toast.LENGTH_SHORT
                ).show()
            }
        }
    }

    /**
     * Main action button.
     * - Not paired -> show the pairing dialog (nothing can work without the shell).
     * - Paired & shell up -> ask the daemon to build the menu layer and render.
     * - Paired & shell not up -> start the shell first, then auto-continue once it
     *   is RUNNING.
     * Tapping again while the menu is up tears it down.
     *
     * No overlay-permission step any more: the menu is not a window, so there is
     * nothing for the user to grant.
     */
    private fun onLaunchClicked() {
        if (!paired) {
            showPermissionCard()
            return
        }
        if (imgui.isRunning) {
            imgui.stop()
            return
        }
        if (shell.isRunning) {
            imgui.start()
        } else {
            pendingLaunch = true
            shell.startIfIdle()
            Toast.makeText(this, "正在启动 Shell 服务...", Toast.LENGTH_SHORT).show()
        }
    }

    private fun proceedLaunchImgui() {
        pendingLaunch = false
        imgui.start()
    }

    private fun showPermissionCard() {
        AppDialogs.showPermissionHelp(this, shellLabel(), menuLabel())
    }

    // ── Settings Screen ───────────────────────────────────────────────────

    private fun buildSettingsScreen(): View {
        return FrameLayout(this)
    }

    // ── Status card ───────────────────────────────────────────────────────

    private fun buildInfoRow(label: String, value: String): LinearLayout {
        return LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL
            addView(TextView(this@MainActivity).apply {
                text = label; textSize = 13f; setTextColor(AimbotColors.ON_SURFACE_VARIANT)
                layoutParams = LinearLayout.LayoutParams(0, wrapContent(), 1f)
            })
            addView(TextView(this@MainActivity).apply {
                text = value; textSize = 13f; setTextColor(AimbotColors.ON_SURFACE)
                typeface = Typeface.create("sans-serif-medium", Typeface.NORMAL)
                gravity = Gravity.END
            }.also { rowValueView ->
                // Stash the value TextView when this is the Shell / Overlay row so
                // refreshStatusRows() can update it.
                when (label) {
                    "Shell" -> shellRowValue = rowValueView
                    "Menu" -> menuRowValue = rowValueView
                }
            })
        }
    }

    private fun shellLabel(): String = shellStatusLabel(paired, shell.state)

    private fun menuLabel(): String = menuStatus(imgui.isRunning)

    private fun refreshStatusRows() {
        shellRowValue?.text = shellLabel()
        menuRowValue?.text = menuLabel()
    }

    // ── Pairing reveal animation ──────────────────────────────────────────

    /**
     * Reveals the "启动" button after pairing succeeds: it fades in from alpha 0 to
     * 1, while the "配对" button slides from the start button's slot (its position
     * while the start button was GONE) to its final spot beside the start button.
     */
    private fun playPairingRevealAnimation() {
        val start = shellStartButton ?: return
        val pair = shellPairButton ?: return
        if (start.visibility == View.VISIBLE && start.alpha >= 1f) return // already shown

        val pairOldLeft = pair.left  // capture position before the start button takes space
        startAnimating = true
        start.visibility = View.VISIBLE
        start.alpha = 0f
        start.isEnabled = true

        // Force a layout pass so the pair button's new (shifted-right) position is
        // known, then animate it back to its previous spot and slide it in.
        (start.parent as? View)?.requestLayout()
        start.post {
            val shift = (pairOldLeft - pair.left).toFloat()
            if (shift != 0f) {
                pair.translationX = shift
                pair.animate()
                    .translationX(0f)
                    .setDuration(320)
                    .setInterpolator(AccelerateDecelerateInterpolator())
                    .start()
            }
            start.animate()
                .alpha(1f)
                .setDuration(320)
                .setInterpolator(AccelerateDecelerateInterpolator())
                .withEndAction { startAnimating = false }
                .start()
        }
    }

    /** Hides the "启动" button again, e.g. when the pairing state is reset. */
    private fun hideStartButton() {
        startAnimating = false
        shellStartButton?.apply {
            visibility = View.GONE
            alpha = 0f
            isEnabled = false
            translationX = 0f
        }
        shellPairButton?.translationX = 0f
    }
}
