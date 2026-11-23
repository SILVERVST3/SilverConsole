// SilverConsole - projeto (.sconsole, XML). Codigo original (MIT).
#include "Project.h"

namespace sc
{
namespace
{
    ChannelData captureChannel (Channel& c)
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
        for (int i = 0; i < c.inserts.size(); ++i)
        {
            auto& s = c.inserts[(size_t) i];
            if (s.isEmpty()) continue;
            InsertData in;
            in.slot = i;
            in.bypass = s.bypassed.load();
            in.hasDesc = s.hasDescription;
            in.isolated = s.preferRemote;
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
        for (auto& in : d.inserts)
        {
            auto* x = e.createNewChildElement ("INSERT");
            x->setAttribute ("slot", in.slot);
            x->setAttribute ("bypass", in.bypass);
            x->setAttribute ("isolated", in.isolated);
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
        for (auto* x : e.getChildWithTagNameIterator ("INSERT"))
        {
            InsertData in;
            in.slot = x->getIntAttribute ("slot", -1);
            in.bypass = x->getBoolAttribute ("bypass");
            in.isolated = x->getBoolAttribute ("isolated", false);
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
        td.ch = captureChannel (*t);
        td.file = t->sourceFile;
        td.output = t->outputBus.load();
        td.sends.clear();
        for (auto& s : t->sends)
            td.sends.push_back ({ s.bus.load(), s.levelDb.load(), s.preFader.load() });
        p.tracks.push_back (std::move (td));
    }
    for (auto& b : engine.buses) p.buses.push_back (captureChannel (*b));
    p.master = captureChannel (engine.master);
    p.masterMono = engine.master.mono.load();
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
        e->setAttribute ("output", t.output);
        e->setAttribute ("sends", (int) t.sends.size());
        for (int i = 0; i < (int) t.sends.size(); ++i)
        {
            const auto& s = t.sends[(size_t) i];
            if (s.bus < 0) continue;
            auto* x = e->createNewChildElement ("SEND");
            x->setAttribute ("index", i);
            x->setAttribute ("bus", s.bus);
            x->setAttribute ("levelDb", s.levelDb);
            x->setAttribute ("pre", s.pre);
        }
    }
    for (auto& b : p.buses) writeChannel (*root.createNewChildElement ("BUS"), b);
    auto* m = root.createNewChildElement ("MASTER");
    writeChannel (*m, p.master);
    m->setAttribute ("mono", p.masterMono);

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

    for (auto* e : root->getChildWithTagNameIterator ("TRACK"))
    {
        if ((int) p.tracks.size() >= kMaxTracks) break;
        TrackData t;
        readChannel (*e, t.ch);
        const auto rel = e->getStringAttribute ("relFile");
        const auto abs = e->getStringAttribute ("file");
        if (rel.isNotEmpty() && dir.getChildFile (rel).existsAsFile()) t.file = dir.getChildFile (rel);
        else if (juce::File::isAbsolutePath (abs)) t.file = juce::File (abs);
        t.output = validBus (e->getIntAttribute ("output", -1));
        t.sends.assign ((size_t) juce::jlimit (1, kMaxSendsPerTrack, e->getIntAttribute ("sends", kDefaultSends)), SendData {});
        for (auto* x : e->getChildWithTagNameIterator ("SEND"))
        {
            const int i = x->getIntAttribute ("index", -1);
            if (! juce::isPositiveAndBelow (i, kMaxSendsPerTrack)) continue;
            if (i >= (int) t.sends.size()) t.sends.resize ((size_t) i + 1);
            t.sends[(size_t) i] = { validBus (x->getIntAttribute ("bus", -1)),
                                    juce::jlimit (kMinDb, 12.0f, (float) x->getDoubleAttribute ("levelDb", 0.0)),
                                    x->getBoolAttribute ("pre") };
        }
        p.tracks.push_back (std::move (t));
    }
    if (auto* m = root->getChildByName ("MASTER"))
    {
        readChannel (*m, p.master);
        p.masterMono = m->getBoolAttribute ("mono");
    }
    p.master.name = "MASTER";
    return true;
}
} // namespace sc
