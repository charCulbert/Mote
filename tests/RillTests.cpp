#include "Plugin.h"
#include "Presets.h"
#include <clap/ext/preset-load.h>
#include <clap/factory/preset-discovery.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#define CHECK(...) do { if (!(__VA_ARGS__)) { std::fprintf(stderr, "Failed at %d: %s\n", __LINE__, #__VA_ARGS__); std::abort(); } } while (false)
namespace
{
const clap_host_t host { CLAP_VERSION, nullptr, "Mote tests", "", "", "1",
    [](const clap_host_t*, const char*) -> const void* { return nullptr; },
    [](const clap_host_t*) {}, [](const clap_host_t*) {}, [](const clap_host_t*) {} };
const clap_host_t auv3Host { CLAP_VERSION, nullptr, "mote 1.0.0 (CLAP-as-AUv3)", "", "", "1",
    [](const clap_host_t*, const char*) -> const void* { return nullptr; },
    [](const clap_host_t*) {}, [](const clap_host_t*) {}, [](const clap_host_t*) {} };
struct Event
{
    uint32_t time;
    int type, key, channel, id;
    double velocity;
    bool operator==(const Event& other) const
    {
        return time == other.time && type == other.type && key == other.key
            && channel == other.channel && id == other.id && velocity == other.velocity;
    }
};
struct Output
{
    std::vector<Event> events;
    int rejectOffs = 0;
    clap_output_events_t list { this, [](const clap_output_events_t* list, const clap_event_header_t* h) {
        auto& out = *static_cast<Output*>(list->ctx);
        if (h->type == CLAP_EVENT_NOTE_ON || h->type == CLAP_EVENT_NOTE_OFF)
        {
            const auto& n = *reinterpret_cast<const clap_event_note_t*>(h);
            if (h->type == CLAP_EVENT_NOTE_OFF && out.rejectOffs-- > 0) return false;
            out.events.push_back({ h->time, h->type == CLAP_EVENT_NOTE_ON ? 0x90 : 0x80, n.key, n.channel, n.note_id, n.velocity });
        }
        else if (h->type == CLAP_EVENT_MIDI)
        {
            const auto& n = *reinterpret_cast<const clap_event_midi_t*>(h);
            out.events.push_back({ h->time, n.data[0] & 0xf0, n.data[1], n.data[0] & 15, -1, n.data[2] / 127.0 });
        }
        return true;
    } };
};
struct Input
{
    std::vector<const clap_event_header_t*> events;
    clap_input_events_t list { this,
        [](const clap_input_events_t* l) { return static_cast<uint32_t>(static_cast<const Input*>(l->ctx)->events.size()); },
        [](const clap_input_events_t* l, uint32_t i) { return static_cast<const Input*>(l->ctx)->events[i]; } };
};
clap_event_transport_t transport(double beat, bool playing = true, double tempo = 60)
{
    clap_event_transport_t t {};
    t.header = { sizeof(t), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_TRANSPORT, 0 };
    t.flags = CLAP_TRANSPORT_HAS_TEMPO | CLAP_TRANSPORT_HAS_BEATS_TIMELINE | (playing ? CLAP_TRANSPORT_IS_PLAYING : 0);
    t.song_pos_beats = static_cast<clap_beattime>(std::llround(beat * CLAP_BEATTIME_FACTOR));
    t.tempo = tempo;
    return t;
}
clap_event_note_t note(int key = 60, bool on = true, uint32_t time = 0, int id = 10)
{
    return { { sizeof(clap_event_note_t), time, CLAP_CORE_EVENT_SPACE_ID,
               static_cast<uint16_t>(on ? CLAP_EVENT_NOTE_ON : CLAP_EVENT_NOTE_OFF), 0 }, id, 0, 0, static_cast<int16_t>(key), on ? .8 : 0 };
}
clap_event_midi_t midi(uint8_t status, uint8_t key, uint8_t value, uint32_t time = 0)
{
    return { { sizeof(clap_event_midi_t), time, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI, 0 }, 0, { status, key, value } };
}
struct Plugin
{
    const clap_plugin_t* p;
    const clap_plugin_params_t* params;
    const clap_plugin_state_t* state;
    explicit Plugin(const clap_host_t* testHost = &host)
    {
        const auto* factory = static_cast<const clap_plugin_factory_t*>(rill::entryGetFactory(CLAP_PLUGIN_FACTORY_ID));
        p = factory->create_plugin(factory, testHost, rill::pluginId);
        CHECK(p && p->init(p));
        params = static_cast<const clap_plugin_params_t*>(p->get_extension(p, CLAP_EXT_PARAMS));
        state = static_cast<const clap_plugin_state_t*>(p->get_extension(p, CLAP_EXT_STATE));
        CHECK(p->activate(p, 1000, 1, 32000));
        CHECK(p->start_processing(p));
    }
    ~Plugin() { p->stop_processing(p); p->deactivate(p); p->destroy(p); }
    void set(clap_id id, double value)
    {
        clap_event_param_value_t event { { sizeof(event), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0 }, id, nullptr, -1, -1, -1, -1, value };
        Input in {{ &event.header }};
        params->flush(p, &in.list, nullptr);
    }
    void preset(const char* key)
    {
        const auto* ext = static_cast<const clap_plugin_preset_load_t*>(p->get_extension(p, CLAP_EXT_PRESET_LOAD));
        CHECK(ext && ext->from_location(p, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr, key));
    }
    void run(uint32_t frames, const clap_event_transport_t* clock, Output& output, Input input = {},
             clap_audio_buffer_t* audioOutput = nullptr)
    {
        clap_process_t block {};
        block.frames_count = frames; block.transport = clock; block.in_events = &input.list; block.out_events = &output.list;
        block.audio_outputs = audioOutput;
        block.audio_outputs_count = audioOutput ? 1 : 0;
        CHECK(p->process(p, &block) == CLAP_PROCESS_CONTINUE);
        for (const auto& e : output.events) CHECK(e.time < frames);
    }
};
std::vector<Event> render(uint32_t blockSize)
{
    Plugin p;
    p.set(rill::rate, 0);
    const auto a = note(), b = note(64, true, 0, 11), c = note(67, true, 0, 12);
    std::vector<Event> result;
    for (uint32_t frame = 0; frame < 16000; frame += blockSize)
    {
        const auto clock = transport(frame / 1000.0);
        Output out;
        p.run(std::min(blockSize, 16000 - frame), &clock, out, frame == 0 ? Input{{ &a.header, &b.header, &c.header }} : Input{});
        for (auto e : out.events) { e.time += frame; result.push_back(e); }
    }
    return result;
}
void timingAndPhrase()
{
    auto expected = render(16000);
    CHECK(expected == render(127));
    CHECK(expected == render(512));
    std::vector<uint32_t> times;
    std::vector<int> keys;
    for (auto& e : expected) if (e.type == 0x90) { times.push_back(e.time); keys.push_back(e.key); }
    CHECK(times.size() == 16);
    for (size_t i = 0; i < times.size(); ++i)
    {
        CHECK(times[i] == i * 1000);
        CHECK(keys[i] == std::array<int, 3>{ 60, 64, 67 }[i % 3]);
    }
    for (size_t i = 0; i < expected.size(); i += 2)
    {
        CHECK(expected[i + 1].time == expected[i].time + 650);
        CHECK(expected[i + 1].id == expected[i].id);
    }
}
void tempoChange()
{
    Plugin p; p.preset("up"); p.set(rill::rate, 0);
    auto clock = transport(0); auto n = note(); Output out;
    p.run(250, &clock, out, Input{{ &n.header }});
    clock = transport(.25, true, 120); out.events.clear();
    p.run(251, &clock, out);
    CHECK(out.events.size() == 1 && out.events[0].type == 0x80 && out.events[0].time == 200);

}
void fractionalTempo()
{
    Plugin p;
    p.set(rill::rate, 0);
    const auto clock = transport(0, true, 123.456);
    const auto n = note();
    Output out;
    p.run(5000, &clock, out, Input{{ &n.header }});

    std::vector<uint32_t> onsets;
    for (const auto& event : out.events)
        if (event.type == 0x90) onsets.push_back(event.time);
    CHECK(onsets.size() == 11);
    for (size_t i = 0; i < onsets.size(); ++i)
        CHECK(onsets[i] == static_cast<uint32_t>(std::ceil(i * 60000.0 / 123.456)));
}
void noteOrder()
{
    const std::array<std::vector<int>, 3> expected {{
        { 60, 64, 67, 60, 64, 67 }, { 67, 64, 60, 67, 64, 60 }, { 60, 64, 67, 64, 60, 64 }
    }};
    for (int order = 0; order < 3; ++order)
    {
        Plugin p; p.set(rill::direction, order);
        const auto a = note(67), b = note(60, true, 0, 11), c = note(64, true, 0, 12);
        auto clock = transport(0); Output out;
        p.run(3000, &clock, out, Input{{ &a.header, &b.header, &c.header }});
        std::vector<int> keys;
        for (auto e : out.events) if (e.type == 0x90) keys.push_back(e.key);
        CHECK(keys == expected[order]);
        p.set(rill::direction, (order + 1) % 3);
        clock = transport(3); out.events.clear(); p.run(1, &clock, out);
        CHECK(out.events.front().key == expected[(order + 1) % 3].front());
    }
    Plugin single; single.set(rill::direction, 2);
    const auto on = note(), off = note(60, false, 1100);
    auto clock = transport(0); Output out;
    single.run(2000, &clock, out, Input{{ &on.header, &off.header }});
    CHECK(out.events.size() == 6);
    for (auto e : out.events) CHECK(e.key == 60);
    CHECK(out.events.back().type == 0x80 && out.events.back().time == 1100);
}
void octaveAndGate()
{
    Plugin p;
    p.set(rill::rate, 0);
    p.set(rill::octaves, 2);
    p.set(rill::gate, 25);
    const auto a = note(), b = note(64, true, 0, 11);
    const auto clock = transport(0); Output out;
    p.run(4000, &clock, out, Input{{ &a.header, &b.header }});
    CHECK(out.events.size() == 8);
    const std::array<int, 4> keys { 60, 64, 72, 76 };
    for (size_t i = 0; i < keys.size(); ++i)
    {
        CHECK(out.events[i * 2].type == 0x90 && out.events[i * 2].key == keys[i]);
        CHECK(out.events[i * 2].time == i * 1000);
        CHECK(out.events[i * 2 + 1].type == 0x80 && out.events[i * 2 + 1].time == i * 1000 + 250);
    }
}
void latchAndMidi()
{
    Plugin p; p.preset("up"); p.set(rill::latch, 1);
    auto on = midi(0x93, 60, 100), off = midi(0x93, 60, 0, 10);
    auto clock = transport(0); Output out;
    p.run(600, &clock, out, Input{{ &on.header, &off.header }});
    CHECK(out.events.size() == 3 && out.events[2].type == 0x90 && out.events[2].channel == 3);
    on = midi(0x93, 67, 100); off = midi(0x83, 67, 0, 10);
    clock = transport(.6); out.events.clear();
    p.run(600, &clock, out, Input{{ &on.header, &off.header }});
    CHECK(out.events.back().type == 0x90 && out.events.back().key == 67);
    p.set(rill::latch, 0); clock = transport(1.2); out.events.clear();
    p.run(1100, &clock, out);
    CHECK(out.events.size() == 1 && out.events[0].type == 0x80 && out.events[0].time == 0);

    Plugin pedal; pedal.preset("up");
    auto sustain = midi(0xb3, 64, 127), bend = midi(0xe3, 0, 70, 20);
    on = midi(0x93, 60, 100); off = midi(0x83, 60, 0, 10);
    clock = transport(0); out.events.clear();
    pedal.run(600, &clock, out, Input{{ &sustain.header, &on.header, &off.header, &bend.header }});
    CHECK(out.events.back().type == 0x90);
    CHECK(out.events[1].type == 0xe0 && out.events[1].time == 20);
    auto panic = midi(0xb3, 123, 0);
    clock = transport(.6); out.events.clear();
    pedal.run(600, &clock, out, Input{{ &panic.header }});
    CHECK(out.events.size() == 2 && out.events[0].type == 0x80 && out.events[1].type == 0xb0);
}
void transportAndRelease()
{
    Plugin p; p.preset("up"); auto n = note(); Output out;
    p.run(1, nullptr, out, Input{{ &n.header }}); CHECK(out.events.empty());
    auto stopped = transport(0, false), start = transport(0); start.header.time = 100;
    p.run(200, &stopped, out, Input{{ &start.header }});
    CHECK(out.events.size() == 1 && out.events[0].time == 100);
    stopped = transport(.1, false); out.events.clear(); out.rejectOffs = 2;
    p.run(5, &stopped, out);
    CHECK(out.events.size() == 1 && out.events[0].type == 0x80);
    auto running = transport(0); out.events.clear(); p.run(1, &running, out);
    CHECK(out.events.size() == 1 && out.events[0].type == 0x90);
    running = transport(8); out.events.clear(); p.run(1, &running, out);
    CHECK(out.events.size() == 2 && out.events[0].type == 0x80 && out.events[1].type == 0x90);
    n = note(-1, false, 0, -1); n.channel = -1;
    running = transport(8.001); out.events.clear(); p.run(1000, &running, out, Input{{ &n.header }});
    CHECK(out.events.size() == 1 && out.events[0].type == 0x80);
}
void auv3RenderClock()
{
    Plugin p(&auv3Host);
    const auto* ports = static_cast<const clap_plugin_audio_ports_t*>(p.p->get_extension(p.p, CLAP_EXT_AUDIO_PORTS));
    CHECK(ports && ports->count(p.p, true) == 0 && ports->count(p.p, false) == 1);
    clap_audio_port_info_t info {};
    CHECK(ports->get(p.p, 0, false, &info));
    CHECK(info.channel_count == 2 && std::strcmp(info.port_type, CLAP_PORT_STEREO) == 0);

    std::array<float, 16> left, right;
    left.fill(1); right.fill(-1);
    std::array<float*, 2> channels { left.data(), right.data() };
    clap_audio_buffer_t audio { channels.data(), nullptr, 2, 0, 0 };
    const auto clock = transport(0); Output out;
    p.run(16, &clock, out, {}, &audio);
    CHECK(std::all_of(left.begin(), left.end(), [](float sample) { return sample == 0; }));
    CHECK(std::all_of(right.begin(), right.end(), [](float sample) { return sample == 0; }));
}
void presetsAndState()
{
    Plugin p;
    CHECK(p.params->count(p.p) == rill::parameters.size());
    for (const auto& preset : rill::presets)
    {
        p.preset(preset.key);
        for (uint32_t index = 0; index < rill::parameters.size(); ++index)
        {
            const auto id = rill::parameters[index].id;
            double value = -1, parsed = -1; char text[32]; clap_param_info_t info;
            CHECK(p.params->get_info(p.p, index, &info) && info.id == id);
            CHECK(p.params->get_value(p.p, id, &value) && value == preset.values[id]);
            CHECK(p.params->value_to_text(p.p, id, value, text, sizeof(text)));
            CHECK(p.params->text_to_value(p.p, id, text, &parsed) && parsed == value);
        }
    }
    CHECK(!rill::findParameter(2));
    std::vector<char> data;
    const clap_ostream_t writer { &data, [](const clap_ostream_t* s, const void* bytes, uint64_t size) -> int64_t {
        auto& data = *static_cast<std::vector<char>*>(s->ctx);
        auto* first = static_cast<const char*>(bytes);
        data.insert(data.end(), first, first + size); return size;
    } };
    CHECK(p.state->save(p.p, &writer));
    p.preset("up");
    struct Read { std::vector<char>& data; size_t offset = 0; } read { data };
    const clap_istream_t reader { &read, [](const clap_istream_t* s, void* bytes, uint64_t size) -> int64_t {
        auto& r = *static_cast<Read*>(s->ctx);
        const auto n = std::min<size_t>(size, r.data.size() - r.offset);
        std::memcpy(bytes, r.data.data() + r.offset, n); r.offset += n; return n;
    } };
    CHECK(p.state->load(p.p, &reader));
    double value; CHECK(p.params->get_value(p.p, rill::direction, &value) && value == 2);
    read.offset = 0; data[0] = 0; CHECK(!p.state->load(p.p, &reader));
    CHECK(p.params->get_value(p.p, rill::direction, &value) && value == 2);

    struct LegacyState {
        uint32_t magic = 0x52494c4c, version = 1;
        std::array<double, 10> values { 0, 16, 0, 7, 3, 4, 5, 50, 0, 1 };
    } legacy;
    data.resize(sizeof(legacy));
    std::memcpy(data.data(), &legacy, sizeof(legacy));
    read.offset = 0;
    CHECK(p.state->load(p.p, &reader));
    const auto a = note(), b = note(64, true, 0, 11), c = note(67, true, 0, 12);
    const auto clock = transport(0); Output out;
    p.run(3000, &clock, out, Input{{ &a.header, &b.header, &c.header }});
    CHECK(out.events.size() == 6);
    for (size_t i = 0; i < 3; ++i)
    {
        CHECK(out.events[i * 2].key == std::array<int, 3>{ 103, 100, 96 }[i]);
        CHECK(out.events[i * 2].time == i * 1000);
        CHECK(out.events[i * 2 + 1].time == i * 1000 + 50);
    }
}
}
int main()
{
    CHECK(rill::entryInit("."));
    CHECK(rill::entryGetFactory(CLAP_PRESET_DISCOVERY_FACTORY_ID));
    timingAndPhrase(); noteOrder(); octaveAndGate(); tempoChange(); fractionalTempo(); latchAndMidi(); transportAndRelease(); auv3RenderClock(); presetsAndState();
    rill::entryDeinit();
    std::puts("PASS: three orders, octave range, gate, single note, block invariance, tempo, latch, MIDI, panic, transport, AUv3 render clock, note-off retry, presets, state");
}
