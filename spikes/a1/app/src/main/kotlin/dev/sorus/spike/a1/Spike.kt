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
