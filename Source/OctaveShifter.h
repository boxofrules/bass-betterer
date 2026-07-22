#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

// OctaveShifter — Whammy-style dual-tap crossfaded delay-line (granular)
// OCTAVE-UP pitch shifter for the HI layers. Ship parameters: grain 32 ms,
// Hann crossfade, 6 kHz pre-LPF, correlation-splice snap 32 ms. UP only —
// no ratio parameter exists and none is reserved.
//
// Architecture:
//   * ring buffer; TWO read taps whose delay sweeps grain -> 0 at rate
//     (1 - ratio) = -1 sample/sample (the read head races ahead at 2x, so
//     pitch doubles exactly between wraps);
//   * each tap's delay wraps every `grain`; taps run half a grain apart;
//   * exact Hann-pair crossfade at the wrap, amplitude-complementary
//     normalisation (the taps are snap-aligned, i.e. coherent);
//   * SNAP (correlation splice): at each wrap the tap searches up to `snap`
//     samples of EXTRA delay for the ring position whose waveform best
//     matches what the OTHER tap is currently playing (8 ms normalised
//     cross-correlation). Without it a free-running splice scatters bass
//     fundamentals into inharmonic sidebands (measured: 110 Hz in, dominant
//     partial lands near 235 Hz instead of 220);
//   * 2nd-order Butterworth pre-LPF before the shift (content above the
//     cutoff lands at cutoff*2 — anti-alias / de-fizz insurance).
//
// Determinism contract:
//   - all internal state and math is double precision; output depends only
//     on absolute sample position and buffer history, so any block-size
//     chopping of the same input renders bit-identically.
//
// UI-agnostic and JUCE-free on purpose.
struct OctaveShifter
{
    // ship params (tonelab octave.py SHIP_PARAMS — fit 2026-07-07, 384
    // configs vs the real HI OCT captures; REPORT.md is the record)
    static constexpr double RATIO      = 2.0;      // octave UP, fixed
    static constexpr double GRAIN_MS   = 32.0;
    static constexpr double PRE_LPF_HZ = 6000.0;
    static constexpr double SNAP_MS    = 32.0;
    // crossfade shape is exactly 1.0 (the Hann pair): sin(pi*x)**1.0 == sin(pi*x),
    // so the pow() is elided — bit-identical to numpy's `** 1.0`.

    // octave.py chops anything larger into 4096-sample pieces (the ring's
    // write margin is sized for hardware-scale blocks); block-size invariance
    // makes the chop transparent. Same here.
    static constexpr int MAX_CHUNK = 4096;

    void prepare (double sampleRate)
    {
        sr    = sampleRate;
        grain = std::max (8.0, GRAIN_MS * 1.0e-3 * sr);            // samples (fractional)
        snap  = (int) std::nearbyint (std::max (0.0, SNAP_MS) * 1.0e-3 * sr);
        tmpl  = (int) (0.008 * sr);                                // 8 ms splice template
        W     = grain / std::max (std::abs (1.0 - RATIO), 1.0e-6); // wrap period (== grain at 2.0)

        int n = 1;
        while (n < (int) grain + snap + tmpl + 8192 + 4)
            n <<= 1;
        buf.assign ((size_t) n, 0.0);
        mask = (int64_t) n - 1;

        // pre-shift anti-alias LPF: Butterworth 2nd order at min(6 kHz, 0.49*sr),
        // bilinear transform with prewarp — scipy.signal.butter's design (see
        // the faithfulness note above)
        {
            const double wn = std::min (PRE_LPF_HZ, 0.49 * sr) / (0.5 * sr);
            const double K  = std::tan (PI * wn * 0.5);
            const double nm = 1.0 + SQRT2 * K + K * K;
            lb0 = K * K / nm;
            lb1 = 2.0 * lb0;
            lb2 = lb0;
            la1 = 2.0 * (K * K - 1.0) / nm;
            la2 = (1.0 - SQRT2 * K + K * K) / nm;
        }

        wScr.assign ((size_t) tmpl, 0.0);
        lScr.assign ((size_t) (snap + tmpl), 0.0);
        dIn.assign ((size_t) MAX_CHUNK, 0.0);
        dOut.assign ((size_t) MAX_CHUNK, 0.0);
        reset();
    }

    void reset()
    {
        std::fill (buf.begin(), buf.end(), 0.0);
        wpos = 0;
        nextWrap = { W, W * 0.5 };
        off = { 0.0, 0.0 };
        lz0 = lz1 = 0.0;
    }

    // Latency reported to the host while a HI strip is unmuted.
    // The readout sits a time-varying (tap delay 0..grain) + (splice offset
    // 0..snap) behind the input; a single PDC number is necessarily a summary.
    // We report the centre of that distribution: the Hann-weighted mean tap
    // delay is exactly grain/2, and the correlation splice lands anywhere in
    // 0..snap (period-aligned on bass), mean ~snap/2 — so grain/2 + snap/2
    // = 32 ms at ship params (1536 smp @ 48 kHz). REPORT.md quotes the full
    // worst-case range (~grain/2..grain+snap ≈ 16-64 ms) for honesty.
    int latencySamples() const { return (int) std::lround (0.5 * (grain + (double) snap)); }

    // ---- block API -----------------------------------------------------------
    // Double-precision core. Output depends only on absolute position and
    // history => block-size invariant.
    void process (const double* in, double* out, int n)
    {
        int pos = 0;
        while (n - pos > MAX_CHUNK)
        {
            processChunk (in + pos, out + pos, MAX_CHUNK);
            pos += MAX_CHUNK;
        }
        processChunk (in + pos, out + pos, n - pos);
    }

    // float wrapper for the plugin's audio path (float->double is exact; the
    // final double->float rounding is the only lossy step and is outside the
    // Python-fidelity surface)
    void processBlock (const float* in, float* out, int n)
    {
        int pos = 0;
        while (pos < n)
        {
            const int m = std::min (n - pos, MAX_CHUNK);
            for (int i = 0; i < m; ++i) dIn[(size_t) i] = (double) in[pos + i];
            processChunk (dIn.data(), dOut.data(), m);
            for (int i = 0; i < m; ++i) out[pos + i] = (float) dOut[(size_t) i];
            pos += m;
        }
    }

private:
    static constexpr double PI    = 3.14159265358979323846;
    static constexpr double SQRT2 = 1.41421356237309514547;   // np.sqrt(2)

    void processChunk (const double* in, double* out, int n)
    {
        if (n <= 0) return;

        // pre-LPF (scipy sosfilt transposed-DF2 recurrence, zi = 0 at reset)
        // + write into the ring. Reads never look ahead of `now`, so writing
        // the whole block first is causally safe (octave.py does the same).
        for (int i = 0; i < n; ++i)
        {
            const double x = in[i];
            const double y = lb0 * x + lz0;
            lz0 = lb1 * x - la1 * y + lz1;
            lz1 = lb2 * x - la2 * y;
            buf[(size_t) ((wpos + (int64_t) i) & mask)] = y;
        }

        const int64_t t0 = wpos, tEnd = wpos + (int64_t) n;
        int64_t segStart = t0;
        while (segStart < tEnd)
        {
            // next wrap event of either tap inside the block
            const double nw = std::min (nextWrap[0], nextWrap[1]);
            const int64_t segEnd = nw < (double) tEnd
                ? std::min<int64_t> (tEnd, (int64_t) std::ceil (nw)) : tEnd;
            for (int64_t t = segStart; t < segEnd; ++t)
            {
                const double td = (double) t;
                const double b0 = delayAt (0, td), b1 = delayAt (1, td);
                const double p0 = clamp01 (b0 / grain), p1 = clamp01 (b1 / grain);
                const double g0 = std::sin (PI * p0), g1 = std::sin (PI * p1);
                // amplitude-complementary: the taps are snap-aligned (coherent sum)
                const double norm = g0 + g1;
                out[t - t0] = (g0 * readAt (td - (b0 + off[0]))
                             + g1 * readAt (td - (b1 + off[1]))) / (norm + 1.0e-12);
            }
            for (int tap = 0; tap < 2; ++tap)
                if (nextWrap[(size_t) tap] <= (double) segEnd && nextWrap[(size_t) tap] < (double) tEnd)
                {
                    const double tE = nextWrap[(size_t) tap];
                    nextWrap[(size_t) tap] += W;
                    splice (tap, tE);
                }
            segStart = segEnd;
        }
        wpos += (int64_t) n;
    }

    // base tap delay (samples) at absolute time t, before the splice offset:
    // up-shift sweeps grain -> 0 over one wrap period
    double delayAt (int tap, double t) const
    {
        const double frac = (nextWrap[(size_t) tap] - t) / W;   // 1 at wrap -> 0 at next
        return frac * grain;
    }

    static double clamp01 (double v) { return std::min (std::max (v, 0.0), 1.0); }

    // linear-interpolated fractional ring read at absolute position (can be
    // negative during fill-in: int64 & mask wraps two's-complement, exactly
    // like numpy int64 indexing in octave.py)
    double readAt (double pos) const
    {
        const double fl = std::floor (pos);
        const int64_t i0 = (int64_t) fl;
        const double fr = pos - fl;
        const double a = buf[(size_t) (i0 & mask)];
        const double b = buf[(size_t) ((i0 + 1) & mask)];
        return a + fr * (b - a);
    }

    // Correlation splice at wrap time tE (octave.py _splice, 1:1): align the
    // wrapping tap's incoming grain with what the OTHER tap (at full gain
    // right now) is playing — normalised cross-correlation of an 8 ms
    // template over 0..snap samples of extra delay. First maximum wins on
    // exact ties, matching np.argmax.
    void splice (int tap, double tE)
    {
        if (snap <= 0) { off[(size_t) tap] = 0.0; return; }
        const int other = 1 - tap;
        const int T = tmpl, S = snap;
        const int64_t posO = (int64_t) std::nearbyint (tE - delayAt (other, tE) - off[(size_t) other]);
        const int64_t posN = (int64_t) std::nearbyint (tE - grain);   // nominal post-wrap delay (up) = grain

        double ww = 0.0;
        for (int i = 0; i < T; ++i)
        {
            const double v = buf[(size_t) ((posO - (int64_t) T + 1 + (int64_t) i) & mask)];
            wScr[(size_t) i] = v;
            ww += v * v;
        }
        if (ww < 1.0e-12) { off[(size_t) tap] = 0.0; return; }   // silence: keep nominal

        const int L = S + T;                                     // candidates, oldest first
        for (int i = 0; i < L; ++i)
            lScr[(size_t) i] = buf[(size_t) ((posN - (int64_t) L + 1 + (int64_t) i) & mask)];

        double best = 0.0;
        int bestK = -1;
        for (int k = 0; k <= S; ++k)
        {
            double c = 0.0, e = 0.0;
            const double* Lp = lScr.data() + k;
            for (int j = 0; j < T; ++j)
            {
                c += Lp[j] * wScr[(size_t) j];
                e += Lp[j] * Lp[j];
            }
            const double score = c / (std::sqrt (e) + 1.0e-9);
            if (bestK < 0 || score > best) { best = score; bestK = k; }
        }
        off[(size_t) tap] = (double) (S - bestK);                // corr[k] <-> extra delay S - k
    }

    // ---- state (plain scalars + one ring — octave.py's struct verbatim) -----
    double sr = 48000.0;
    double grain = 0.0, W = 0.0;
    int snap = 0, tmpl = 0;
    std::array<double, 2> nextWrap { 0.0, 0.0 };   // absolute output sample index of each tap's next wrap
    std::array<double, 2> off      { 0.0, 0.0 };   // per-grain splice offsets
    std::vector<double> buf;                        // ring buffer (power-of-two)
    int64_t mask = 0;
    int64_t wpos = 0;                               // samples written so far

    double lb0 = 1.0, lb1 = 0.0, lb2 = 0.0, la1 = 0.0, la2 = 0.0;   // pre-LPF biquad
    double lz0 = 0.0, lz1 = 0.0;                                    // (transposed DF2 state)

    std::vector<double> wScr, lScr;                 // splice scratch (no audio-thread allocs)
    std::vector<double> dIn, dOut;                  // float-wrapper scratch
};
