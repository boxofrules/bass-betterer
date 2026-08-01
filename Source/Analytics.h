// Bass Better-er — © 2026 Box of Rules. Source-available, proprietary — see LICENSE.
#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_cryptography/juce_cryptography.h>
#include <atomic>
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
//
// Events are not sent one-by-one: they queue in an encrypted local file and
// flush in batches — hourly, shortly after launch (delivering anything queued
// from previous sessions), or when the queue grows past a threshold. Each
// event carries its real timestamp (`ts`) so batching costs no accuracy, and
// nothing is lost when the machine is offline. Failed sends stay queued
// (capped; oldest dropped first). The queue file is encrypted so a shared
// machine doesn't accumulate a readable usage log — that is local hygiene,
// not secrecy: the payload format is documented and the key is right here.
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

    // ---- encrypted on-disk queue (message thread only) ----

    inline const juce::BlowFish& cipher()
    {
        // Local-file privacy only (see header comment) — not a secret.
        static const juce::uint8 k[] = { 0x42,0x6f,0x52,0x21,0x9c,0x3e,0x71,0xd4,
                                         0x08,0xa5,0x5b,0xee,0x17,0xc2,0x60,0x9f,
                                         0x33,0x7a,0x84,0xd1,0x2b,0x46,0xf0,0x1d };
        static const juce::BlowFish bf (k, (int) sizeof (k));
        return bf;
    }

    inline juce::File queueFile()
    {
        return props().getFile().getSiblingFile ("usage-queue.dat");
    }

    inline juce::Array<juce::var> loadQueue()
    {
        juce::MemoryBlock mb;
        if (! queueFile().loadFileAsData (mb) || mb.isEmpty())
            return {};
        cipher().decrypt (mb);
        auto parsed = juce::JSON::parse (mb.toString());
        juce::Array<juce::var> out;
        if (auto* arr = parsed.getArray()) out = *arr;
        return out;   // undecryptable/garbled -> empty; the queue is disposable
    }

    inline void saveQueue (const juce::Array<juce::var>& q)
    {
        if (q.isEmpty()) { queueFile().deleteFile(); return; }
        auto json = juce::JSON::toString (juce::var (q), true);
        juce::MemoryBlock mb (json.toRawUTF8(), json.getNumBytesAsUTF8());
        cipher().encrypt (mb);
        queueFile().replaceWithData (mb.getData(), mb.getSize());
    }

    // ---- batch flusher: hourly + post-launch + when the queue grows ----

    class Flusher : private juce::Timer,
                    public juce::DeletedAtShutdown
    {
    public:
        static Flusher& get()
        {
            static Flusher* f = new Flusher();   // DeletedAtShutdown owns it
            return *f;
        }

        void noteQueued (int queueSize)
        {
            if (queueSize >= flushAtSize) flushNow();
        }

        void flushNow()
        {
            if (inFlight.exchange (true)) return;
            auto q = loadQueue();
            if (q.isEmpty()) { inFlight = false; return; }

            const int n = juce::jmin (25, q.size());   // server cap per request
            juce::Array<juce::var> chunk;
            for (int i = 0; i < n; ++i) chunk.add (q[i]);

            auto* body = new juce::DynamicObject();
            body->setProperty ("events", juce::var (chunk));
            const auto json = juce::JSON::toString (juce::var (body), true);

            juce::Thread::launch ([json, n]
            {
                int status = 0;
                juce::URL url (endpoint());
                auto stream = url.withPOSTData (json).createInputStream (
                    juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inPostData)
                        .withExtraHeaders ("Content-Type: application/json\r\nAccept: application/json")
                        .withConnectionTimeoutMs (5000)
                        .withStatusCode (&status));
                if (stream != nullptr) stream->readEntireStreamAsString();   // drain, ignore
                const bool ok = status >= 200 && status < 300;

                juce::MessageManager::callAsync ([ok, n]
                {
                    auto& f = get();
                    if (ok)
                    {
                        auto q2 = loadQueue();
                        q2.removeRange (0, n);
                        saveQueue (q2);
                        f.inFlight = false;
                        if (! q2.isEmpty()) f.flushNow();   // next chunk
                    }
                    else
                    {
                        f.inFlight = false;   // keep queued; the timer retries
                    }
                });
            });
        }

    private:
        Flusher()
        {
            startTimer (60 * 60 * 1000);   // hourly
            // deliver anything queued from a previous / offline session,
            // shortly after load so plugin scans and startup stay untouched
            juce::Timer::callAfterDelay (20000, [] { get().flushNow(); });
        }

        void timerCallback() override { flushNow(); }

        static constexpr int flushAtSize = 20;
        std::atomic<bool> inFlight { false };
    };

    // message-thread only (PropertiesFile + queue file)
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

        auto* pr = new juce::DynamicObject();
        pr->setProperty ("ts", (juce::int64) juce::Time::currentTimeMillis() / 1000);
        for (const auto& kv : extra) pr->setProperty (kv.first, kv.second);
        obj->setProperty ("props", juce::var (pr));

        auto q = loadQueue();
        q.add (juce::var (obj));
        while (q.size() > 500) q.remove (0);   // cap: drop oldest, never grow unbounded
        saveQueue (q);

        if (event == "opt_out") Flusher::get().flushNow();
        else                    Flusher::get().noteQueued (q.size());
    }

    inline void setEnabled (bool on)
    {
        if (! on && enabled()) send ("opt_out");   // final event while still allowed
        props().setValue ("shareUsageStats", on);
        props().saveIfNeeded();
    }
}
