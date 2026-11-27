// SilverConsole - cache de plugins escaneados (como o REAPER: cada arquivo VST3
// fica registrado com tamanho e data; so arquivos novos ou alterados voltam a
// ser abertos no scanner isolado). Codigo original (MIT).
#pragma once
#include <JuceHeader.h>
#include "PluginScanPolicy.h"
#include "PluginScanSupervisor.h"
#include <atomic>
#include <map>

namespace sc
{
struct PluginScanCache
{
    static constexpr const char* settingsKey = "vst3ScanCache";

    /** Identidade do arquivo: tamanho + data de modificacao do binario. Num bundle
        .vst3 (pasta) vale o binario de Contents/x86_64-win; sem ele, a pasta. */
    struct Fingerprint
    {
        juce::int64 size = -1, modTime = 0;
        bool operator== (const Fingerprint& o) const noexcept { return size == o.size && modTime == o.modTime; }
        bool operator!= (const Fingerprint& o) const noexcept { return ! operator== (o); }
        bool exists() const noexcept { return size >= 0; }
    };

    struct Entry
    {
        juce::String path;
        Fingerprint fp;
        bool ok = false;   // false = falhou no scanner (quarentena)
        int types = 0;     // quantos plugins o arquivo tinha
    };

    static Fingerprint fingerprintOf (const juce::File& f)
    {
        Fingerprint fp;
        if (f.isDirectory())
        {
            juce::File bin;
            for (auto* arch : { "x86_64-win", "arm64x-win", "arm64ec-win", "arm64-win" })
            {
                auto dir = f.getChildFile ("Contents").getChildFile (arch);
                if (dir.isDirectory())
                {
                    auto found = dir.findChildFiles (juce::File::findFiles, false, "*.vst3");
                    if (! found.isEmpty()) { bin = found.getFirst(); break; }
                }
            }
            if (bin.existsAsFile()) { fp.size = bin.getSize(); fp.modTime = bin.getLastModificationTime().toMilliseconds(); }
            else { fp.size = 0; fp.modTime = f.getLastModificationTime().toMilliseconds(); }
        }
        else if (f.existsAsFile())
        {
            fp.size = f.getSize();
            fp.modTime = f.getLastModificationTime().toMilliseconds();
        }
        return fp;
    }

    /** Mesma chave para o bundle (pasta .vst3) e para o binario dentro dele
        (Contents/x86_64-win/x.vst3): a lista antiga guarda um, a busca acha o outro. */
    static juce::String keyFor (const juce::String& path)
    {
        auto p = path.toLowerCase().replaceCharacter ('/', '\\');
        const int i = p.indexOf (".vst3\\contents\\");
        if (i >= 0) p = p.substring (0, i + 5);
        while (p.endsWithChar ('\\')) p = p.dropLastCharacters (1);
        return p;
    }

    const Entry* find (const juce::String& path) const
    {
        auto it = entries.find (keyFor (path));
        return it != entries.end() ? &it->second : nullptr;
    }
    void set (const juce::String& path, Fingerprint fp, bool ok, int types)
    {
        entries[keyFor (path)] = { path, fp, ok, types };
    }
    void remove (const juce::String& path) { entries.erase (keyFor (path)); }
    bool empty() const noexcept { return entries.empty(); }
    int size() const noexcept { return (int) entries.size(); }

    /** O arquivo continua igual ao registrado (nao precisa abrir de novo). */
    bool isUpToDate (const juce::String& path, Fingerprint current) const
    {
        auto* e = find (path);
        return e != nullptr && current.exists() && e->fp == current;
    }

    juce::String serialize() const
    {
        juce::XmlElement root ("VST3_SCAN_CACHE");
        root.setAttribute ("version", 1);
        for (auto& [k, e] : entries)
        {
            auto* x = root.createNewChildElement ("FILE");
            x->setAttribute ("path", e.path);
            x->setAttribute ("size", juce::String (e.fp.size));
            x->setAttribute ("time", juce::String (e.fp.modTime));
            x->setAttribute ("ok", e.ok);
            x->setAttribute ("types", e.types);
        }
        return root.toString (juce::XmlElement::TextFormat().singleLine().withoutHeader());
    }

    static PluginScanCache deserialize (const juce::String& text)
    {
        PluginScanCache c;
        if (auto xml = juce::parseXML (text))
            if (xml->hasTagName ("VST3_SCAN_CACHE"))
                for (auto* x : xml->getChildWithTagNameIterator ("FILE"))
                {
                    const auto path = x->getStringAttribute ("path");
                    if (path.isEmpty()) continue;
                    Fingerprint fp { x->getStringAttribute ("size").getLargeIntValue(), x->getStringAttribute ("time").getLargeIntValue() };
                    c.set (path, fp, x->getBoolAttribute ("ok"), x->getIntAttribute ("types"));
                }
        return c;
    }

    std::map<juce::String, Entry> entries;
};

//==============================================================================
/** Varredura em processo isolado (um ScPluginScanWorker por arquivo). Roda numa
    thread de trabalho; nao mexe na lista de plugins (o resultado e aplicado na
    thread de mensagens). */
struct PluginScanRun
{
    enum class Mode
    {
        newOnly,     // so arquivos novos ou alterados (padrao, como o REAPER)
        everything,  // ignora o cache (mantem a quarentena)
        retryFailed, // so os que falharam antes
        probeDsp     // abre e processa audio de todos (teste)
    };

    struct Request
    {
        juce::File worker;
        Mode mode = Mode::newOnly;
        juce::StringArray candidates;  // vazio = pastas padrao de VST3
        juce::StringArray quarantined; // PluginScanPolicy
        PluginScanCache cache;
    };

    struct Result
    {
        std::vector<juce::PluginDescription> types;
        juce::StringArray candidates, successPaths, failedPaths;
        std::vector<PluginScanCache::Fingerprint> successFp, failedFp;
        std::vector<int> successTypes;
        int total = 0, scanned = 0, skippedKnown = 0, skippedFailed = 0, failed = 0, timedOut = 0;
        bool cancelled = false;
    };

    static juce::StringArray defaultCandidates()
    {
        juce::VST3PluginFormat format;
        auto c = format.searchPathsForPlugins (format.getDefaultLocationsToSearch(), true, false);
        c.removeDuplicates (true);
        return c;
    }

    /** current: nome do arquivo sendo examinado (para a barra de status). */
    static void run (const Request& req, Result& result, std::atomic<bool>& cancel, std::atomic<float>& progress,
                     std::atomic<int>* doneCount = nullptr, juce::String* current = nullptr, juce::CriticalSection* currentLock = nullptr)
    {
        // Apenas a enumeracao de caminhos acontece no host. Todo carregamento
        // de VST3 para discovery acontece em processo filho independente.
        auto candidates = req.candidates.isEmpty() ? defaultCandidates() : req.candidates;
        result.candidates = candidates;
        result.total = candidates.size();
        const bool retry = req.mode == Mode::retryFailed;

        for (int i = 0; i < candidates.size(); ++i)
        {
            auto bump = [&]
            {
                progress = (float) (i + 1) / (float) juce::jmax (1, candidates.size());
                if (doneCount != nullptr) doneCount->store (i + 1);
            };
            if (cancel.load()) { result.cancelled = true; break; }
            const auto& candidate = candidates.getReference (i);
            const auto fp = PluginScanCache::fingerprintOf (juce::File (candidate));
            const auto* cached = req.cache.find (candidate);
            const bool changed = cached == nullptr || cached->fp != fp;

            bool scan = PluginScanPolicy::shouldScan (req.quarantined, candidate, retry);
            // em quarentena mas o arquivo mudou (plugin atualizado): tenta de novo, como o REAPER
            if (! scan && ! retry && req.mode != Mode::probeDsp && cached != nullptr && ! cached->ok && changed)
                scan = true;
            if (! scan) { ++result.skippedFailed; bump(); continue; }
            if (req.mode == Mode::newOnly && ! changed && cached->ok) { ++result.skippedKnown; bump(); continue; }

            ++result.scanned;
            if (current != nullptr && currentLock != nullptr)
            {
                const juce::ScopedLock sl (*currentLock);
                *current = juce::File (candidate).getFileNameWithoutExtension();
            }
            const auto destination = juce::File::getSpecialLocation (juce::File::tempDirectory)
                .getChildFile ("silverconsole-scan-" + juce::Uuid().toString() + ".xml");
            juce::StringArray args;
            args.add (req.worker.getFullPathName());
            if (req.mode == Mode::probeDsp) args.add ("--probe");
            args.add (candidate);
            args.add (destination.getFullPathName());

            int found = 0;
            // Supervisor compartilhado com os testes de falha/cancelamento.
            // Nao roda na thread realtime e mata o filho em timeout/cancel.
            const auto scanResult = PluginScanSupervisor::run (args, cancel, 20000);
            if (scanResult.status == PluginScanSupervisor::Status::cancelled) result.cancelled = true;
            if (scanResult.status == PluginScanSupervisor::Status::timedOut) ++result.timedOut;

            if (scanResult.ok() && destination.getSize() > 0 && destination.getSize() <= 4 * 1024 * 1024)
                if (auto xml = juce::XmlDocument::parse (destination))
                    if (xml->hasTagName ("SILVERCONSOLE_PLUGIN_SCAN") && xml->getStringAttribute ("format") == "VST3"
                        && xml->getIntAttribute ("schemaVersion") == 1)
                        for (auto* node : xml->getChildIterator())
                        {
                            if (! node->hasTagName ("PLUGIN")) continue;
                            juce::PluginDescription desc;
                            if (desc.loadFromXml (*node) && desc.pluginFormatName == "VST3" && desc.name.isNotEmpty())
                            {
                                result.types.push_back (std::move (desc));
                                ++found;
                            }
                        }
            destination.deleteFile();
            if (result.cancelled) break;

            if (found > 0)
            {
                result.successPaths.add (candidate);
                result.successFp.push_back (fp);
                result.successTypes.push_back (found);
            }
            else
            {
                result.failedPaths.add (candidate);
                result.failedFp.push_back (fp);
                ++result.failed;
            }
            bump();
        }
        if (current != nullptr && currentLock != nullptr)
        {
            const juce::ScopedLock sl (*currentLock);
            current->clear();
        }
    }

    /** Aplica o resultado (thread de mensagens): atualiza a lista de plugins, o
        cache e a quarentena. Arquivos que sumiram das pastas saem da lista.
        Devolve quantos plugins entraram (novos) e quantos sairam. */
    static void apply (const Result& r, Mode mode, juce::KnownPluginList& known, PluginScanCache& cache,
                       juce::StringArray& quarantined, int& added, int& removed)
    {
        added = removed = 0;
        auto sameFile = [] (const juce::String& a, const juce::String& b) { return PluginScanCache::keyFor (a) == PluginScanCache::keyFor (b); };
        auto typesOf = [&] (const juce::String& path)
        {
            juce::Array<juce::PluginDescription> out;
            for (auto& d : known.getTypes()) if (sameFile (d.fileOrIdentifier, path)) out.add (d);
            return out;
        };

        for (int i = 0; i < r.successPaths.size(); ++i)
        {
            const auto& path = r.successPaths[i];
            PluginScanPolicy::reportResult (quarantined, path, true);
            const auto before = typesOf (path);
            // plugin alterado: as descricoes antigas saem e entram as novas
            for (auto& d : before) known.removeType (d);
            for (auto& d : r.types)
                if (sameFile (d.fileOrIdentifier, path))
                {
                    bool existed = false;
                    for (auto& b : before) existed = existed || b.isDuplicateOf (d);
                    known.addType (d);
                    if (! existed) ++added;
                }
            cache.set (path, r.successFp[(size_t) i], true, r.successTypes[(size_t) i]);
        }
        for (int i = 0; i < r.failedPaths.size(); ++i)
        {
            const auto& path = r.failedPaths[i];
            if (mode == Mode::probeDsp) { PluginScanPolicy::reportResult (quarantined, path, false); continue; }
            PluginScanPolicy::reportResult (quarantined, path, false);
            cache.set (path, r.failedFp[(size_t) i], false, 0);
        }
        // descricoes de plugins que nao foram achadas pelo scanner isolado (por
        // exemplo, de fora da varredura) ficam; os arquivos apagados do disco saem.
        if (! r.cancelled && mode != Mode::retryFailed)
        {
            juce::Array<juce::PluginDescription> gone;
            for (auto& d : known.getTypes())
                if (d.pluginFormatName == "VST3" && ! juce::File (d.fileOrIdentifier).exists()) gone.add (d);
            for (auto& d : gone) { known.removeType (d); ++removed; }
            std::vector<juce::String> stale;
            for (auto& [k, e] : cache.entries) if (! juce::File (e.path).exists()) stale.push_back (e.path);
            for (auto& p : stale) { cache.remove (p); for (int i = quarantined.size(); --i >= 0;) if (sameFile (quarantined[i], p)) quarantined.remove (i); }
        }
    }

    /** Primeira vez com o cache: registra os plugins que ja estao na lista (e os
        em quarentena) com a identidade atual, para nao reescanear tudo. */
    static void seedFromKnown (PluginScanCache& cache, const juce::KnownPluginList& known, const juce::StringArray& quarantined)
    {
        if (! cache.empty()) return;
        std::map<juce::String, int> counts;
        for (auto& d : known.getTypes())
            if (d.pluginFormatName == "VST3") ++counts[d.fileOrIdentifier];
        for (auto& [path, n] : counts)
        {
            const auto fp = PluginScanCache::fingerprintOf (juce::File (path));
            if (fp.exists()) cache.set (path, fp, true, n);
        }
        for (auto& q : quarantined)
        {
            const auto fp = PluginScanCache::fingerprintOf (juce::File (q));
            if (fp.exists()) cache.set (q, fp, false, 0);
        }
    }
};
} // namespace sc
