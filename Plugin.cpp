#include "Plugin.h"
#include "Presets.h"
#include "Engine.h"
#include "char_clap_utils/Streams.h"
#include "char_clap_utils/WebUI.h"
#include <clap/helpers/param-queue.hh>
#include <clap/helpers/plugin.hh>
#include <clap/helpers/plugin.hxx>
#include <clap/ext/preset-load.h>
#include <clap/factory/preset-discovery.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

namespace mote
{
namespace
{
enum class EditType { begin, value, end };
struct Edit { EditType type; clap_id id; double value; };

class MotePlugin final : public clap::helpers::Plugin<clap::helpers::MisbehaviourHandler::Terminate,
                                                     clap::helpers::CheckingLevel::Minimal>
{
    using Base = clap::helpers::Plugin<clap::helpers::MisbehaviourHandler::Terminate,
                                      clap::helpers::CheckingLevel::Minimal>;
public:
    explicit MotePlugin(const clap_host_t* h)
        : Base(&descriptor(), h), host(h),
          auv3Host(h && h->name && std::strstr(h->name, "(CLAP-as-AUv3)")),
          ui(h, [this](std::string_view text) { return receiveUI(text); })
    {
        for (auto& value : values) value.store(0);
        for (const auto& p : parameters) values[p.id].store(p.initial);
        for (auto& key : keys) key.store(-1);
    }

protected:
    bool init() noexcept override
    {
        hostParams = static_cast<const clap_host_params_t*>(host->get_extension(host, CLAP_EXT_PARAMS));
        hostState = static_cast<const clap_host_state_t*>(host->get_extension(host, CLAP_EXT_STATE));
        hostPresets = static_cast<const clap_host_preset_load_t*>(host->get_extension(host, CLAP_EXT_PRESET_LOAD));
        return true;
    }
    bool activate(double sr, uint32_t, uint32_t) noexcept override
    {
        if (!std::isfinite(sr) || sr <= 0) return false;
        sampleRate = sr;
        reset();
        return true;
    }
    void reset() noexcept override { engine.reset(); publishStatus(); }
    bool startProcessing() noexcept override { return true; }
    clap_process_status process(const clap_process_t* block) noexcept override
    {
        if (!block || block->frames_count == 0) return CLAP_PROCESS_ERROR;
        emitEdits(block->out_events);
        for (uint32_t bus = 0; bus < block->audio_outputs_count; ++bus)
        {
            auto& output = block->audio_outputs[bus];
            for (uint32_t channel = 0; channel < output.channel_count; ++channel)
            {
                if (output.data32 && output.data32[channel])
                    std::fill_n(output.data32[channel], block->frames_count, 0.0f);
                if (output.data64 && output.data64[channel])
                    std::fill_n(output.data64[channel], block->frames_count, 0.0);
            }
        }
        for (const auto& p : parameters) engine.set(p.id, values[p.id].load(std::memory_order_relaxed));
        if (clearRequested.exchange(false, std::memory_order_acq_rel)) engine.clear();
        engine.process(*block, sampleRate);
        // Reflect host automation after the engine has applied it at its sample offset.
        applyParameters(block->in_events);
        publishStatus();
        return CLAP_PROCESS_CONTINUE;
    }
    bool implementsNotePorts() const noexcept override { return true; }
    uint32_t notePortsCount(bool) const noexcept override { return 1; }
    bool notePortsInfo(uint32_t index, bool input, clap_note_port_info_t* info) const noexcept override
    {
        if (index != 0 || !info) return false;
        *info = {};
        info->id = 0;
        info->supported_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI;
        info->preferred_dialect = CLAP_NOTE_DIALECT_CLAP;
        std::snprintf(info->name, sizeof(info->name), "%s", input ? "Notes in" : "Notes out");
        return true;
    }
    bool implementsAudioPorts() const noexcept override { return auv3Host; }
    uint32_t audioPortsCount(bool input) const noexcept override { return auv3Host && !input ? 1 : 0; }
    bool audioPortsInfo(uint32_t index, bool input, clap_audio_port_info_t* info) const noexcept override
    {
        if (!auv3Host || input || index != 0 || !info) return false;
        *info = {};
        info->id = 0;
        info->channel_count = 2;
        info->flags = CLAP_AUDIO_PORT_IS_MAIN;
        info->port_type = CLAP_PORT_STEREO;
        info->in_place_pair = CLAP_INVALID_ID;
        std::snprintf(info->name, sizeof(info->name), "Render clock");
        return true;
    }
    bool implementsParams() const noexcept override { return true; }
    uint32_t paramsCount() const noexcept override { return static_cast<uint32_t>(parameters.size()); }
    bool paramsInfo(uint32_t index, clap_param_info_t* info) const noexcept override
    {
        if (!info || index >= parameters.size()) return false;
        const auto& p = parameters[index];
        *info = {};
        info->id = p.id;
        info->flags = CLAP_PARAM_IS_AUTOMATABLE | CLAP_PARAM_IS_STEPPED | CLAP_PARAM_REQUIRES_PROCESS;
        if (p.id == rate || p.id == latch || p.id == direction) info->flags |= CLAP_PARAM_IS_ENUM;
        info->min_value = p.min; info->max_value = p.max; info->default_value = p.initial;
        std::snprintf(info->name, sizeof(info->name), "%s", p.name);
        return true;
    }
    bool paramsValue(clap_id id, double* result) noexcept override
    {
        if (!findParameter(id) || !result) return false;
        *result = values[id].load(std::memory_order_relaxed);
        return true;
    }
    bool paramsValueToText(clap_id id, double value, char* text, uint32_t size) noexcept override
    {
        if (!findParameter(id) || !text || !size) return false;
        value = clampParameter(id, value);
        int written;
        if (id == rate) written = std::snprintf(text, size, "%s", divisionNames[static_cast<size_t>(value)]);
        else if (id == direction) written = std::snprintf(text, size, "%s", directionNames[static_cast<size_t>(value)]);
        else if (id == latch) written = std::snprintf(text, size, "%s", value == 0 ? "Off" : "On");
        else written = std::snprintf(text, size, "%.0f%s", value, id == gate ? "%" : "x");
        return written >= 0 && static_cast<uint32_t>(written) < size;
    }
    bool paramsTextToValue(clap_id id, const char* text, double* value) noexcept override
    {
        if (!findParameter(id) || !text || !value) return false;
        for (int i = static_cast<int>(findParameter(id)->min); i <= findParameter(id)->max; ++i)
        {
            char formatted[32];
            paramsValueToText(id, i, formatted, sizeof(formatted));
            if (std::strcmp(formatted, text) == 0) { *value = i; return true; }
        }
        char* end = nullptr;
        auto parsed = std::strtod(text, &end);
        if (end == text || !std::isfinite(parsed)) return false;
        while (*end == ' ') ++end;
        if ((id == gate && *end == '%') || (id == octaves && (*end == 'x' || *end == 'X'))) ++end;
        if (*end != 0) return false;
        *value = clampParameter(id, parsed);
        return true;
    }
    void paramsFlush(const clap_input_events_t* input, const clap_output_events_t* output) noexcept override
    {
        emitEdits(output);
        applyParameters(input);
    }
    bool implementsState() const noexcept override { return true; }
    bool stateSave(const clap_ostream_t* stream) noexcept override
    {
        State state;
        for (const auto& p : parameters) state.values[p.id] = values[p.id].load(std::memory_order_relaxed);
        return stream && char_clap::writeComplete(*stream, &state, sizeof(state));
    }
    bool stateLoad(const clap_istream_t* stream) noexcept override
    {
        State state;
        if (!stream || !char_clap::readComplete(*stream, &state, sizeof(state))
            || state.magic != 0x4d4f5445 || state.version != 1) return false;
        for (auto value : state.values) if (!std::isfinite(value)) return false;
        if (state.values[octaves] < 1) state.values[octaves] = findParameter(octaves)->initial;
        if (state.values[gate] < 5) state.values[gate] = findParameter(gate)->initial;
        for (const auto& p : parameters) setValue(p.id, state.values[p.id]);
        clearRequested.store(true, std::memory_order_release);
        notifyValuesChanged();
        return true;
    }
    bool implementsPresetLoad() const noexcept override { return true; }
    bool presetLoadFromLocation(uint32_t kind, const char* location, const char* key) noexcept override
    {
        if (kind != CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN || location || !key) return false;
        for (const auto& preset : presets)
            if (std::strcmp(preset.key, key) == 0)
            {
                for (const auto& p : parameters) setValue(p.id, preset.values[p.id]);
                notifyValuesChanged();
                if (hostPresets) hostPresets->loaded(host, kind, location, key);
                return true;
            }
        return false;
    }
    bool enableDraftExtensions() const noexcept override { return true; }
    bool implementsWebview() const noexcept override { return true; }

    int32_t webviewGetUri(char* uri, uint32_t capacity) const noexcept override
    {
        return ui.getUri(uri, capacity);
    }

    bool webviewGetResource(const char* path, char* mime, uint32_t mimeCapacity,
                            const clap_ostream_t* stream) override
    {
        return ui.getResource(path, mime, mimeCapacity, stream);
    }

    bool webviewReceive(const void* data, uint32_t size) const noexcept override
    {
        return ui.receiveBytes(data, size);
    }

    bool implementsGui() const noexcept override { return true; }
    bool guiIsApiSupported(const char* api, bool floating) noexcept override
    {
        return ui.guiIsApiSupported(api, floating);
    }

    bool guiGetPreferredApi(const char** api, bool* floating) noexcept override
    {
        return ui.guiGetPreferredApi(api, floating);
    }

    bool guiCreate(const char* api, bool floating) noexcept override
    {
        return ui.guiCreate(api, floating, 480, 280);
    }

    void guiDestroy() noexcept override
    {
        uiVisible.store(false, std::memory_order_release);
        uiReady.store(false, std::memory_order_release);
        ui.guiDestroy();
    }

    bool guiShow() noexcept override
    {
        if (!ui.guiShow()) return false;
        uiVisible.store(true, std::memory_order_release);
        return true;
    }

    bool guiHide() noexcept override
    {
        if (!ui.guiHide()) return false;
        uiVisible.store(false, std::memory_order_release);
        return true;
    }

    bool guiGetSize(uint32_t* width, uint32_t* height) noexcept override
    {
        return ui.guiGetSize(width, height);
    }

    bool guiCanResize() const noexcept override { return true; }
    bool guiAdjustSize(uint32_t* width, uint32_t* height) noexcept override
    {
        if (!width || !height) return false;
        *width = std::max(320u, *width);
        *height = std::max(260u, *height);
        return true;
    }

    bool guiSetSize(uint32_t width, uint32_t height) noexcept override
    {
        return ui.guiSetSize(width, height);
    }

    bool guiSetParent(const clap_window_t* window) noexcept override
    {
        return ui.guiSetParent(window);
    }

    void onMainThread() noexcept override
    {
        if (valuesDirty.exchange(false, std::memory_order_acq_rel)) sendValues();
        if (statusDirty.exchange(false, std::memory_order_acq_rel)) sendStatus();
    }

private:
    struct State { uint32_t magic = 0x4d4f5445, version = 1; Values values {}; };

    void setValue(clap_id id, double value) noexcept
    {
        values[id].store(clampParameter(id, value), std::memory_order_relaxed);
    }
    void applyParameters(const clap_input_events_t* input) noexcept
    {
        const auto count = input ? input->size(input) : 0;
        for (uint32_t i = 0; i < count; ++i)
        {
            const auto* e = input->get(input, i);
            if (!e || e->space_id != CLAP_CORE_EVENT_SPACE_ID || e->type != CLAP_EVENT_PARAM_VALUE
                || e->size < sizeof(clap_event_param_value_t)) continue;
            const auto& p = reinterpret_cast<const clap_event_param_value_t&>(*e);
            if (!findParameter(p.param_id) || p.note_id >= 0 || p.port_index >= 0 || p.channel >= 0 || p.key >= 0) continue;
            setValue(p.param_id, p.value);
            if (!valuesDirty.exchange(true, std::memory_order_acq_rel)) host->request_callback(host);
        }
    }
    void notifyValuesChanged() noexcept
    {
        if (!valuesDirty.exchange(true, std::memory_order_acq_rel)) host->request_callback(host);
        if (hostParams) hostParams->rescan(host, CLAP_PARAM_RESCAN_VALUES);
        if (hostState) hostState->mark_dirty(host);
        host->request_process(host);
    }
    bool receiveUI(std::string_view message)
    {
        if (message == "ready")
        {
            uiReady.store(true, std::memory_order_release);
            sendValues(); sendStatus();
            return true;
        }
        if (message == "clear")
        {
            clearRequested.store(true, std::memory_order_release);
            host->request_process(host);
            return true;
        }
        if (message.substr(0, 7) == "preset:")
        {
            const std::string key(message.substr(7));
            return presetLoadFromLocation(CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr, key.c_str());
        }
        const std::string text(message);
        unsigned id = 0;
        double value = 0;
        if (std::sscanf(text.c_str(), "value:%u:%lf", &id, &value) == 2)
        {
            if (!findParameter(id) || !std::isfinite(value)) return false;
            value = clampParameter(id, value);
            if (!queueEdit({ EditType::value, id, value })) return false;
            setValue(id, value);
            notifyValuesChanged();
            return true;
        }
        if (std::sscanf(text.c_str(), "begin:%u", &id) == 1) return queueEdit({ EditType::begin, id, 0 });
        if (std::sscanf(text.c_str(), "end:%u", &id) == 1) return queueEdit({ EditType::end, id, 0 });
        return false;
    }
    bool queueEdit(const Edit& edit)
    {
        if (!findParameter(edit.id) || !edits.tryPush(edit)) return false;
        if (hostParams) hostParams->request_flush(host);
        host->request_process(host);
        return true;
    }
    void emitEdits(const clap_output_events_t* output) noexcept
    {
        if (!output) return;
        Edit edit;
        while (edits.tryPeek(edit))
        {
            bool sent;
            if (edit.type == EditType::value)
            {
                const clap_event_param_value_t e { { sizeof(e), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, CLAP_EVENT_IS_LIVE },
                    edit.id, nullptr, -1, -1, -1, -1, edit.value };
                sent = output->try_push(output, &e.header);
            }
            else
            {
                const clap_event_param_gesture_t e { { sizeof(e), 0, CLAP_CORE_EVENT_SPACE_ID,
                    static_cast<uint16_t>(edit.type == EditType::begin ? CLAP_EVENT_PARAM_GESTURE_BEGIN : CLAP_EVENT_PARAM_GESTURE_END),
                    CLAP_EVENT_IS_LIVE }, edit.id };
                sent = output->try_push(output, &e.header);
            }
            if (!sent) break;
            edits.consume();
        }
    }
    void sendValues() const
    {
        std::string text = "values:";
        for (const auto& p : parameters)
        {
            const auto id = p.id;
            char pair[48];
            std::snprintf(pair, sizeof(pair), "%u=%.9g;", id, values[id].load(std::memory_order_relaxed));
            text += pair;
        }
        ui.send(text);
    }
    void publishStatus() noexcept
    {
        if (lastRevision == engine.revision) return;
        lastRevision = engine.revision;
        clockState.store(engine.playing ? 2 : engine.hasClock ? 1 : 0, std::memory_order_relaxed);
        tempo.store(engine.bpm, std::memory_order_relaxed);
        tick.store(engine.tick, std::memory_order_relaxed);
        pitch.store(engine.pitchIndex, std::memory_order_relaxed);
        key.store(engine.outputKey, std::memory_order_relaxed);
        emitted.store(engine.emitted, std::memory_order_relaxed);
        std::array<int, Engine::capacity> sorted {};
        size_t count = 0;
        for (const auto& n : engine.bank) if (n.used) sorted[count++] = n.key;
        std::sort(sorted.begin(), sorted.begin() + count);
        for (size_t i = 0; i < count; ++i) keys[i].store(sorted[i], std::memory_order_relaxed);
        held.store(static_cast<uint32_t>(count), std::memory_order_relaxed);
        if (uiReady.load(std::memory_order_acquire) && !statusDirty.exchange(true, std::memory_order_acq_rel))
            host->request_callback(host);
    }
    void sendStatus() const
    {
        char line[180];
        const auto count = held.load(std::memory_order_relaxed);
        std::snprintf(line, sizeof(line), "status:%d:%.2f:%u:%d:%d:%d:%llu:",
            clockState.load(std::memory_order_relaxed), tempo.load(std::memory_order_relaxed), count,
            tick.load(std::memory_order_relaxed), pitch.load(std::memory_order_relaxed), key.load(std::memory_order_relaxed),
            static_cast<unsigned long long>(emitted.load(std::memory_order_relaxed)));
        std::string text(line);
        for (size_t i = 0; i < count; ++i)
        {
            std::snprintf(line, sizeof(line), "%s%d", i ? "," : "", keys[i].load(std::memory_order_relaxed));
            text += line;
        }
        ui.send(text);
    }

    const clap_host_t* host;
    const bool auv3Host;
    const clap_host_params_t* hostParams = nullptr;
    const clap_host_state_t* hostState = nullptr;
    const clap_host_preset_load_t* hostPresets = nullptr;
    char_clap::WebUI ui;
    Engine engine;
    double sampleRate = 48000;
    std::array<std::atomic<double>, stateValueCount> values;
    clap::helpers::ParamQueue<Edit, 128> edits;
    std::atomic<bool> uiReady { false }, uiVisible { false }, valuesDirty { false }, statusDirty { false }, clearRequested { false };
    std::atomic<int> clockState { 0 }, tick { -1 }, pitch { -1 }, key { -1 };
    std::atomic<double> tempo { 120 };
    std::atomic<uint32_t> held { 0 };
    std::atomic<uint64_t> emitted { 0 };
    std::array<std::atomic<int>, Engine::capacity> keys;
    uint64_t lastRevision = 0;
};

uint32_t pluginCount(const clap_plugin_factory_t*) { return 1; }

const clap_plugin_descriptor_t* pluginDescriptor(const clap_plugin_factory_t*, uint32_t index)
{
    return index == 0 ? &descriptor() : nullptr;
}

const clap_plugin_t* createPlugin(const clap_plugin_factory_t*, const clap_host_t* host,
                                  const char* id)
{
    if (!host || !id || std::strcmp(id, pluginId) != 0) return nullptr;
    return (new MotePlugin(host))->clapPlugin();
}

struct PresetProvider
{
    clap_preset_discovery_provider_t provider;
    const clap_preset_discovery_indexer_t* indexer;

    explicit PresetProvider(const clap_preset_discovery_indexer_t* newIndexer)
        : provider { &providerDescriptor(), this, init, destroy, metadata, extension },
          indexer(newIndexer)
    {}

    static const clap_preset_discovery_provider_descriptor_t& providerDescriptor()
    {
        static const clap_preset_discovery_provider_descriptor_t value {
            CLAP_VERSION, "com.charlieculbert.mote.presets",
            "Mote Presets", "Charlie Culbert"
        };
        return value;
    }

    static PresetProvider& from(const clap_preset_discovery_provider_t* provider)
    {
        return *static_cast<PresetProvider*>(provider->provider_data);
    }

    static bool init(const clap_preset_discovery_provider_t* provider)
    {
        static const clap_preset_discovery_location_t location {
            CLAP_PRESET_DISCOVERY_IS_FACTORY_CONTENT, "Factory Presets",
            CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr
        };
        return from(provider).indexer->declare_location(from(provider).indexer, &location);
    }

    static void destroy(const clap_preset_discovery_provider_t* provider)
    {
        delete &from(provider);
    }

    static bool metadata(const clap_preset_discovery_provider_t*, uint32_t kind,
                         const char* location,
                         const clap_preset_discovery_metadata_receiver_t* receiver)
    {
        if (kind != CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN || location || !receiver)
            return false;
        const clap_universal_plugin_id_t plugin { "clap", pluginId };
        for (const auto& preset : presets)
        {
            if (!receiver->begin_preset(receiver, preset.name, preset.key)) return false;
            receiver->add_plugin_id(receiver, &plugin);
            receiver->set_flags(receiver, CLAP_PRESET_DISCOVERY_IS_FACTORY_CONTENT);
            receiver->add_creator(receiver, "Charlie Culbert");
            receiver->add_feature(receiver, CLAP_PLUGIN_FEATURE_NOTE_EFFECT);
        }
        return true;
    }

    static const void* extension(const clap_preset_discovery_provider_t*, const char*)
    {
        return nullptr;
    }
};

uint32_t presetProviderCount(const clap_preset_discovery_factory_t*) { return 1; }

const clap_preset_discovery_provider_descriptor_t* presetProviderDescriptor(
    const clap_preset_discovery_factory_t*, uint32_t index)
{
    return index == 0 ? &PresetProvider::providerDescriptor() : nullptr;
}

const clap_preset_discovery_provider_t* createPresetProvider(
    const clap_preset_discovery_factory_t*, const clap_preset_discovery_indexer_t* indexer,
    const char* id)
{
    if (!indexer || !id
        || std::strcmp(id, PresetProvider::providerDescriptor().id) != 0)
        return nullptr;
    return &(new PresetProvider(indexer))->provider;
}

} // namespace

const clap_plugin_descriptor_t& descriptor() noexcept
{
    static const char* features[] { CLAP_PLUGIN_FEATURE_NOTE_EFFECT, nullptr };
    static const clap_plugin_descriptor_t value {
        CLAP_VERSION, pluginId, "Mote", "Charlie Culbert",
        "", "", "", "1.0.0",
        "A simple arpeggiator", features
    };
    return value;
}

bool entryInit(const char* path) { return char_clap::setResourceRoot(path); }
void entryDeinit() { char_clap::resourceRoot.clear(); }

const void* entryGetFactory(const char* factoryId)
{
    if (!factoryId) return nullptr;
    if (std::strcmp(factoryId, CLAP_PLUGIN_FACTORY_ID) == 0)
    {
        static const clap_plugin_factory_t factory { pluginCount, pluginDescriptor, createPlugin };
        return &factory;
    }
    if (std::strcmp(factoryId, CLAP_PRESET_DISCOVERY_FACTORY_ID) == 0)
    {
        static const clap_preset_discovery_factory_t factory {
            presetProviderCount, presetProviderDescriptor, createPresetProvider
        };
        return &factory;
    }
    return nullptr;
}

} // namespace mote
