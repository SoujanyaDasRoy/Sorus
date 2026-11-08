package dev.sorus.spike.a1

import android.app.Activity
import android.content.Intent
import android.content.pm.PackageManager
import android.media.projection.MediaProjectionManager
import android.os.Bundle
import android.view.ViewGroup.LayoutParams.MATCH_PARENT
import android.view.ViewGroup.LayoutParams.WRAP_CONTENT
import android.widget.Button
import android.widget.CheckBox
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import rikka.shizuku.Shizuku
import kotlin.concurrent.thread

class MainActivity : Activity() {
    private lateinit var logView: TextView
    private val permissionListener = Shizuku.OnRequestPermissionResultListener { _, result ->
        if (result == PackageManager.PERMISSION_GRANTED) grantViaShizuku() else Spike.log("Shizuku permission denied")
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            // Edge-to-edge (Android 15+): keep the controls clear of the status and navigation bars.
            setOnApplyWindowInsetsListener { v, insets ->
                val bars = insets.getInsets(android.view.WindowInsets.Type.systemBars())
                v.setPadding(32, bars.top + 16, 32, bars.bottom + 16)
                insets
            }
        }
        fun button(label: String, onClick: () -> Unit) =
            root.addView(Button(this).apply { text = label; setOnClickListener { onClick() } }, MATCH_PARENT, WRAP_CONTENT)
        button("1. Grant permissions via Shizuku") { grant() }
        button("2. A1: start full mode (capture)") { Spike.blockFrames = 480; start() }
        button("A4: full mode with 5 ms blocks") { Spike.blockFrames = 240; start() }
        button("3. A1: stop") { stopService(Intent(this, CaptureService::class.java)) }
        button("A2: start session mode") {
            stopService(Intent(this, CaptureService::class.java))  // never both at once: they share sessions
            startForegroundService(Intent(this, SessionService::class.java))
        }
        button("A2: show table") { sendToSession(SessionService.ACTION_TABLE) }
        button("A2: stop") { stopService(Intent(this, SessionService::class.java)) }
        button("A3: measure EQ fit (plays tones ~9 min)") {
            stopService(Intent(this, CaptureService::class.java))
            stopService(Intent(this, SessionService::class.java))
            thread(name = "a3") { runCatching { A3Runner(applicationContext).run() }.onFailure { Spike.log("A3 failed: $it") } }
        }
        root.addView(CheckBox(this).apply {
            text = "Effect on (A1: +6 dB bass, A2: telephone EQ). Untick to hear the difference"
            isChecked = Spike.bassOn
            setOnCheckedChangeListener { _, on ->
                Spike.bassOn = on
                Spike.log("effect " + if (on) "on" else "off")
                sendToSession(SessionService.ACTION_APPLY)
            }
        })
        logView = TextView(this).apply { textSize = 12f; setTextIsSelectable(true) }
        root.addView(ScrollView(this).apply { addView(logView) }, MATCH_PARENT, MATCH_PARENT)
        setContentView(root)

        Shizuku.addRequestPermissionResultListener(permissionListener)
        Spike.onLog = { logView.text = it }
        logView.text = Spike.text()
    }

    override fun onDestroy() {
        Shizuku.removeRequestPermissionResultListener(permissionListener)
        Spike.onLog = null
        super.onDestroy()
    }

    private fun grant() {
        if (!Shizuku.pingBinder()) {
            Spike.log("Shizuku is not running: start it in the Shizuku app (wireless debugging)")
            return
        }
        if (Shizuku.checkSelfPermission() == PackageManager.PERMISSION_GRANTED) grantViaShizuku()
        else Shizuku.requestPermission(1)
    }

    private fun grantViaShizuku() = thread {
        val pkg = packageName
        for (cmd in listOf(
            "appops set $pkg PROJECT_MEDIA allow",  // skips the screen-capture consent dialog
            "pm grant $pkg android.permission.RECORD_AUDIO",
            "pm grant $pkg android.permission.POST_NOTIFICATIONS",
        )) {
            val out = runCatching { Spike.sh(cmd) }.getOrElse { "error: $it" }
            Spike.log("$cmd -> ${out.ifEmpty { "ok" }}")
        }
    }

    private fun sendToSession(action: String) {
        if (SessionService.active) startService(Intent(this, SessionService::class.java).setAction(action))
    }

    private fun start() {
        stopService(Intent(this, SessionService::class.java))
        val mpm = getSystemService(MediaProjectionManager::class.java)
        @Suppress("DEPRECATION")
        startActivityForResult(mpm.createScreenCaptureIntent(), 2)
    }

    @Deprecated("Deprecated in Java")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (requestCode != 2) return
        if (resultCode != RESULT_OK || data == null) {
            Spike.log("screen-capture permission refused")
            return
        }
        startForegroundService(Intent(this, CaptureService::class.java).putExtra("projection", data))
    }
}
