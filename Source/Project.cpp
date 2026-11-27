// SilverConsole - projeto (.sconsole, XML). Codigo original (MIT).
#include "Project.h"

namespace sc
{
namespace
{
    ChannelData captureChannel (Channel& c, const std::vector<std::unique_ptr<Track>>& tracks)
    {
        ChannelData d;
        d.name = c.name;
        d.colour = c.colour;
        d.gainDb = c.gainDb.load();
        d.pan = c.pan.load();
        d.mute = c.mute.load();
        d.solo = c.solo.load();
        d.phase = c.phase.load();
        d.numInserts = c.inserts.size();
        d.group = c.group.load();
        for (int i = 0; i < c.inserts.size(); ++i)
        {
            auto& s = c.inserts[(size_t) i];
            if (s.isEmpty()) continue;
            InsertData in;
            in.slot = i;
            in.bypass = s.bypassed.load();
            in.hasDesc = s.hasDescription;
            in.isolated = s.preferRemote;
            in.offline = s.offline;
            in.mix = s.mix.load();
            for (size_t k = 0; k < tracks.size(); ++k)
                if (s.sidechain != nullptr && tracks[k].get() == s.sidechain) in.sidechain = (int) k;
            in.desc = s.description;
            if (s.processor != nullptr) s.processor->getStateInformation (in.state);
            else in.state = s.missingState;
            if (in.hasDesc) d.inserts.push_back (std::move (in));
        }
        d.autoMode = c.getAutoMode();
        for (auto& l : c.lanes)
        {
            if (l->points.empty()) continue;
            d.lanes.push_back ({ l->target, l->slot, l->param, l->step, l->points });
        }
        return d;
    }

    void writeChannel (juce::XmlElement& e, const ChannelData& d)
    {
        e.setAttribute ("name", d.name);
        e.setAttribute ("colour", d.colour.toString());
        e.setAttribute ("gainDb", d.gainDb);
        e.setAttribute ("pan", d.pan);
        e.setAttribute ("mute", d.mute);
        e.setAttribute ("solo", d.solo);
        e.setAttribute ("phase", d.phase);
        e.setAttribute ("inserts", d.numInserts);
        if (d.group >= 0) e.setAttribute ("group", d.group);
        for (auto& in : d.inserts)
        {
            auto* x = e.createNewChildElement ("INSERT");
            x->setAttribute ("slot", in.slot);
            x->setAttribute ("bypass", in.bypass);
            x->setAttribute ("isolated", in.isolated);
            if (in.offline) x->setAttribute ("offline", true);
            if (in.mix < 0.999f) x->setAttribute ("mix", in.mix);
            if (in.sidechain >= 0) x->setAttribute ("sidechain", in.sidechain);
            if (auto desc = in.desc.createXml()) x->addChildElement (desc.release());
            x->createNewChildElement ("STATE")->addTextElement (in.state.toBase64Encoding());
        }
        auto* a = e.createNewChildElement ("AUTOMATION");
        a->setAttribute ("mode", (int) d.autoMode);
        for (auto& l : d.lanes)
        {
            auto* x = a->createNewChildElement ("LANE");
            x->setAttribute ("target", (int) l.target);
            x->setAttribute ("slot", l.slot);
            x->setAttribute ("param", l.param);
            x->setAttribute ("step", l.step);
            juce::String pts;
            pts.preallocateBytes ((size_t) l.points.size() * 20);
            for (auto& p : l.points) pts << juce::String (p.t, 4) << ',' << juce::String (p.v, 5) << ' ';
            x->addTextElement (pts.trimEnd());
        }
    }

    void readChannel (const juce::XmlElement& e, ChannelData& d)
    {
        d.name = e.getStringAttribute ("name");
        d.colour = juce::Colour::fromString (e.getStringAttribute ("colour", "ff7fc8ff"));
        d.gainDb = juce::jlimit (kMinDb, 12.0f, (float) e.getDoubleAttribute ("gainDb", 0.0));
        d.pan = juce::jlimit (-1.0f, 1.0f, (float) e.getDoubleAttribute ("pan", 0.0));
        d.mute = e.getBoolAttribute ("mute");
        d.solo = e.getBoolAttribute ("solo");
        d.phase = e.getBoolAttribute ("phase");
        d.group = e.getIntAttribute ("group", -1);
        if (! juce::isPositiveAndBelow (d.group, kMaxGroups)) d.group = -1;
        for (auto* x : e.getChildWithTagNameIterator ("INSERT"))
        {
            InsertData in;
            in.slot = x->getIntAttribute ("slot", -1);
            in.bypass = x->getBoolAttribute ("bypass");
            in.isolated = x->getBoolAttribute ("isolated", false);
            in.offline = x->getBoolAttribute ("offline", false);
            in.mix = juce::jlimit (0.0f, 1.0f, (float) x->getDoubleAttribute ("mix", 1.0));
            in.sidechain = x->getIntAttribute ("sidechain", -1);
            if (! juce::isPositiveAndBelow (in.slot, kMaxInsertsPerChannel)) continue;
            if (auto* p = x->getChildByName ("PLUGIN")) in.hasDesc = in.desc.loadFromXml (*p);
            if (auto* s = x->getChildByName ("STATE")) in.state.fromBase64Encoding (s->getAllSubText().trim());
            if (in.hasDesc) d.inserts.push_back (std::move (in));
        }
        if (auto* a = e.getChildByName ("AUTOMATION"))
        {
            d.autoMode = (AutoMode) juce::jlimit (0, 4, a->getIntAttribute ("mode", (int) AutoMode::read));
            for (auto* x : a->getChildWithTagNameIterator ("LANE"))
            {
                LaneData l;
                l.target = (AutoTarget) juce::jlimit (0, 3, x->getIntAttribute ("target"));
                l.slot = x->getIntAttribute ("slot", -1);
                l.param = x->getIntAttribute ("param", -1);
                l.step = x->getBoolAttribute ("step");
                if (l.target == AutoTarget::plugin && (! juce::isPositiveAndBelow (l.slot, kMaxInsertsPerChannel) || l.param < 0)) continue;
                auto tokens = juce::StringArray::fromTokens (x->getAllSubText(), " \n\r\t", "");
                for (auto& tok : tokens)
                {
                    const auto t = tok.upToFirstOccurrenceOf (",", false, false).getDoubleValue();
                    const auto v = (float) tok.fromFirstOccurrenceOf (",", false, false).getDoubleValue();
                    if (std::isfinite (t) && std::isfinite (v) && t >= 0.0 && l.points.size() < 2000000)
                        l.points.push_back ({ t, v });
                }
                std::sort (l.points.begin(), l.points.end(), [] (const AutoPoint& p, const AutoPoint& q) { return p.t < q.t; });
                if (! l.points.empty()) d.lanes.push_back (std::move (l));
            }
        }
        // Numero de slots: o salvo (projetos antigos: padrao), sempre cobrindo
        // os slots com plugin ou com linha de automacao.
        d.numInserts = juce::jlimit (1, kMaxInsertsPerChannel, e.getIntAttribute ("inserts", kDefaultInserts));
        for (auto& in : d.inserts) d.numInserts = juce::jmax (d.numInserts, in.slot + 1);
        for (auto& l : d.lanes) if (l.target == AutoTarget::plugin) d.numInserts = juce::jmax (d.numInserts, l.slot + 1);
    }
}

ProjectData captureProject (Engine& engine)
{
    ProjectData p;
    for (auto& t : engine.tracks)
    {
        TrackData td;
        td.ch = captureChannel (*t, engine.tracks);
        td.file = t->sourceFile;
        td.frozenFile = t->frozenFile;
        td.output = t->outputBus.load();
        td.sends.clear();
        for (auto& s : t->sends)
            td.sends.push_back ({ s.bus.load(), s.levelDb.load(), s.preFader.load(), s.preFx.load() });
        p.tracks.push_back (std::move (td));
    }
    for (auto& b : engine.buses)
    {
        p.buses.push_back (captureChannel (*b, engine.tracks));
        std::vector<SendData> sends;
        for (auto& s : b->sends) sends.push_back ({ s.bus.load(), s.levelDb.load(), s.preFader.load(), false });
        p.busSends.push_back (std::move (sends));
    }
    p.master = captureChannel (engine.master, engine.tracks);
    p.masterComp = CompData::from (engine.master.comp);
    p.masterMono = engine.master.mono.load();
    for (int g = 0; g < kMaxGroups; ++g)
        p.vca[(size_t) g] = { engine.vca[(size_t) g].gainDb.load(), engine.vca[(size_t) g].mute.load(), engine.vca[(size_t) g].shown };
    const double sr = engine.getSampleRate();
    p.loopOn = engine.loopOn.load();
    p.loopStartSec = (double) engine.loopStart.load() / sr;
    p.loopEndSec = (double) engine.loopEnd.load() / sr;
    return p;
}

void applyChannelBasics (Engine& engine, Channel& c, const ChannelData& d)
{
    std::vector<std::unique_ptr<AutoLane>> lanes;
    for (auto& ld : d.lanes)
    {
        auto l = std::make_unique<AutoLane>();
        l->target = ld.target; l->slot = ld.slot; l->param = ld.param; l->step = ld.step; l->points = ld.points;
        lanes.push_back (std::move (l));
    }
    engine.editAutomation ([&] { c.lanes = std::move (lanes); });
    engine.setInsertCount (c, d.numInserts);
    c.autoMode = (int) d.autoMode;
    c.name = d.name;
    c.colour = d.colour;
    c.gainDb = d.gainDb;
    c.pan = d.pan;
    c.mute = d.mute;
    c.solo = d.solo;
    c.phase = d.phase;
    c.group = d.group;
}

namespace
{
    void writeSends (juce::XmlElement& e, const std::vector<SendData>& sends)
    {
        e.setAttribute ("sends", (int) sends.size());
        for (int i = 0; i < (int) sends.size(); ++i)
        {
            const auto& s = sends[(size_t) i];
            if (s.bus < 0) continue;
            auto* x = e.createNewChildElement ("SEND");
            x->setAttribute ("index", i);
            x->setAttribute ("bus", s.bus);
            x->setAttribute ("levelDb", s.levelDb);
            x->setAttribute ("pre", s.pre);
            if (s.preFx) x->setAttribute ("preFx", true);
        }
    }

    std::vector<SendData> readSends (const juce::XmlElement& e, int defaultCount, const std::function<int (int)>& validBus)
    {
        std::vector<SendData> sends ((size_t) juce::jlimit (1, kMaxSendsPerTrack, e.getIntAttribute ("sends", defaultCount)));
        for (auto* x : e.getChildWithTagNameIterator ("SEND"))
        {
            const int i = x->getIntAttribute ("index", -1);
            if (! juce::isPositiveAndBelow (i, kMaxSendsPerTrack)) continue;
            if (i >= (int) sends.size()) sends.resize ((size_t) i + 1);
            sends[(size_t) i] = { validBus (x->getIntAttribute ("bus", -1)),
                                  juce::jlimit (kMinDb, 12.0f, (float) x->getDoubleAttribute ("levelDb", 0.0)),
                                  x->getBoolAttribute ("pre"), x->getBoolAttribute ("preFx", false) };
        }
        return sends;
    }
}

void applySends (Engine& engine, Channel& c, const std::vector<SendData>& sends)
{
    engine.setSendCount (c, (int) sends.size());
    for (int s = 0; s < (int) sends.size() && s < c.sends.size(); ++s)
    {
        c.sends[(size_t) s].bus = sends[(size_t) s].bus;
        c.sends[(size_t) s].levelDb = sends[(size_t) s].levelDb;
        c.sends[(size_t) s].preFader = sends[(size_t) s].pre;
        c.sends[(size_t) s].preFx = c.kind == ChannelKind::track && sends[(size_t) s].preFx;
    }
}

bool writeProject (const ProjectData& p, const juce::File& file, juce::String& error)
{
    juce::XmlElement root ("SILVERCONSOLE");
    root.setAttribute ("version", 1);
    root.setAttribute ("loopOn", p.loopOn);
    root.setAttribute ("loopStart", p.loopStartSec);
    root.setAttribute ("loopEnd", p.loopEndSec);

    const auto dir = file.getParentDirectory();
    for (auto& t : p.tracks)
    {
        auto* e = root.createNewChildElement ("TRACK");
        writeChannel (*e, t.ch);
        e->setAttribute ("file", t.file.getFullPathName());
        if (t.file != juce::File())
            e->setAttribute ("relFile", t.file.getRelativePathFrom (dir).replaceCharacter ('\\', '/'));
        if (t.frozenFile != juce::File())
        {
            e->setAttribute ("frozenFile", t.frozenFile.getFullPathName());
            e->setAttribute ("relFrozenFile", t.frozenFile.getRelativePathFrom (dir).replaceCharacter ('\\', '/'));
        }
        e->setAttribute ("output", t.output);
        writeSends (*e, t.sends);
    }
    for (size_t bi = 0; bi < p.buses.size(); ++bi)
    {
        auto* e = root.createNewChildElement ("BUS");
        writeChannel (*e, p.buses[bi]);
        if (bi < p.busSends.size()) writeSends (*e, p.busSends[bi]);
    }
    auto* m = root.createNewChildElement ("MASTER");
    writeChannel (*m, p.master);
    m->setAttribute ("mono", p.masterMono);
    {
        const auto& c = p.masterComp;
        auto* x = m->createNewChildElement ("BUSCOMP");
        x->setAttribute ("on", c.on);
        x->setAttribute ("threshold", c.thresholdDb);
        x->setAttribute ("ratio", c.ratio);
        x->setAttribute ("attack", c.attack);
        x->setAttribute ("release", c.release);
        x->setAttribute ("makeup", c.makeupDb);
        x->setAttribute ("mix", c.mix);
        x->setAttribute ("hpf", c.hpf);
    }
    for (int g = 0; g < kMaxGroups; ++g)
    {
        const auto& v = p.vca[(size_t) g];
        if (v.gainDb == 0.0f && ! v.mute && ! v.shown) continue;
        auto* x = root.createNewChildElement ("VCA");
        x->setAttribute ("group", g);
        x->setAttribute ("gainDb", v.gainDb);
        x->setAttribute ("mute", v.mute);
        x->setAttribute ("shown", v.shown);
    }
    for (auto& mk : p.markers)
    {
        auto* x = root.createNewChildElement ("MARKER");
        x->setAttribute ("t", mk.t);
        x->setAttribute ("name", mk.name);
    }
    for (auto& rg : p.regions)
    {
        auto* x = root.createNewChildElement ("REGION");
        x->setAttribute ("start", rg.start);
        x->setAttribute ("end", rg.end);
        x->setAttribute ("name", rg.name);
    }

    juce::TemporaryFile temp (file);
    if (! root.writeTo (temp.getFile()) || ! temp.overwriteTargetFileWithTemporary())
    {
        error = "Nao foi possivel salvar em " + file.getFullPathName();
        return false;
    }
    return true;
}

bool readProject (const juce::File& file, ProjectData& p, juce::String& error)
{
    auto root = juce::XmlDocument::parse (file);
    if (root == nullptr || ! root->hasTagName ("SILVERCONSOLE"))
    {
        error = "Arquivo de projeto invalido: " + file.getFileName();
        return false;
    }
    p = {};
    p.loopOn = root->getBoolAttribute ("loopOn");
    p.loopStartSec = root->getDoubleAttribute ("loopStart");
    p.loopEndSec = root->getDoubleAttribute ("loopEnd");

    const auto dir = file.getParentDirectory();
    for (auto* e : root->getChildWithTagNameIterator ("BUS"))
    {
        if ((int) p.buses.size() >= kMaxBuses) break;
        ChannelData d;
        readChannel (*e, d);
        p.buses.push_back (std::move (d));
    }
    const int numBuses = (int) p.buses.size();
    auto validBus = [numBuses] (int b) { return b >= 0 && b < numBuses ? b : -1; };
    {
        int bi = 0;
        for (auto* e : root->getChildWithTagNameIterator ("BUS"))
        {
            if (bi >= numBuses) break;
            const int self = bi;
            auto sends = readSends (*e, kDefaultBusSends, [&validBus, self] (int b) { return b == self ? -1 : validBus (b); });
            for (auto& s : sends) s.preFx = false; // pre-FX so existe nas faixas
            p.busSends.push_back (std::move (sends));
            ++bi;
        }
    }

    for (auto* e : root->getChildWithTagNameIterator ("TRACK"))
    {
        if ((int) p.tracks.size() >= kMaxTracks) break;
        TrackData t;
        readChannel (*e, t.ch);
        const auto rel = e->getStringAttribute ("relFile");
        const auto abs = e->getStringAttribute ("file");
        if (rel.isNotEmpty() && dir.getChildFile (rel).existsAsFile()) t.file = dir.getChildFile (rel);
        else if (juce::File::isAbsolutePath (abs)) t.file = juce::File (abs);
        const auto relFrozen = e->getStringAttribute ("relFrozenFile");
        const auto absFrozen = e->getStringAttribute ("frozenFile");
        if (relFrozen.isNotEmpty() && dir.getChildFile (relFrozen).existsAsFile()) t.frozenFile = dir.getChildFile (relFrozen);
        else if (juce::File::isAbsolutePath (absFrozen)) t.frozenFile = juce::File (absFrozen);
        t.output = validBus (e->getIntAttribute ("output", -1));
        t.sends = readSends (*e, kDefaultSends, validBus);
        p.tracks.push_back (std::move (t));
    }
    if (auto* m = root->getChildByName ("MASTER"))
    {
        readChannel (*m, p.master);
        p.masterMono = m->getBoolAttribute ("mono");
        if (auto* x = m->getChildByName ("BUSCOMP"))
        {
            auto& c = p.masterComp;
            c.on = x->getBoolAttribute ("on");
            c.thresholdDb = juce::jlimit (-30.0f, 10.0f, (float) x->getDoubleAttribute ("threshold", -10.0));
            c.ratio = juce::jlimit (0, BusCompParams::numRatios - 1, x->getIntAttribute ("ratio", 1));
            c.attack = juce::jlimit (0, BusCompParams::numAttacks - 1, x->getIntAttribute ("attack", 2));
            c.release = juce::jlimit (0, BusCompParams::numReleases - 1, x->getIntAttribute ("release", 4));
            c.makeupDb = juce::jlimit (0.0f, 20.0f, (float) x->getDoubleAttribute ("makeup", 0.0));
            c.mix = juce::jlimit (0.0f, 1.0f, (float) x->getDoubleAttribute ("mix", 1.0));
            c.hpf = juce::jlimit (0, BusCompParams::numHpf - 1, x->getIntAttribute ("hpf", 0));
        }
    }
    for (auto* x : root->getChildWithTagNameIterator ("VCA"))
    {
        const int g = x->getIntAttribute ("group", -1);
        if (! juce::isPositiveAndBelow (g, kMaxGroups)) continue;
        p.vca[(size_t) g] = { juce::jlimit (kMinDb, 12.0f, (float) x->getDoubleAttribute ("gainDb", 0.0)),
                              x->getBoolAttribute ("mute"), x->getBoolAttribute ("shown") };
    }
    for (auto* x : root->getChildWithTagNameIterator ("MARKER"))
        p.markers.push_back ({ juce::jmax (0.0, x->getDoubleAttribute ("t")), x->getStringAttribute ("name") });
    for (auto* x : root->getChildWithTagNameIterator ("REGION"))
    {
        RegionData r { juce::jmax (0.0, x->getDoubleAttribute ("start")), x->getDoubleAttribute ("end"), x->getStringAttribute ("name") };
        if (r.end > r.start) p.regions.push_back (r);
    }
    p.master.name = "MASTER";
    return true;
}
} // namespace sc
