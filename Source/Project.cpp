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
        for (int i = 0; i < kNumInserts; ++i)
        {
            auto& s = c.inserts[(size_t) i];
            if (s.isEmpty()) continue;
            InsertData in;
            in.slot = i;
            in.bypass = s.bypassed.load();
            in.hasDesc = s.hasDescription;
            in.desc = s.description;
            if (s.processor != nullptr) s.processor->getStateInformation (in.state);
            else in.state = s.missingState;
            if (in.hasDesc) d.inserts.push_back (std::move (in));
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
        for (auto& in : d.inserts)
        {
            auto* x = e.createNewChildElement ("INSERT");
            x->setAttribute ("slot", in.slot);
            x->setAttribute ("bypass", in.bypass);
            if (auto desc = in.desc.createXml()) x->addChildElement (desc.release());
            x->createNewChildElement ("STATE")->addTextElement (in.state.toBase64Encoding());
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
            if (! juce::isPositiveAndBelow (in.slot, kNumInserts)) continue;
            if (auto* p = x->getChildByName ("PLUGIN")) in.hasDesc = in.desc.loadFromXml (*p);
            if (auto* s = x->getChildByName ("STATE")) in.state.fromBase64Encoding (s->getAllSubText().trim());
            if (in.hasDesc) d.inserts.push_back (std::move (in));
        }
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
        for (int i = 0; i < kNumSends; ++i)
        {
            auto& s = t->sends[(size_t) i];
            td.sends[(size_t) i] = { s.bus.load(), s.levelDb.load(), s.preFader.load() };
        }
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

void applyChannelBasics (Channel& c, const ChannelData& d)
{
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
        for (int i = 0; i < kNumSends; ++i)
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
        for (auto* x : e->getChildWithTagNameIterator ("SEND"))
        {
            const int i = x->getIntAttribute ("index", -1);
            if (! juce::isPositiveAndBelow (i, kNumSends)) continue;
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
