package dev.sorus.spike.a1

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.ServiceInfo
import android.media.audiofx.AudioEffect
import android.media.audiofx.DynamicsProcessing
import android.os.IBinder
import android.os.Process
import kotlin.concurrent.thread

/**
 * A2 spike (session mode): finds other apps' audio sessions two ways and attaches a DynamicsProcessing EQ to each.
 *  - broadcasts: apps that announce their session with ACTION_OPEN_AUDIO_EFFECT_CONTROL_SESSION;
 *  - dumpsys: every playing media/game/unknown session listed by `dumpsys audio` (through Shizuku).
 * The EQ is a deliberately obvious "telephone" curve (lows and highs -24 dB) so it is easy to hear; the checkbox
 * in the activity toggles it for A/B. "Show A2 table" prints one row per app.
 */
class SessionService : Service() {
    private class Row(val pkg: String) {
        var broadcast = false
        var dumpsys = false
        var attached = false
        var error: String? = null
        val types = sortedSetOf<String>()
    }

    private val effects = HashMap<Int, DynamicsProcessing>()  // session -> EQ (main thread only)
    private val rows = sortedMapOf<String, Row>()
    @Volatile private var running = false
    private var poller: Thread? = null

    private val receiver = object : BroadcastReceiver() {
        override fun onReceive(c: Context, i: Intent) {
            val session = i.getIntExtra(AudioEffect.EXTRA_AUDIO_SESSION, -1)
            val pkg = i.getStringExtra(AudioEffect.EXTRA_PACKAGE_NAME) ?: "?"
            if (i.action == AudioEffect.ACTION_OPEN_AUDIO_EFFECT_CONTROL_SESSION) {
                row(pkg).broadcast = true
                Spike.log("broadcast OPEN: $pkg session $session")
                attach(session, pkg)
            } else {
                Spike.log("broadcast CLOSE: $pkg session $session")
                effects.remove(session)?.release()
            }
        }
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.action == ACTION_TABLE) { printTable(); return START_NOT_STICKY }
        if (intent?.action == ACTION_APPLY) { applyToggle(); return START_NOT_STICKY }
        if (running) return START_NOT_STICKY
        val nm = getSystemService(NotificationManager::class.java)
        nm.createNotificationChannel(NotificationChannel("a2", "Session mode", NotificationManager.IMPORTANCE_LOW))
        startForeground(2, Notification.Builder(this, "a2")
            .setSmallIcon(android.R.drawable.ic_media_play)
            .setContentTitle("Sorus A2 spike: session-mode EQ")
            .build(), ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE)
        registerReceiver(receiver, IntentFilter().apply {
            addAction(AudioEffect.ACTION_OPEN_AUDIO_EFFECT_CONTROL_SESSION)
            addAction(AudioEffect.ACTION_CLOSE_AUDIO_EFFECT_CONTROL_SESSION)
        }, RECEIVER_EXPORTED)
        running = true
        active = true
        poller = thread(name = "a2-dumpsys") { pollLoop() }
        Spike.log("session mode started: EQ ${if (Spike.bassOn) "on" else "off"} (checkbox toggles it)")
        return START_NOT_STICKY
    }

    override fun onDestroy() {
        running = false
        active = false
        poller?.join(3000)
        unregisterReceiver(receiver)
        effects.values.forEach { it.release() }
        effects.clear()
        printTable()
        Spike.log("session mode stopped; EQs released")
        super.onDestroy()
    }

    private fun pollLoop() {
        val dumper = Spike.Dumper()
        while (running) {
            val players = dumper.players().filter { it.eligible && it.uid != Process.myUid() }
            android.os.Handler(mainLooper).post {
                for (p in players) {
                    val pkg = packageManager.getNameForUid(p.uid) ?: p.uid.toString()
                    row(pkg).dumpsys = true
                    row(pkg).types += p.type.substringAfterLast('.')
                    if (p.started) attach(p.session, pkg)
                }
            }
            Thread.sleep(1000)
        }
    }

    private fun row(pkg: String) = rows.getOrPut(pkg) { Row(pkg) }

    private fun attach(session: Int, pkg: String) {
        if (session <= 0 || session in effects) return
        try {
            effects[session] = telephoneEq(session)
            row(pkg).attached = true
            Spike.log("EQ attached: $pkg session $session")
        } catch (e: Exception) {
            row(pkg).error = e.message ?: e.toString()
            Spike.log("EQ attach FAILED: $pkg session $session: $e")
        }
    }

    private fun telephoneEq(session: Int): DynamicsProcessing {
        val cfg = DynamicsProcessing.Config.Builder(
            DynamicsProcessing.VARIANT_FAVOR_FREQUENCY_RESOLUTION, 2,
            true, 3,   // pre-EQ, 3 bands
            false, 0,  // no multiband compressor
            false, 0,  // no post-EQ
            false,     // no limiter
        ).build()
        val bands = listOf(300f to -24f, 3000f to 0f, 20000f to -24f)  // band cutoff (upper edge) -> gain
        for (ch in 0 until 2) {
            val eq = cfg.getPreEqByChannelIndex(ch)
            bands.forEachIndexed { i, (cutoff, gain) ->
                eq.setBand(i, DynamicsProcessing.EqBand(true, cutoff, gain))
            }
            cfg.setPreEqByChannelIndex(ch, eq)
        }
        return DynamicsProcessing(Int.MAX_VALUE, session, cfg).apply { enabled = Spike.bassOn }
    }

    private fun applyToggle() {
        effects.values.forEach { runCatching { it.enabled = Spike.bassOn } }
        Spike.log("session EQ ${if (Spike.bassOn) "on" else "off"} on ${effects.size} sessions")
    }

    private fun printTable() {
        Spike.log("A2 table: app | broadcast | dumpsys | attached | player types | error\n" +
            rows.values.joinToString("\n") {
                "${it.pkg} | ${yn(it.broadcast)} | ${yn(it.dumpsys)} | ${yn(it.attached)} | " +
                    "${it.types.joinToString("+").ifEmpty { "-" }} | ${it.error ?: "-"}"
            })
    }

    private fun yn(b: Boolean) = if (b) "yes" else "no"

    companion object {
        @Volatile var active = false  // so the activity only sends commands to a running service
        const val ACTION_TABLE = "dev.sorus.spike.a1.TABLE"
        const val ACTION_APPLY = "dev.sorus.spike.a1.APPLY"
    }
}
