# Bass Better-er

[![Support on Ko-fi](https://img.shields.io/badge/Ko--fi-Support-FF5E5B?logo=ko-fi&logoColor=white)](https://ko-fi.com/boxofrules)

One bass DI, rebuilt into a whole session.

A JUCE 8 audio effect plugin (AU, VST3, and Standalone, macOS and Windows). Drop it on a bass DI and it splits the signal into parallel layers. Each layer owns its slice of the spectrum and is voiced from a real Box of Rules studio capture, then they blend back into one apparent instrument. Deeper, wider, and more alive than the DI that went in.

![Bass Better-er](assets/bass-betterer.png)

## Channels and controls

A real bass record is never one signal. It is a foundation you feel, a body you hear, dirt that bites, and the room around all of it, balanced live. Bass Better-er bottles that as parallel frequency role layers. They overlap rather than brick wall, the lows stay mono and centred, and the stereo image widens as it climbs. It holds together on a phone and opens up on a big system.

| Layer | Role |
| --- | --- |
| DI | The original dry DI tone, blended back in. Muted by default. |
| SUB | The foundation. Always on, dead centre. |
| LOW CLEAN | Body and warmth. |
| LOW FX | Grit and aggression, with an engageable FUZZ. |
| ROOM | Air and space around the whole thing. |

Every layer is a channel strip with the same controls.

| Strip control | What it does |
| --- | --- |
| Gain | Layer level. |
| M | Mute. |
| S | Solo. |
| Pan | Placement in the stereo field (shown only in stereo). |
| Ø | Phase (polarity) invert. |
| SC | Sidechain. Duck this layer when the dirt hits, keyed off the LOW FX. |
| FUZZ | Engage the dirt. LOW FX layers only. |

**Sidechain ducking:** the LOW FX dirt acts as a sidechain key. Arm SC on any layer, even the SUB, and it ducks out of the way when the dirt comes in, so the grit cuts through without the low end fighting it.

| Master | What it does |
| --- | --- |
| INPUT | Input gain. Also drives the fuzz, like a pedal. |
| GLUE | Sums the layers into one cohesive instrument. |
| OUTPUT | Output gain. |
| FREQ | Spectrum display on or off (turn off to save CPU). |

**Presets:** the header PRESET menu has factory starting points (Hysterical, Subby, Clean Stack, Dirt Duck, Init) plus a Save current option for your own. Saved presets are portable across projects. Your full settings are also saved with the DAW project automatically, and via the host's own preset and A/B system.

## Install (from a Release)

Grab the latest Release installer for your platform.

**macOS:** open `Bass-Better-er-macOS.dmg` and run the installer (`.pkg`). It puts the AU and VST3 in place (universal Apple Silicon and Intel). The build is signed but not notarized, so the first run needs right click then Open.

**Windows:** run `Bass-Better-er-Windows.exe`. It installs the VST3 to the standard folder. Windows is VST3 only, there is no AU.

Restart your DAW, rescan plug-ins, then drop it on a bass DI track.

## Support

Made by Box of Rules. If Bass Better-er earns a place on your tracks, throw a coffee in the tip jar. It funds more free tools like this.

[☕ ko-fi.com/boxofrules](https://ko-fi.com/boxofrules)

## License

Proprietary. © Box of Rules. All rights reserved. See [LICENSE](LICENSE).
