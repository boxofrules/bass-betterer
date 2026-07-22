// Bass Better-er — © 2026 Box of Rules. Source-available, proprietary — see LICENSE.
#pragma once
#include <array>
#include <cstring>

// Drive parameter surface for the FuzzChain (v0.2.0). Two fixed drive
// characters per drive-capable strip:
//   FUZZ — the original Bass Better-er fuzz (the locked v1 constants below,
//          byte-identical to every earlier release);
//   DIST — a tighter, brighter distortion character fitted against a real
//          hardware drive chain. The fitted values are part of the encrypted
//          asset pack (ir/drive_fits.bin) alongside the impulse responses —
//          the numbers are the product, same policy as the IRs.
//
// Strip slots: 0..2 = LOW FX 57 / 421 / TWEETER, 3 = HI CRUNCH, 4 = HI AIR.
namespace drive
{
constexpr int NUM_PARAMS = 10;

enum ParamIndex : int
{
    Amount = 0,   // overall drive. FuzzChain clipper drive = DRIVE_PER_AMOUNT * amount
    Tightness,    // how clean/tight the lows stay: low-shelf into the clipper
    Bite,         // HF drive: pre-clip high-shelf emphasis
    Asym,         // even-harmonic character (negative-lobe tanh scale)
    Hard,         // soft (tanh) .. hard clip knee blend
    Casc,         // >= 0 scales the per-band grit cascade; < 0 = warmth shelf on the grit region
    Presence,     // post-cab high-shelf lift, dB
    Low,          // > 0 blends clean low end back in; < 0 high-passes the excess sub
    Tame,         // transient reduction (less pluck, more glue)
    Body,         // saturated low-mid weight (140 Hz bell, 4 dB per unit)
};

using Params = std::array<float, NUM_PARAMS>;

// FuzzChain clipper drive = DRIVE_PER_AMOUNT * amount. 64 is a power of two:
// the locked v1 drives (150/84/97) divide by 64 to exactly-representable
// floats, so FUZZ reproduces the shipped clipper drive BIT-EXACTLY (the fnv
// fingerprint contract in tools/bor_bench.cpp).
constexpr float DRIVE_PER_AMOUNT = 64.0f;

// Per-strip locked mic-chain shaping (de-fizz/warmth/grit/level trim). The
// locks stay the STRIP's regardless of the selected drive character — the
// character swaps the clipper voice, not the mic chain. levelDb equalises
// drive-on loudness to the strip's clean path at the same fader (K-weighted,
// measured with `bor-bench cal`).
struct Locks
{
    float fizz;                  // post-cab de-fizz amount (-13 dB/unit shelf @ 7 kHz)
    float warmth;                // 75/205 Hz warmth bells
    float levelDb;               // output trim: drive loudness == clean at the same fader
    std::array<float, 8> grit;   // per-band cascade shape (FuzzChain::CENTRES)
};

constexpr int NUM_DRIVE_STRIPS = 5;
constexpr int FIRST_HI_SLOT    = 3;

// The HI strips have no separate mic-chain shaping (their character IS the
// drive), so fizz/warmth/grit are neutral there; levelDb is the measured
// drive-vs-clean match through the full octave -> drive -> cab chain.
inline constexpr std::array<Locks, NUM_DRIVE_STRIPS> LOCKS { {
    { 0.8f, 0.65f, -11.2f,  { { 0.9f, 0.25f, 0.85f, 0.8f, 0.4f, 0.45f, 0.8f, 0.25f } } },   // LOW FX 57
    { 0.8f, 1.0f,  -16.1f,  { { 0.65f, 0.25f, 0.85f, 0.15f, 0.0f, 0.0f, 0.2f, 0.4f } } },   // LOW FX 421
    { 1.0f, 0.65f, -11.4f,  { { 0.9f, 0.25f, 0.85f, 0.8f, 0.4f, 0.35f, 0.0f, 0.0f } } },    // LOW FX TWT
    { 0.0f, 0.0f,  -16.57f, { { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f } } },       // HI CRUNCH
    { 0.0f, 0.0f,  -15.15f, { { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f } } },       // HI AIR
} };

// FUZZ — the locked v1 constants, public since v0.1 (amount = drive/64
// exactly: 150/64, 84/64, 97/64). The HI rows borrow the 57 and TWEETER
// characters (the fuzz was never part of the HI chain — it is offered there
// as a creative option, no accuracy claim).
inline constexpr std::array<Params, NUM_DRIVE_STRIPS> FUZZ { {
    //  amount     tght  bite  asym  hard  casc  pres  low    tame  body
    { { 2.34375f,  1.0f, 0.0f, 0.2f, 0.7f, 1.0f, 0.0f, -0.2f, 0.0f, 2.8f } },   // LOW FX 57
    { { 1.3125f,   1.0f, 0.0f, 0.2f, 0.7f, 1.0f, 0.0f, -0.2f, 0.0f, 4.0f } },   // LOW FX 421
    { { 1.515625f, 1.0f, 0.0f, 0.2f, 0.7f, 1.0f, 0.0f, -1.2f, 0.0f, 2.8f } },   // LOW FX TWT
    { { 2.34375f,  1.0f, 0.0f, 0.2f, 0.7f, 1.0f, 0.0f, -0.2f, 0.0f, 2.8f } },   // HI CRUNCH (57 character)
    { { 1.515625f, 1.0f, 0.0f, 0.2f, 0.7f, 1.0f, 0.0f, -1.2f, 0.0f, 2.8f } },   // HI AIR (TWT character)
} };

// Per-character loudness adjusts ON TOP of the strip locks' levelDb
// (measured with `bor-bench cal`, v0.2.0): the baked levelDb trims were
// measured on FUZZ for the LO strips and on DIST for the HI strips, so the
// OTHER character on each strip carries its own measured adjust. FUZZ on the
// LO strips is 0.0 BY CONTRACT — byte-identity with every earlier release.
inline constexpr std::array<float, NUM_DRIVE_STRIPS> FUZZ_TRIM_DB { { 0.0f, 0.0f, 0.0f,  4.00f,  3.64f } };
inline constexpr std::array<float, NUM_DRIVE_STRIPS> DIST_TRIM_DB { { 4.32f, -0.33f, -5.62f, 1.30f, -0.17f } };

// DIST — the fitted distortion character. Values arrive at runtime from the
// encrypted asset pack (ir/drive_fits.bin, embedded as BinaryData like the
// IRs): 'B''B''F''1' + 3 rows x 10 float32 LE (LOW-shared, HI CRUNCH, HI AIR).
// Until/unless loadFits succeeds, DIST falls back to the FUZZ rows so a
// build with a malformed pack still makes sound instead of silence.
inline std::array<Params, NUM_DRIVE_STRIPS> DIST = FUZZ;

inline bool loadFits (const void* data, int size)
{
    if (data == nullptr || size < 4 + 3 * NUM_PARAMS * (int) sizeof (float))
        return false;
    const auto* bytes = static_cast<const unsigned char*> (data);
    if (std::memcmp (bytes, "BBF1", 4) != 0)
        return false;
    std::array<Params, 3> rows {};
    std::memcpy (rows.data(), bytes + 4, 3 * NUM_PARAMS * sizeof (float));
    DIST[0] = rows[0]; DIST[1] = rows[0]; DIST[2] = rows[0];   // one LOW character
    DIST[3] = rows[1];                                          // HI CRUNCH (native)
    DIST[4] = rows[2];                                          // HI AIR (native)
    return true;
}
} // namespace drive
