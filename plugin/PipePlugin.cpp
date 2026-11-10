// Pipe - physically modelled pipe organ
// DPF wrapper around the engine: parameters, programs, state and MIDI timing.
#include "DistrhoPlugin.hpp"
#include "../src/engine/Engine.hpp"
#include "../src/engine/Presets.hpp"

START_NAMESPACE_DISTRHO

using namespace pipe;

class PipePlugin : public Plugin
{
public:
    PipePlugin()
        : Plugin(kNumParams, kNumFactoryPresets, kNumStates),
          engine(new Engine())
    {
        for (int i = 0; i < kNumStates; ++i) states[i] = "";
        engine->prepare(getSampleRate());
    }

protected:
    const char* getLabel() const override { return "Pipe"; }
    const char* getDescription() const override
    {
        return "Physically modelled pipe organ with MPE and polyphonic aftertouch, microtonal tunings and per-pipe voicing. Every pipe could be built.";
    }
    const char* getMaker() const override { return "78Hz"; }
    const char* getHomePage() const override { return "https://github.com/78Hz/pipe"; }
    const char* getLicense() const override { return "Custom"; }
    uint32_t getVersion() const override { return d_version(0, 1, 0); }

    void initAudioPort(bool input, uint32_t index, AudioPort& port) override
    {
        port.groupId = kPortGroupStereo;
        Plugin::initAudioPort(input, index, port);
    }

    void initParameter(uint32_t index, Parameter& p) override
    {
        const ParamInfo& info = paramInfo((int)index);
        p.name = info.name;
        p.symbol = info.symbol;
        p.unit = info.unit;
        p.ranges.min = info.min;
        p.ranges.max = info.max;
        p.ranges.def = info.def;
        switch (info.type)
        {
        case kPBool:   p.hints = kParameterIsAutomatable | kParameterIsBoolean | kParameterIsInteger; break;
        case kPInt:    p.hints = kParameterIsAutomatable | kParameterIsInteger; break;
        case kPEnum:   p.hints = kParameterIsAutomatable | kParameterIsInteger; break;
        case kPOutput: p.hints = kParameterIsOutput; break;
        default:       p.hints = kParameterIsAutomatable; break;
        }
        if (info.type == kPEnum && info.enumCount > 0)
        {
            ParameterEnumerationValue* const values = new ParameterEnumerationValue[info.enumCount];
            for (int i = 0; i < info.enumCount; ++i)
            {
                values[i].label = info.enumLabels[i];
                values[i].value = (float)i;
            }
            p.enumValues.count = (uint8_t)info.enumCount;
            p.enumValues.restrictedMode = true;
            p.enumValues.values = values;
        }
        if (index < (uint32_t)kNumStops)
            p.groupId = kPortGroupNone;
    }

    void initProgramName(uint32_t index, String& name) override
    {
        name = kFactory[index].name;
    }

    void initState(uint32_t index, State& state) override
    {
        state.key = kStateKeys[index];
        state.label = kStateKeys[index];
        state.defaultValue = "";
        state.hints = kStateIsHostReadable;
    }

    float getParameterValue(uint32_t index) const override
    {
        return engine->getParameter((int)index);
    }

    void setParameterValue(uint32_t index, float value) override
    {
        engine->setParameter((int)index, value);
    }

    void loadProgram(uint32_t index) override
    {
        if (index >= (uint32_t)kNumFactoryPresets) return;
        const PresetData p = factoryPreset((int)index);
        for (const auto& kv : p.params) engine->setParameter(kv.first, kv.second);
        for (const auto& kv : p.states) applyState(kv.first, kv.second.c_str());
        applyState(kStatePresetName, kFactory[index].name);
    }

    String getState(const char* key) const override
    {
        for (int i = 0; i < kNumStates; ++i)
            if (std::strcmp(key, kStateKeys[i]) == 0) return String(states[i].c_str());
        return String();
    }

    void setState(const char* key, const char* value) override
    {
        for (int i = 0; i < kNumStates; ++i)
            if (std::strcmp(key, kStateKeys[i]) == 0) applyState(i, value);
    }

    void activate() override
    {
        engine->prepare(getSampleRate());
    }

    void sampleRateChanged(double newSampleRate) override
    {
        engine->prepare(newSampleRate);
    }

    void run(const float**, float** outputs, uint32_t frames, const MidiEvent* events, uint32_t eventCount) override
    {
        float* const L = outputs[0];
        float* const R = outputs[1];
        std::memset(L, 0, sizeof(float) * frames);
        std::memset(R, 0, sizeof(float) * frames);
        uint32_t pos = 0;
        for (uint32_t e = 0; e < eventCount; ++e)
        {
            const MidiEvent& ev = events[e];
            const uint32_t at = std::min(ev.frame, frames);
            if (at > pos) { renderSlice(L + pos, R + pos, at - pos); pos = at; }
            const uint8_t* data = ev.size > MidiEvent::kDataSize ? ev.dataExt : ev.data;
            engine->midi(data, (int)ev.size);
        }
        if (pos < frames) renderSlice(L + pos, R + pos, frames - pos);
    }

private:
    void renderSlice(float* L, float* R, uint32_t n)
    {
        // the room processes in place, so render into a zeroed slice
        engine->render(L, R, (int)n);
    }

    void applyState(int i, const char* value)
    {
        states[i] = value ? value : "";
        switch (i)
        {
        case kStateVoicing: engine->setVoicingText(states[i]); break;
        case kStatePipeTune: engine->setPipeTuningText(states[i]); break;
        case kStateScalaScl:
        case kStateScalaKbm: engine->setScala(states[kStateScalaScl], states[kStateScalaKbm]); break;
        case kStateMatrix: engine->setMatrixText(states[i]); break;
        case kStateCustomIr: engine->setCustomIrPath(states[i]); break;
        default: break;
        }
    }

    std::unique_ptr<Engine> engine;
    std::string states[kNumStates];

    DISTRHO_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PipePlugin)
};

Plugin* createPlugin()
{
    return new PipePlugin();
}

END_NAMESPACE_DISTRHO
