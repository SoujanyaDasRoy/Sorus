package dev.sorus.spike.a1

import android.os.Handler
import android.os.Looper
import rikka.shizuku.Shizuku
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/** Shared state for the throwaway A1 spike: on-screen log, bass toggle, Shizuku shell. */
object Spike {
    @Volatile var bassOn = true

    private val main = Handler(Looper.getMainLooper())
    private val lines = ArrayDeque<String>()
    var onLog: ((String) -> Unit)? = null

    fun log(msg: String) {
        val line = SimpleDateFormat("HH:mm:ss", Locale.US).format(Date()) + "  " + msg
        android.util.Log.i("SorusA1", msg)
        main.post {
            lines.addFirst(line)
            while (lines.size > 300) lines.removeLast()
            onLog?.invoke(lines.joinToString("\n"))
        }
    }

    fun text() = lines.joinToString("\n")

    /** One `AudioPlaybackConfiguration` line of `dumpsys audio`. */
    data class Player(val uid: Int, val session: Int, val usage: String, val flags: Int, val started: Boolean,
                      val type: String) {
        /** Media/game/unknown usage on a real session (never session 0: the output mix). */
        val eligible get() = session > 0 && usage in setOf("USAGE_MEDIA", "USAGE_GAME", "USAGE_UNKNOWN")
    }

    /** Polls `dumpsys audio` through Shizuku; logs the first player line and the first failure only. */
    class Dumper {
        private var loggedSample = false
        private var failing = false
        private val uidRe = Regex("u/pid:(\\d+)/")
        private val sessionRe = Regex("session(?:Id)?:(\\d+)")
        private val usageRe = Regex("usage=(\\w+)")
        private val flagsRe = Regex("flags=0x([0-9A-Fa-f]+)")
        private val stateRe = Regex("state:(\\w+)")
        private val typeRe = Regex("type:(\\S+)")

        fun players(): List<Player> {
            val dump = runCatching { sh("dumpsys audio") }.onSuccess { failing = false }.getOrElse {
                if (!failing) log("dumpsys failed: ${it.cause ?: it} (Shizuku running and permitted?)")
                failing = true
                return emptyList()
            }
            return dump.lineSequence().filter { "AudioPlaybackConfiguration piid:" in it }.mapNotNull { line ->
                if (!loggedSample) { log("player line sample: ${line.trim()}"); loggedSample = true }
                Player(
                    uid = uidRe.find(line)?.groupValues?.get(1)?.toInt() ?: return@mapNotNull null,
                    session = sessionRe.find(line)?.groupValues?.get(1)?.toInt() ?: return@mapNotNull null,
                    usage = usageRe.find(line)?.groupValues?.get(1) ?: return@mapNotNull null,
                    flags = flagsRe.find(line)?.groupValues?.get(1)?.toInt(16) ?: 0,
                    started = stateRe.find(line)?.groupValues?.get(1) == "started",
                    type = typeRe.find(line)?.groupValues?.get(1) ?: "?",
                )
            }.toList()
        }
    }

    /** Runs a shell command as Shizuku's user (shell/adb uid) and returns stdout + stderr. */
    fun sh(cmd: String): String {
        // Shizuku 13 made newProcess private; it is still the simplest way to run a command (spike only).
        val m = Shizuku::class.java.getDeclaredMethod(
            "newProcess", Array<String>::class.java, Array<String>::class.java, String::class.java
        )
        m.isAccessible = true
        val p = m.invoke(null, arrayOf("sh", "-c", cmd), null, null) as Process
        val out = p.inputStream.bufferedReader().readText() + p.errorStream.bufferedReader().readText()
        p.waitFor()
        return out.trim()
    }
}
