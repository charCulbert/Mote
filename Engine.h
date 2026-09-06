#pragma once
#include "Plugin.h"
#include <cstdint>
#include <limits>

namespace mote
{
// One generated voice, a fixed held-note bank, and a beat-domain gate. No allocation.
class Engine
{
public:
    static constexpr size_t capacity = 32;
    struct Note
    {
        int id = -1, channel = 0, key = -1;
        double velocity = 0;
        bool midi = false, down = false, used = false;
    };
    Values values {};
    std::array<Note, capacity> bank {};
    uint64_t revision = 0, emitted = 0;
    int tick = -1, pitchIndex = -1, outputKey = -1;
    bool playing = false, hasClock = false;
    double bpm = 120;

    Engine() { for (const auto& p : parameters) values[p.id] = p.initial; }

    void reset() noexcept
    {
        bank = {};
        sounding = {};
        sustain = {};
        lastTick = noTick;
        pitchCursor = 0;
        expectedBeat = 0;
        hadTransport = false;
        playing = hasClock = false;
        tick = pitchIndex = outputKey = -1;
        emitted = 0;
        ++revision;
    }

    void set(clap_id id, double value) noexcept
    {
        if (!findParameter(id)) return;
        const auto next = clampParameter(id, value);
        if (next == values[id]) return;
        values[id] = next;
        if (id == direction) pitchCursor = 0;
        if (id == latch && next == 0) releaseUnheld();
        ++revision;
    }

    void clear() noexcept
    {
        bank = {};
        sustain = {};
        pitchCursor = 0;
        ++revision;
    }

    void process(const clap_process_t& block, double sampleRate) noexcept
    {
        double beat = 0, tempoIncrement = 0;
        setTransport(block.transport, beat, tempoIncrement, block.out_events, 0);
        uint32_t input = 0;
        const auto count = block.in_events ? block.in_events->size(block.in_events) : 0;
        for (uint32_t frame = 0; frame < block.frames_count; ++frame)
        {
            while (input < count)
            {
                const auto* event = block.in_events->get(block.in_events, input);
                if (!event) { ++input; continue; }
                if (event->time > frame) break;
                ++input;
                if (event->space_id != CLAP_CORE_EVENT_SPACE_ID) continue;
                if (event->type == CLAP_EVENT_TRANSPORT && event->size >= sizeof(clap_event_transport_t))
                    setTransport(reinterpret_cast<const clap_event_transport_t*>(event), beat,
                                 tempoIncrement, block.out_events, frame);
                else
                    receive(*event, block.out_events, frame);
            }
            if (values[latch] == 0) releaseUnheld();
            if (sounding.used && (!playing || heldCount() == 0 || beat + epsilon >= offBeat))
                stop(block.out_events, frame);
            if (playing)
            {
                const auto division = beatDivisions[static_cast<size_t>(values[rate])];
                const auto absoluteTick = static_cast<int64_t>(std::floor((beat + epsilon) / division));
                if (absoluteTick != lastTick)
                {
                    lastTick = absoluteTick;
                    tick = modulo(absoluteTick, 16);
                    trigger(block.out_events, frame, beat, division);
                }
                beat += bpm / (60 * sampleRate);
                bpm = std::clamp(bpm + tempoIncrement, 1.0, 1000.0);
            }
        }
        expectedBeat = beat;
    }

    size_t heldCount() const noexcept
    {
        return static_cast<size_t>(std::count_if(bank.begin(), bank.end(), [](auto& n) { return n.used; }));
    }

private:
    static constexpr int64_t noTick = std::numeric_limits<int64_t>::min();
    static constexpr double epsilon = 4.0 / CLAP_BEATTIME_FACTOR;
    std::array<bool, 16> sustain {};
    Note sounding;
    double offBeat = 0, expectedBeat = 0;
    int64_t lastTick = noTick;
    uint64_t pitchCursor = 0;
    int32_t nextId = 1;
    bool hadTransport = false;

    static int modulo(int64_t value, int divisor) noexcept
    {
        return static_cast<int>((value % divisor + divisor) % divisor);
    }

    void setTransport(const clap_event_transport_t* transport, double& beat, double& increment,
                      const clap_output_events_t* output, uint32_t frame) noexcept
    {
        constexpr auto required = CLAP_TRANSPORT_HAS_TEMPO | CLAP_TRANSPORT_HAS_BEATS_TIMELINE;
        const bool clock = transport && (transport->flags & required) == required
            && std::isfinite(transport->tempo) && transport->tempo > 0 && transport->tempo <= 1000;
        const bool runs = clock && (transport->flags & CLAP_TRANSPORT_IS_PLAYING);
        const auto nextBeat = clock ? static_cast<double>(transport->song_pos_beats) / CLAP_BEATTIME_FACTOR : 0;
        if (runs != playing || clock != hasClock || (clock && bpm != transport->tempo)
            || (runs && hadTransport && std::abs(nextBeat - (frame == 0 ? expectedBeat : beat)) > .01))
            ++revision;
        const bool seek = runs && hadTransport && std::abs(nextBeat - (frame == 0 ? expectedBeat : beat)) > .01;
        if (!runs || !playing || seek)
        {
            stop(output, frame);
            lastTick = noTick;
            pitchCursor = 0;
            tick = -1;
        }
        playing = runs;
        hasClock = clock;
        if (clock) bpm = transport->tempo;
        beat = nextBeat;
        increment = clock && std::isfinite(transport->tempo_inc) ? transport->tempo_inc : 0;
        hadTransport = clock;
    }

    static bool matches(const Note& note, const Note& match) noexcept
    {
        return note.used && note.midi == match.midi
            && (match.id < 0 || note.id == match.id)
            && (match.channel < 0 || note.channel == match.channel)
            && (match.key < 0 || note.key == match.key);
    }

    void noteOn(Note note) noexcept
    {
        if (note.channel < 0 || note.channel > 15 || note.key < 0 || note.key > 127
            || !std::isfinite(note.velocity)) return;
        // A new gesture replaces a latched chord after every physical key is up.
        if (values[latch] != 0 && std::none_of(bank.begin(), bank.end(), [](auto& n) { return n.used && n.down; }))
            clear();
        if (heldCount() == 0) pitchCursor = 0;
        auto found = std::find_if(bank.begin(), bank.end(), [&](auto& n) { return matches(n, note); });
        if (found == bank.end()) found = std::find_if(bank.begin(), bank.end(), [](auto& n) { return !n.used; });
        if (found == bank.end()) return;
        note.down = note.used = true;
        note.velocity = std::clamp(note.velocity, 0.0, 1.0);
        *found = note;
        ++revision;
    }

    void noteOff(const Note& note, bool choke) noexcept
    {
        for (auto& n : bank)
            if (matches(n, note))
            {
                n.down = false;
                if (choke || (values[latch] == 0 && !sustain[static_cast<size_t>(n.channel)])) n.used = false;
                ++revision;
            }
    }

    void releaseUnheld() noexcept
    {
        for (auto& n : bank)
            if (n.used && !n.down && !sustain[static_cast<size_t>(n.channel)])
            {
                n.used = false;
                ++revision;
            }
    }

    void receive(const clap_event_header_t& header, const clap_output_events_t* output, uint32_t frame) noexcept
    {
        if (header.type == CLAP_EVENT_PARAM_VALUE && header.size >= sizeof(clap_event_param_value_t))
        {
            const auto& p = reinterpret_cast<const clap_event_param_value_t&>(header);
            if (p.note_id < 0 && p.port_index < 0 && p.channel < 0 && p.key < 0) set(p.param_id, p.value);
        }
        else if ((header.type == CLAP_EVENT_NOTE_ON || header.type == CLAP_EVENT_NOTE_OFF || header.type == CLAP_EVENT_NOTE_CHOKE)
                 && header.size >= sizeof(clap_event_note_t))
        {
            const auto& n = reinterpret_cast<const clap_event_note_t&>(header);
            if (n.port_index > 0) return;
            const Note note { n.note_id, n.channel, n.key, n.velocity, false };
            if (header.type == CLAP_EVENT_NOTE_ON) noteOn(note);
            else noteOff(note, header.type == CLAP_EVENT_NOTE_CHOKE);
        }
        else if (header.type == CLAP_EVENT_MIDI && header.size >= sizeof(clap_event_midi_t))
        {
            const auto& m = reinterpret_cast<const clap_event_midi_t&>(header);
            if (m.port_index != 0) return;
            const auto type = m.data[0] & 0xf0;
            const auto channel = m.data[0] & 15;
            const Note note { -1, channel, m.data[1] & 127, (m.data[2] & 127) / 127.0, true };
            if (type == 0x90 && m.data[2] != 0) noteOn(note);
            else if (type == 0x80 || type == 0x90) noteOff(note, false);
            else if (type == 0xb0 && m.data[1] == 64)
            {
                sustain[channel] = m.data[2] >= 64;
                if (values[latch] == 0) releaseUnheld();
            }
            else
            {
                if (type == 0xb0 && (m.data[1] == 120 || m.data[1] == 123))
                {
                    for (auto& n : bank) if (n.channel == channel) n.used = false;
                    sustain[channel] = false;
                    if (sounding.channel == channel) stop(output, frame);
                    ++revision;
                }
                if (output) output->try_push(output, &header);
            }
        }
        else if (header.type != CLAP_EVENT_NOTE_END && header.type != CLAP_EVENT_NOTE_EXPRESSION)
        {
            if (output) output->try_push(output, &header);
        }
    }

    bool send(const Note& n, bool on, const clap_output_events_t* output, uint32_t frame) noexcept
    {
        if (!output) return false;
        if (n.midi)
        {
            const clap_event_midi_t event { { sizeof(event), frame, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI, 0 },
                0, { static_cast<uint8_t>((on ? 0x90 : 0x80) | n.channel), static_cast<uint8_t>(n.key),
                     static_cast<uint8_t>(on ? std::clamp(std::lround(n.velocity * 127), 1l, 127l) : 0) } };
            return output->try_push(output, &event.header);
        }
        const clap_event_note_t event { { sizeof(event), frame, CLAP_CORE_EVENT_SPACE_ID,
            static_cast<uint16_t>(on ? CLAP_EVENT_NOTE_ON : CLAP_EVENT_NOTE_OFF), 0 },
            n.id, 0, static_cast<int16_t>(n.channel), static_cast<int16_t>(n.key), on ? n.velocity : 0 };
        return output->try_push(output, &event.header);
    }

    void stop(const clap_output_events_t* output, uint32_t frame) noexcept
    {
        // Retain a rejected note-off and retry before issuing another note-on.
        if (sounding.used && send(sounding, false, output, frame))
        {
            sounding.used = false;
            outputKey = pitchIndex = -1;
            ++revision;
        }
    }

    void trigger(const clap_output_events_t* output, uint32_t frame, double beat, double division) noexcept
    {
        std::array<const Note*, capacity> sorted {};
        size_t count = 0;
        for (auto& n : bank) if (n.used) sorted[count++] = &n;
        if (count == 0) return;
        std::sort(sorted.begin(), sorted.begin() + count, [](auto* a, auto* b) {
            return a->key < b->key || (a->key == b->key && a->channel < b->channel);
        });
        stop(output, frame);
        if (sounding.used) return;
        const auto total = count * static_cast<size_t>(values[octaves]);
        const auto period = values[direction] == 2 && total > 1 ? total * 2 - 2 : total;
        const auto position = static_cast<size_t>(pitchCursor % period);
        const auto selected = values[direction] == 1 ? total - 1 - position
            : (position < total ? position : period - position);
        auto note = *sorted[selected % count];
        note.key = std::min(127, note.key + 12 * static_cast<int>(selected / count));
        note.id = nextId;
        nextId = nextId == std::numeric_limits<int32_t>::max() ? 1 : nextId + 1;
        if (!send(note, true, output, frame)) return;
        sounding = note;
        offBeat = beat + division * values[gate] / 100;
        pitchIndex = static_cast<int>(position);
        outputKey = note.key;
        ++pitchCursor;
        ++emitted;
        ++revision;
    }
};
} // namespace mote
