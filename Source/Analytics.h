// Bass Better-er — © 2026 Box of Rules. Source-available, proprietary — see LICENSE.
#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include <cstdlib>

#ifndef JucePlugin_VersionString
 #define JucePlugin_VersionString "dev"   // console/tool builds (bor-bench etc.)
#endif

// Anonymous usage events -> POST boxofrules.com/api/v1/events.
// The contract lives in the site repo (docs/PLUGIN-ANALYTICS.md) and is
// promised by the site's privacy policy — the rules here are non-negotiable:
//  - anonymous only: no PII, no audio, no session/file names. The only
//    correlator is install_id, a random UUID invented on first run.
//  - the server stores no IPs.
//  - a visible opt-out (SYS "USAGE STATS") kills all sending immediately;
//    switching off sends one final `opt_out`, then nothing ever again.
// Fire-and-forget: never blocks audio or UI, silently drops on failure.
// BOR_EVENTS_URL (env) overrides the endpoint for local validation.
namespace bbeStats
{
    inline juce::PropertiesFile& props()
    {
        static juce::PropertiesFile p ([]
        {
            juce::PropertiesFile::Options o;
            o.applicationName     = "Bass Better-er";
            o.folderName          = "Box of Rules/Bass Better-er";
            o.filenameSuffix      = ".settings";
            o.osxLibrarySubFolder = "Application Support";
            return o;
        }());
        return p;
    }

    inline bool enabled() { return props().getBoolValue ("shareUsageStats", true); }

    inline juce::String installId()
    {
        auto id = props().getValue ("installId");
        if (id.isEmpty())
        {
            id = juce::Uuid().toDashedString();
            props().setValue ("installId", id);
            props().saveIfNeeded();
        }
        return id;
    }

    inline juce::String endpoint()
    {
        if (const char* e = std::getenv ("BOR_EVENTS_URL")) return juce::String (e);
        return "https://boxofrules.com/api/v1/events";
    }

    // message-thread only (PropertiesFile); the network hop is detached
    inline void send (const juce::String& event,
                      std::initializer_list<std::pair<juce::String, juce::var>> extra = {})
    {
        if (! enabled() && event != "opt_out") return;
        auto* obj = new juce::DynamicObject();
        obj->setProperty ("plugin",  "bass-betterer");
        obj->setProperty ("event",   event);
        obj->setProperty ("version", JucePlugin_VersionString);
       #if JUCE_MAC
        obj->setProperty ("os", "macos");
       #elif JUCE_WINDOWS
        obj->setProperty ("os", "windows");
       #else
        obj->setProperty ("os", "linux");
       #endif
        obj->setProperty ("daw", juce::PluginHostType().getHostDescription());
        obj->setProperty ("install_id", installId());
        if (extra.size() > 0)
        {
            auto* pr = new juce::DynamicObject();
            for (const auto& kv : extra) pr->setProperty (kv.first, kv.second);
            obj->setProperty ("props", juce::var (pr));
        }
        const auto json = juce::JSON::toString (juce::var (obj), true);
        juce::Thread::launch ([json]
        {
            juce::URL url (endpoint());
            auto stream = url.withPOSTData (json).createInputStream (
                juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inPostData)
                    .withExtraHeaders ("Content-Type: application/json\r\nAccept: application/json")
                    .withConnectionTimeoutMs (5000));
            if (stream != nullptr) stream->readEntireStreamAsString();   // drain, ignore
        });
    }

    inline void setEnabled (bool on)
    {
        if (! on && enabled()) send ("opt_out");   // final event while still allowed
        props().setValue ("shareUsageStats", on);
        props().saveIfNeeded();
    }
}
