# Bass Better-er

[![Support on Ko-fi](https://img.shields.io/badge/Ko--fi-Support-FF5E5B?logo=ko-fi&logoColor=white)](https://ko-fi.com/boxofrules)

One bass DI, rebuilt into a whole session.

A JUCE 8 audio effect plugin (AU, VST3, and Standalone, macOS and Windows). Drop it on a bass DI and it splits the signal into parallel layers. Each layer owns its slice of the spectrum and is voiced from a real Box of Rules studio capture, then they blend back into one apparent instrument. Deeper, wider, and more alive than the DI that went in.

![Bass Better-er](assets/bass-betterer.png)

Influenced heavily by the bass tones of Royal Blood, Justin Chancellor of Tool, and Muse. Big, harmonically rich low end that sits larger than the mix. The intent is simple: a low effort, low complexity way to get a studio ready signal from any bass input. No amp, no mic setup, no routing. Drop it on a DI and go.

## Channels and controls

A real bass record is never one signal. It is a foundation you feel, a body you hear, dirt that bites, and the room around all of it, balanced live. **Bass Better-er** bottles that as parallel frequency role layers. They overlap rather than brick wall, the lows stay mono and centred, and the stereo image widens as it climbs. It holds together on a phone and opens up on a big system.

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

**macOS:** open `Bass-Better-er-macOS.dmg` and run the installer (`.pkg`) — universal Apple Silicon and Intel. It installs to the system plug-in folders:

| Format | Installed to |
| --- | --- |
| AU | `/Library/Audio/Plug-Ins/Components/Bass Better-er.component` |
| VST3 | `/Library/Audio/Plug-Ins/VST3/Bass Better-er.vst3` |

The build is signed but not notarized, so the first run needs right click then Open.

**Windows:** run `Bass-Better-er-Windows.exe`. Windows is VST3 only (there is no AU), installed to the standard shared VST3 folder:

| Format | Installed to |
| --- | --- |
| VST3 | `C:\Program Files\Common Files\VST3\Bass Better-er.vst3` |

**Linux:** grab `Bass-Better-er-Linux-VST3.zip`, unzip it, and copy the `Bass Better-er.vst3` folder into one of these (Linux is VST3 only, no AU; native x86_64, for hosts like Ubuntu Studio / Reaper / Ardour):

| Scope | Copy `Bass Better-er.vst3` to |
| --- | --- |
| Just you | `~/.vst3/` |
| All users | `/usr/lib/vst3/` |

Restart your DAW, rescan plug-ins, then drop it on a bass DI track.

**Standalone app (no DAW needed):** each Release also has `Bass-Better-er-{macOS,Windows,Linux}-Standalone.zip`. The standalone is **not installed anywhere** — unzip it wherever you like and run the app directly (`Bass Better-er.app` / `.exe` / the `Bass Better-er` binary). It picks an audio input/output device and runs the same tone stack without a host. On macOS it is signed but not notarized, so right click then Open the first time (if it still refuses, clear the quarantine flag: `xattr -dr com.apple.quarantine "Bass Better-er.app"`).

## Support

Made by Box of Rules. **Bass Better-er** was not cheap or quick to build. It is years in the making, drawing on 15 years of professional engineering experience ([deviantops.com](https://deviantops.com)) and a lot of studio time capturing and tuning the real signal chain behind it.

It is free to use. If it earns a place on your tracks, throw a coffee in the tip jar. It funds more free tools like this.

[☕ ko-fi.com/boxofrules](https://ko-fi.com/boxofrules)

## Disclaimer

**Bass Better-er** is provided "as is", with no warranty of any kind. To the maximum extent permitted by law, Box of Rules accepts no responsibility or liability for any damage, data loss, crashes, or other issues arising from this software or from its installation. You install and use it entirely at your own risk.

## License

Proprietary. © Box of Rules. All rights reserved. See [LICENSE](LICENSE).
