package dev.sorus.spike.a1

import android.content.Context
import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioTrack
import android.media.audiofx.DynamicsProcessing
import android.media.audiofx.Visualizer
import org.json.JSONObject
import java.io.File
import kotlin.math.PI
import kotlin.math.abs
import kotlin.math.max
import kotlin.math.sin
import kotlin.math.sqrt

/**
 * A3 spike, on-device half: for every preset in a3.json (written by spikes/a3/a3fit.exe and pushed to the app's
 * external files dir), plays 1/3-octave tones through this app's own AudioTrack session with the fitted
 * DynamicsProcessing pre-EQ, and measures each tone's RMS with a Visualizer on the same session, behind the EQ. Response = level with the EQ minus level with the EQ disabled; compared with the target at
 * the same frequencies. Results go to the log and to a3_results.txt next to a3.json.
 */
class A3Runner(private val context: Context) {
    private val rate = 48000

    fun run() {
        synchronized(A3Runner) {
            if (busy) { Spike.log("A3 already running"); return }
            busy = true
        }
        try { measure() } finally { busy = false }
    }

    companion object { @Volatile var busy = false }

    private fun measure() {
        val dir = context.getExternalFilesDir(null)!!
        val input = File(dir, "a3.json")
        if (!input.exists()) { Spike.log("A3: push a3.json to ${input.path} first"); return }
        val cfg = JSONObject(input.readText())
        val n = cfg.getInt("bands")
        val cutoffs = cfg.getJSONArray("cutoffs").let { a -> FloatArray(a.length()) { a.getDouble(it).toFloat() } }
        val freqs = cfg.getJSONArray("freqs").let { a -> DoubleArray(a.length()) { a.getDouble(it) } }
        val presets = cfg.getJSONArray("presets")
        val frameMs = cfg.optDouble("frame_ms", 0.0).toFloat()
        val out = StringBuilder("A3 on-device: $n bands, ${cfg.getString("method")} fit, frame ${frameMs} ms, " +
            "interleave ${cfg.optBoolean("interleave", true)}\n")

        val track = AudioTrack.Builder()
            .setAudioAttributes(AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_MEDIA)
                .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC).build())
            .setAudioFormat(AudioFormat.Builder().setSampleRate(rate).setEncoding(AudioFormat.ENCODING_PCM_FLOAT)
                .setChannelMask(AudioFormat.CHANNEL_OUT_STEREO).build())
            .setTransferMode(AudioTrack.MODE_STREAM)
            .setBufferSizeInBytes(rate / 10 * 2 * 4)
            .build()
        val builder = DynamicsProcessing.Config.Builder(
            DynamicsProcessing.VARIANT_FAVOR_FREQUENCY_RESOLUTION, 2, true, n, false, 0, false, 0, false)
        if (frameMs > 0) builder.setPreferredFrameDuration(frameMs)
        val dp = DynamicsProcessing(Int.MAX_VALUE, track.audioSessionId, builder.build())
        // On the output mix: a Visualizer on the app's own session sits before the EQ. The mix also carries the
        // vendor's speaker tuning (Samsung SoundAlive), so every tone is measured with the EQ off and on back to back.
        val vis = Visualizer(0).apply {
            measurementMode = Visualizer.MEASUREMENT_MODE_PEAK_RMS
            scalingMode = Visualizer.SCALING_MODE_AS_PLAYED
            enabled = true
        }
        track.play()
        // "interleave": false = one EQ-off reference sweep, then each preset with the EQ left on (settled 2 s).
        // Needed where the output mix is clean (emulator): toggling the EQ per tone catches its gain glide.
        val interleave = cfg.optBoolean("interleave", true)
        val ref = if (interleave) null else { dp.enabled = false; DoubleArray(freqs.size) { tone(track, vis, freqs[it]) } }
        try {
            var worst = 0.0
            for (p in 0 until presets.length()) {
                val pr = presets.getJSONObject(p)
                val gains = pr.getJSONArray("gains")
                val target = pr.getJSONArray("target")
                for (ch in 0 until 2) for (i in 0 until n)
                    dp.setPreEqBandByChannelIndex(ch, i, DynamicsProcessing.EqBand(true, cutoffs[i], gains.getDouble(i).toFloat()))
                var maxErr = 0.0
                var sq = 0.0
                var used = 0
                val row = StringBuilder()
                if (ref != null) { dp.enabled = true; tone(track, vis, 1000.0); tone(track, vis, 1000.0); tone(track, vis, 1000.0) }
                for (k in freqs.indices) {
                    val off: Double
                    val on: Double
                    if (ref != null) { off = ref[k]; on = tone(track, vis, freqs[k]) } else {
                        dp.enabled = false
                        off = tone(track, vis, freqs[k])
                        dp.enabled = true
                        on = tone(track, vis, freqs[k])
                    }
                    if (off < -65) { row.append("%.0fHz skip(%.0f) ".format(freqs[k], off)); continue }  // speaker chain cuts it
                    val resp = on - off
                    val err = resp - target.getDouble(k)
                    maxErr = max(maxErr, abs(err))
                    sq += err * err
                    used++
                    row.append("%.0fHz %.1f/%.1f ".format(freqs[k], resp, target.getDouble(k)))
                }
                worst = max(worst, maxErr)
                val line = "%-22s max %.2f dB, rms %.2f dB (%d points)".format(pr.getString("name"), maxErr,
                    sqrt(sq / max(used, 1)), used)
                Spike.log("A3 $line")
                out.append(line).append("\n   measured/target: ").append(row).append("\n")
            }
            out.append("worst max error: %.2f dB\n".format(worst))
            Spike.log("A3 done: worst max error %.2f dB".format(worst))
        } finally {
            track.stop(); track.release(); dp.release(); vis.release()
            File(dir, "a3_results.txt").writeText(out.toString())
        }
    }

    /** Plays a tone for 600 ms and returns its RMS on the output mix in dB (median of readings from 400 ms on). */
    private fun tone(track: AudioTrack, vis: Visualizer, freq: Double): Double {
        val m = Visualizer.MeasurementPeakRms()
        val block = FloatArray(rate / 100 * 2)  // 10 ms stereo
        var phase = 0.0
        val step = 2 * PI * freq / rate
        val readings = ArrayList<Double>()
        for (b in 0 until 60) {
            for (i in 0 until block.size / 2) {
                val v = (0.1 * sin(phase)).toFloat()  // -20 dBFS peak
                block[2 * i] = v; block[2 * i + 1] = v
                phase += step
            }
            track.write(block, 0, block.size, AudioTrack.WRITE_BLOCKING)
            if (b >= 40 && b % 4 == 0) { vis.getMeasurementPeakRms(m); readings += m.mRms / 100.0 }
        }
        return readings.sorted()[readings.size / 2]
    }
}
