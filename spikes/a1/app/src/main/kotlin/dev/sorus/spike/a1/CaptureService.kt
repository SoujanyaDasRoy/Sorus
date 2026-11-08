package dev.sorus.spike.a1

import android.app.Activity
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Intent
import android.content.pm.ServiceInfo
import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioPlaybackCaptureConfiguration
import android.media.AudioRecord
import android.media.AudioTrack
import android.media.audiofx.DynamicsProcessing
import android.media.projection.MediaProjection
import android.media.projection.MediaProjectionManager
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.os.PowerManager
import android.os.Process
import kotlin.concurrent.thread
import kotlin.math.PI
import kotlin.math.abs
import kotlin.math.cos
import kotlin.math.log10
import kotlin.math.max
import kotlin.math.pow
import kotlin.math.sin
import kotlin.math.sqrt

/**
 * A1 spike: captures other apps' media/game/unknown audio (AudioPlaybackCapture), applies a +6 dB low shelf at
 * 100 Hz and plays it back. The originals are silenced the way RootlessJamesDSP does it: a DynamicsProcessing
 * effect with -200 dB input gain on each playing session. Effects only run on the speaker path, so the
 * captured copy stays intact. Sessions are found by parsing `dumpsys audio` through Shizuku.
 */
class CaptureService : Service() {
    private var projection: MediaProjection? = null
    private var wakeLock: PowerManager.WakeLock? = null
    @Volatile private var running = false
    private var audioThread: Thread? = null
    private var muterThread: Thread? = null
    private class Mute(val effect: DynamicsProcessing, val pkg: String, val since: Long) { var missing = 0 }
    private val mutes = HashMap<Int, Mute>()  // session id -> mute (muter thread only)
    private val uncapturable = HashMap<Int, Int>()  // session -> uid: playing, but a uid-only probe hears nothing
    private val sessionUid = HashMap<Int, Int>()
    @Volatile private var mutedCount = 0
    @Volatile private var lastSignalNs = System.nanoTime()  // last capture block above -80 dBFS

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (running) return START_NOT_STICKY
        val nm = getSystemService(NotificationManager::class.java)
        nm.createNotificationChannel(NotificationChannel("a1", "Capture", NotificationManager.IMPORTANCE_LOW))
        val n = Notification.Builder(this, "a1")
            .setSmallIcon(android.R.drawable.ic_media_play)
            .setContentTitle("Sorus A1 spike is processing audio")
            .build()
        startForeground(1, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PROJECTION)

        @Suppress("DEPRECATION")
        val data = intent?.getParcelableExtra<Intent>("projection")
        val mpm = getSystemService(MediaProjectionManager::class.java)
        projection = data?.let { mpm.getMediaProjection(Activity.RESULT_OK, it) }
        val p = projection ?: run {
            Spike.log("no media projection; stopping")
            stopSelf()
            return START_NOT_STICKY
        }
        p.registerCallback(object : MediaProjection.Callback() {
            override fun onStop() {
                Spike.log("media projection stopped by the system")
                stopSelf()
            }
        }, Handler(Looper.getMainLooper()))

        wakeLock = getSystemService(PowerManager::class.java)
            .newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "sorus:a1").apply { acquire() }
        running = true
        audioThread = thread(name = "a1-audio", priority = Thread.MAX_PRIORITY) { audioLoop(p) }
        muterThread = thread(name = "a1-muter") { muterLoop() }
        Spike.log("capture started")
        return START_NOT_STICKY
    }

    override fun onDestroy() {
        running = false
        audioThread?.join(1000)
        muterThread?.join(3000)
        projection?.stop()
        wakeLock?.release()
        Spike.log("capture stopped; originals unmuted")
        super.onDestroy()
    }

    private fun audioLoop(p: MediaProjection) {
        Process.setThreadPriority(Process.THREAD_PRIORITY_URGENT_AUDIO)
        val rate = 48000
        val frames = 480  // 10 ms
        val config = AudioPlaybackCaptureConfiguration.Builder(p)
            .addMatchingUsage(AudioAttributes.USAGE_MEDIA)
            .addMatchingUsage(AudioAttributes.USAGE_GAME)
            .addMatchingUsage(AudioAttributes.USAGE_UNKNOWN)
            .excludeUid(Process.myUid())
            .build()
        val inFormat = AudioFormat.Builder().setSampleRate(rate)
            .setEncoding(AudioFormat.ENCODING_PCM_FLOAT).setChannelMask(AudioFormat.CHANNEL_IN_STEREO).build()
        val outFormat = AudioFormat.Builder().setSampleRate(rate)
            .setEncoding(AudioFormat.ENCODING_PCM_FLOAT).setChannelMask(AudioFormat.CHANNEL_OUT_STEREO).build()
        val bytes = frames * 2 * 4
        val record = try {
            AudioRecord.Builder().setAudioFormat(inFormat).setAudioPlaybackCaptureConfig(config)
                .setBufferSizeInBytes(max(bytes * 4, AudioRecord.getMinBufferSize(rate,
                    AudioFormat.CHANNEL_IN_STEREO, AudioFormat.ENCODING_PCM_FLOAT)))
                .build()
        } catch (e: Exception) {
            Spike.log("AudioRecord failed: $e (RECORD_AUDIO granted?)")
            stopSelf()
            return
        }
        val track = AudioTrack.Builder()
            .setAudioAttributes(AudioAttributes.Builder()
                .setUsage(AudioAttributes.USAGE_MEDIA)
                .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                .setAllowedCapturePolicy(AudioAttributes.ALLOW_CAPTURE_BY_NONE)
                .build())
            .setAudioFormat(outFormat)
            .setBufferSizeInBytes(max(bytes * 4, AudioTrack.getMinBufferSize(rate,
                AudioFormat.CHANNEL_OUT_STEREO, AudioFormat.ENCODING_PCM_FLOAT)))
            .setTransferMode(AudioTrack.MODE_STREAM)
            .setPerformanceMode(AudioTrack.PERFORMANCE_MODE_LOW_LATENCY)
            .build()

        val shelf = LowShelf(rate.toDouble(), 100.0, 6.0)
        val buf = FloatArray(frames * 2)
        record.startRecording()
        track.play()
        val t0 = System.nanoTime()
        var lastReport = t0
        var total = 0L
        var peak = 0f
        while (running) {
            val n = record.read(buf, 0, buf.size, AudioRecord.READ_BLOCKING)
            if (n <= 0) continue
            if (Spike.bassOn) shelf.process(buf, n) else shelf.reset()
            for (i in 0 until n) {
                buf[i] = buf[i].coerceIn(-1f, 1f)
                peak = max(peak, abs(buf[i]))
            }
            track.write(buf, 0, n, AudioTrack.WRITE_BLOCKING)
            total += n / 2
            val now = System.nanoTime()
            for (i in 0 until n) if (abs(buf[i]) > 1e-4f) { lastSignalNs = now; break }  // above -80 dBFS
            if (now - lastReport > 30_000_000_000L) {  // every 30 s: proves it survives screen-off
                val db = if (peak > 0) 20 * log10(peak) else -999f
                Spike.log("t=${(now - t0) / 1_000_000_000}s frames=$total peak=${"%.1f".format(db)} dBFS " +
                    "underruns=${track.underrunCount} muted=$mutedCount")
                lastReport = now
                peak = 0f
            }
        }
        record.stop(); record.release()
        track.stop(); track.release()
    }

    private val noProjectionFlag = 1 shl 10  // AudioAttributes.FLAG_NO_MEDIA_PROJECTION

    private fun muterLoop() {
        val dumper = Spike.Dumper()
        while (running) {
            val seen = HashSet<Int>()
            val started = HashSet<Int>()
            for (pl in dumper.players()) {
                if (pl.uid == Process.myUid() || !pl.eligible) continue
                val (uid, session, usage) = pl
                val pkg = packageManager.getNameForUid(uid) ?: uid.toString()
                if (pl.flags and noProjectionFlag != 0) {
                    if (session !in mutes) Spike.log("not muting $pkg session $session: app forbids capture")
                    continue
                }
                seen += session
                sessionUid[session] = uid
                val isStarted = pl.started
                if (isStarted) started += session
                if (session in mutes) continue
                if (session in uncapturable) {
                    // Re-check: a track gap can look like "not capturable"; mute again once its audio shows up.
                    if (!isStarted || !probe(uid)) continue
                    uncapturable.remove(session)
                    Spike.log("$pkg session $session reaches the capture again; re-muting")
                }
                mute(session, pkg, usage)
            }
            val now = System.nanoTime()
            // Capture silent while a muted session plays: either a gap in its audio or the app is not capturable
            // (e.g. Spotify via AAudio). A capture of that uid alone decides; only unmute when it hears nothing.
            if (now - lastSignalNs > 1_500_000_000L) {
                for ((s, m) in mutes.entries.toList()) {
                    if (s !in started || now - m.since < 1_500_000_000L) continue
                    val uid = sessionUid[s] ?: continue
                    if (probe(uid)) continue
                    m.effect.release()
                    mutes.remove(s)
                    uncapturable[s] = uid
                    Spike.log("NOT CAPTURABLE (for now): ${m.pkg} session $s plays but nothing reaches the capture; unmuted")
                }
            }
            // dumpsys can miss a live player for a poll: release only after 3 misses in a row.
            for ((s, m) in mutes.entries.toList()) {
                if (s in seen) { m.missing = 0; continue }
                if (++m.missing < 3) continue
                m.effect.release()
                mutes.remove(s)
                Spike.log("session $s gone; mute released")
            }
            uncapturable.keys.retainAll(seen)
            sessionUid.keys.retainAll(seen)
            mutedCount = mutes.size
            Thread.sleep(1000)
        }
        mutes.values.forEach { it.effect.release() }
        mutes.clear()
    }

    private fun mute(session: Int, pkg: String, usage: String) {
        try {
            val fx = DynamicsProcessing(Int.MAX_VALUE, session, null).apply {
                setInputGainAllChannelsTo(-200f)
                enabled = true
            }
            mutes[session] = Mute(fx, pkg, System.nanoTime())
            Spike.log("muted original: $pkg session $session ($usage)")
        } catch (e: Exception) {
            Spike.log("could not mute $pkg session $session: $e")
        }
    }

    /** Records only [uid]'s playback for 250 ms; true if anything above -80 dBFS arrives. */
    private fun probe(uid: Int): Boolean {
        val p = projection ?: return false
        val rate = 48000
        val rec = try {
            AudioRecord.Builder()
                .setAudioFormat(AudioFormat.Builder().setSampleRate(rate)
                    .setEncoding(AudioFormat.ENCODING_PCM_FLOAT).setChannelMask(AudioFormat.CHANNEL_IN_STEREO).build())
                .setAudioPlaybackCaptureConfig(AudioPlaybackCaptureConfiguration.Builder(p).addMatchingUid(uid).build())
                .setBufferSizeInBytes(rate / 4 * 2 * 4)
                .build()
        } catch (e: Exception) {
            Spike.log("probe failed for uid $uid: $e")
            return false
        }
        val buf = FloatArray(rate / 4 * 2)
        rec.startRecording()
        var heard = false
        var got = 0
        while (got < buf.size) {
            val n = rec.read(buf, 0, buf.size - got, AudioRecord.READ_BLOCKING)
            if (n <= 0) break
            got += n
            if ((0 until n).any { abs(buf[it]) > 1e-4f }) { heard = true; break }
        }
        rec.stop()
        rec.release()
        return heard
    }
}

/** RBJ low shelf (S = 1), stereo interleaved. */
private class LowShelf(fs: Double, f0: Double, gainDb: Double) {
    private val b0: Float; private val b1: Float; private val b2: Float; private val a1: Float; private val a2: Float
    private val z = FloatArray(4)  // transposed direct form II: two states per channel

    init {
        val a = 10.0.pow(gainDb / 40)
        val w = 2 * PI * f0 / fs
        val c = cos(w)
        val alpha = sin(w) / 2 * sqrt(2.0)
        val k = 2 * sqrt(a) * alpha
        val a0 = (a + 1) + (a - 1) * c + k
        b0 = (a * ((a + 1) - (a - 1) * c + k) / a0).toFloat()
        b1 = (2 * a * ((a - 1) - (a + 1) * c) / a0).toFloat()
        b2 = (a * ((a + 1) - (a - 1) * c - k) / a0).toFloat()
        a1 = (-2 * ((a - 1) + (a + 1) * c) / a0).toFloat()
        a2 = (((a + 1) + (a - 1) * c - k) / a0).toFloat()
    }

    fun reset() = z.fill(0f)

    fun process(buf: FloatArray, n: Int) {
        for (i in 0 until n) {
            val ch = (i and 1) * 2
            val x = buf[i]
            val y = b0 * x + z[ch]
            z[ch] = b1 * x - a1 * y + z[ch + 1]
            z[ch + 1] = b2 * x - a2 * y
            buf[i] = y
        }
    }
}
