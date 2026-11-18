// Pipe - physically modelled pipe organ
// The interface: a line drawing of the console (stop jambs either side of the music desk), with
// drawers for wind, tuning, room, expression, voicing and presets.
#include "DistrhoUI.hpp"
#include "../src/engine/Params.hpp"
#include "../src/engine/Presets.hpp"
#include "../src/engine/Tuning.hpp"
#include "../src/engine/Voicing.hpp"
#include "../src/engine/Wind.hpp"
#include "../src/engine/Room.hpp"
#include "../src/engine/Matrix.hpp"
#include "../src/engine/MpeInput.hpp"
#include "../src/engine/Wav.hpp"
#include "IrMeta.hpp"
#include "../src/engine/PipeEdit.hpp"
#include "FontPlex.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <sstream>
#include <filesystem>
#include <future>
#include <fstream>
#include <functional>
#include <memory>
#include <vector>

START_NAMESPACE_DISTRHO

using namespace pipe;
namespace fs = std::filesystem;

static constexpr float kW = 1280.f, kH = 800.f;

enum Screen { kScrConsole = 0, kScrExpression, kScrWind, kScrVoicing, kScrTuning, kScrRoom, kScrPresets, kScrWorkshop, kNumScreens };
static const char* const kScreenNames[kNumScreens] = { "CONSOLE", "EXPRESSION", "WIND", "VOICING", "TUNING", "ROOM", "PRESETS", "WORKSHOP" };

static std::string userPresetDir()
{
    const char* home = std::getenv("USERPROFILE");
    if (!home || !*home) home = std::getenv("HOME");
    fs::path p = home ? fs::path(home) : fs::temp_directory_path();
    if (fs::exists(p / "Documents")) p /= "Documents";
    return (p / "OpusXIV Presets").string();
}

static std::string readFile(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static std::string upper(std::string s)
{
    for (auto& c : s) c = (char)std::toupper((unsigned char)c);
    return s;
}

static std::string fmt(const char* f, double v)
{
    char b[64]; std::snprintf(b, sizeof(b), f, v); return b;
}

static std::string noteName12(int key)
{
    static const char* n[12] = { "C", "C#", "D", "EB", "E", "F", "F#", "G", "G#", "A", "BB", "B" };
    return std::string(n[((key % 12) + 12) % 12]) + std::to_string(key / 12 - 1);
}

class PipeUI : public UI
{
public:
    PipeUI()
        : UI(DISTRHO_UI_DEFAULT_WIDTH, DISTRHO_UI_DEFAULT_HEIGHT),
          voicing(new Voicing())
    {
        voicing->setDefaults();
        for (int i = 0; i < kNumParams; ++i) pv[i] = paramInfo(i).def;
        font = createFontFromMemory("plex", pipe_res::kFontPlex, (uint)pipe_res::kFontPlexSize, false);
        fontBold = createFontFromMemory("plexbold", pipe_res::kFontPlexBold, (uint)pipe_res::kFontPlexBoldSize, false);
        const double sf = getScaleFactor();
        if (sf != 1.0) setSize((uint)(kW * sf), (uint)(kH * sf));
        setGeometryConstraints((uint)(kW * 0.5 * sf), (uint)(kH * 0.5 * sf), true, false);
        for (float& v : windHist) v = 0.f;
        for (float& v : windHistLong) v = 0.f;
        for (int g = 0; g < kNumGroups; ++g) include[g] = g != kGroupRouting;
        saveName = "MY ORGAN";
        matrix.setDefaults();
        scanUserPresets();
        syncTuning();
    }

protected:
    // ------------------------------------------------------------ host -> UI
    void parameterChanged(uint32_t index, float value) override
    {
        if (index >= (uint32_t)kNumParams) return;
        if (index < (uint32_t)kOutFirst && pv[index] != value && !loading) dirty = true;
        pv[index] = value;
        if (index == kParamTuning || index == kParamEdoN || index == kParamARef || index == kParamAnchorKey) syncTuning();
        if (index < (uint32_t)kOutFirst || screen == kScrConsole || screen == kScrWind || screen == kScrExpression || screen == kScrRoom)
            repaint();
    }

    void programLoaded(uint32_t index) override
    {
        presetLabel = std::string("P") + fmt("%02.0f", index + 1) + " " + kFactory[index].name;
        dirty = false;
        repaint();
    }

    void stateChanged(const char* key, const char* value) override
    {
        for (int i = 0; i < kNumStates; ++i)
        {
            if (std::strcmp(key, kStateKeys[i]) != 0) continue;
            st[i] = value ? value : "";
            if (i == kStateVoicing) voicing->parseVoicing(st[i]);
            if (i == kStatePipeTune) voicing->parseTuning(st[i]);
            if (i == kStateScalaScl || i == kStateScalaKbm) syncTuning();
            if (i == kStatePresetName && !st[i].empty()) presetLabel = st[i];
            if (i == kStatePipeEdits) { pipeEdits = parsePipeEdits(st[i]); wsKey = -1; }
            if (i == kStateMatrix) matrix.parse(st[i]);
        }
        repaint();
    }

    void uiIdle() override
    {
        learnPoll();
        if (wsFuture.valid() && wsFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) { wsResult = wsFuture.get(); repaint(); }
        // wind history for the pressure graph: 4 s for sag and bounce, 2 min for the wind running out
        const auto now = std::chrono::steady_clock::now();
        if (now - histShortT >= std::chrono::milliseconds(33))
        {
            histShortT = now;
            windHist[windPos] = pv[kOutChestGt];
            windPos = (windPos + 1) % kHist;
            if (screen == kScrWind) repaint();
        }
        if (now - histLongT >= std::chrono::milliseconds(1000))
        {
            histLongT = now;
            windHistLong[windPosLong] = pv[kOutChestGt];
            windPosLong = (windPosLong + 1) % kHist;
        }
    }

    // --------------------------------------------------------------- drawing
    void onNanoDisplay() override
    {
        hits.clear();
        const float s = getWidth() / kW;
        scale(s, s);
        fontFaceId(font);

        // the sheet: paper (or the chosen ground) inside a thin ruled border
        fill(0, 0, kW, kH, frameCol);
        fill(8, 8, kW - 16, kH - 16, lcd);
        beginPath(); rect(8.75f, 8.75f, kW - 17.5f, kH - 17.5f); strokeColor(ink); strokeWidth(1.5f); stroke();

        drawHeader();
        drawDrawerTabs();
        if (screen == kScrConsole) drawConsole();
        else
        {
            // a drawer: the page, laid out for the full sheet, drawn into the space between the header and
            // the drawer tabs (scaled to fit); its hit areas are mapped the same way
            const float k = (kDrawerBottom - kDrawerTop) / (kH - 76.f);
            const float ox = (kW - kW * k) / 2.f, oy = kDrawerTop - 76.f * k;
            const size_t h0 = hits.size();
            save(); translate(ox, oy); scale(k, k);
            switch (screen)
            {
            case kScrExpression: drawExpression(); break;
            case kScrWind: drawWind(); break;
            case kScrVoicing: drawVoicing(); break;
            case kScrTuning: drawTuning(); break;
            case kScrRoom: drawRoom(); break;
            case kScrPresets: drawPresets(); break;
            case kScrWorkshop: drawWorkshop(); break;
            default: break;
            }
            restore();
            for (size_t i = h0; i < hits.size(); ++i) { Hit& h = hits[i]; h.x = ox + h.x * k; h.y = oy + h.y * k; h.w *= k; h.h *= k; }
        }
        if (!toast.empty())
        {
            const float w = 520;
            fill(kW / 2 - w / 2, kH - 60, w, 34, ink);
            text(kW / 2, kH - 43, toast, 20, lcd, ALIGN_CENTER | ALIGN_MIDDLE);
        }
    }

    // ----------------------------------------------------------------- input
    bool onMouse(const MouseEvent& ev) override
    {
        const float s = getWidth() / kW;
        const float x = (float)ev.pos.getX() / s, y = (float)ev.pos.getY() / s;
        if (!ev.press)
        {
            if (dragHit >= 0 && dragHit < (int)hits.size()) { const Hit& h = hits[(size_t)dragHit]; if (h.param >= 0) editParameter((uint32_t)h.param, false); }
            dragHit = -1;
            return false;
        }
        if (ev.button != 1) return false;
        toast.clear();
        nameFocus = false;
        for (int i = (int)hits.size() - 1; i >= 0; --i)
        {
            const Hit& h = hits[(size_t)i];
            if (x < h.x || y < h.y || x > h.x + h.w || y > h.y + h.h) continue;
            if (h.param >= 0)
            {
                dragHit = i; dragStartY = y; dragStartV = pv[h.param];
                editParameter((uint32_t)h.param, true);
            }
            if (h.drag) { dragHit = i; dragStartY = y; }
            if (h.click) h.click();
            repaint();
            return true;
        }
        return false;
    }

    bool onMotion(const MotionEvent& ev) override
    {
        if (dragHit < 0 || dragHit >= (int)hits.size()) return false;
        const Hit& h = hits[(size_t)dragHit];
        const float s = getWidth() / kW;
        const float y = (float)ev.pos.getY() / s;
        if (h.drag) { auto f = h.drag; f(dragStartY - y); repaint(); return true; }
        const ParamInfo& pi = paramInfo(h.param);
        float v = dragStartV + (dragStartY - y) / 200.f * (pi.max - pi.min);
        v = std::max(pi.min, std::min(pi.max, v));
        if (pi.type == kPInt || pi.type == kPEnum) v = std::round(v);
        setParam(h.param, v);
        repaint();
        return true;
    }

    bool onScroll(const ScrollEvent& ev) override
    {
        const float s = getWidth() / kW;
        const float x = (float)ev.pos.getX() / s, y = (float)ev.pos.getY() / s;
        for (int i = (int)hits.size() - 1; i >= 0; --i)
        {
            const Hit& h = hits[(size_t)i];
            if (h.param < 0 || x < h.x || y < h.y || x > h.x + h.w || y > h.y + h.h) continue;
            const ParamInfo& pi = paramInfo(h.param);
            const float step = (pi.type == kPInt || pi.type == kPEnum) ? 1.f : (pi.max - pi.min) / 100.f;
            float v = pv[h.param] + (ev.delta.getY() > 0 ? step : -step);
            setParam(h.param, std::max(pi.min, std::min(pi.max, v)));
            repaint();
            return true;
        }
        return false;
    }

    bool onKeyboard(const KeyboardEvent& ev) override
    {
        if (!nameFocus || !ev.press) return false;
        if (ev.key == kKeyBackspace) { if (!saveName.empty()) saveName.pop_back(); }
        else if (ev.key == kKeyEnter || ev.key == kKeyEscape) nameFocus = false;
        else if (ev.key >= 32 && ev.key < 127 && saveName.size() < 24)
        {
            const char c = (char)std::toupper((int)ev.key);
            if (std::isalnum((unsigned char)c) || c == ' ' || c == '-' || c == '+' || c == '_') saveName.push_back(c);
        }
        repaint();
        return true;
    }

    void uiFileBrowserSelected(const char* filename) override
    {
        if (!filename) return;
        const std::string path = filename;
        switch (fileAction)
        {
        case kFileScl:
        {
            const std::string text = readFile(path);
            ScalaScale test; std::string err;
            if (!parseScl(text, test, &err)) { toast = "NOT A VALID .SCL FILE: " + upper(err); break; }
            setStateValue(kStateScalaScl, text);
            setStateValue(kStateScalaName, upper(fs::path(path).stem().string()));
            setParam(kParamTuning, kTunScala);
            toast = "LOADED " + upper(fs::path(path).filename().string());
            break;
        }
        case kFileIr:
        {
            WavData wav; std::string err;
            if (!readWavFile(path, wav, err, 48000 * 2)) { toast = "IR: " + err; break; }
            if (wav.channels != 1 && wav.channels != 2 && wav.channels != 4) { toast = fmt("IR HAS %.0f CHANNELS: USE 1, 2 OR 4", wav.channels); break; }
            setStateValue(kStateCustomIr, path);
            setParam(kParamRoom, kRoomCustom);
            toast = "LOADED IR " + upper(fs::path(path).filename().string());
            break;
        }
        case kFileKbm:
            setStateValue(kStateScalaKbm, readFile(path));
            toast = "LOADED " + upper(fs::path(path).filename().string());
            break;
        case kFileImport:
        {
            PresetData p;
            if (!decodePreset(readFile(path), p)) { toast = "NOT A PIPE PRESET FILE"; break; }
            applyPreset(p, p.groups);
            presetLabel = p.name.empty() ? upper(fs::path(path).stem().string()) : p.name;
            // keep a copy in the user folder
            writeUserPreset(presetLabel, encodePreset(p));
            toast = "IMPORTED " + presetLabel;
            break;
        }
        case kFileExport:
        {
            std::ofstream f(path, std::ios::binary);
            f << encodePreset(currentPreset(saveName));
            toast = f.good() ? "EXPORTED " + upper(fs::path(path).filename().string()) : "COULD NOT WRITE FILE";
            break;
        }
        default: break;
        }
        repaint();
    }

private:
    // ===================================================== small draw helpers
    struct Hit { float x, y, w, h; std::function<void()> click; int param = -1; std::function<void(float)> drag; };
    std::vector<Hit> hits;

    void addHit(float x, float y, float w, float h, std::function<void()> f) { hits.push_back({ x, y, w, h, std::move(f), -1 }); }
    void addDrag(float x, float y, float w, float h, int param) { hits.push_back({ x, y, w, h, nullptr, param }); }

    void fill(float x, float y, float w, float h, Color c) { beginPath(); rect(x, y, w, h); fillColor(c); NanoVG::fill(); }
    void frame(float x, float y, float w, float h, float t = 1.5f)
    {
        beginPath(); rect(x + t * 0.5f, y + t * 0.5f, w - t, h - t); strokeColor(ink); strokeWidth(t); stroke();
    }
    // Sizes in this file were set for the earlier pixel font; Plex Sans Condensed reads the same at 0.78 of them
    static constexpr float kFontScale = 0.78f;
    void fontSize(float size) { NanoVG::fontSize(size * kFontScale); }
    void text(float x, float y, const std::string& s, float size = 20.f, Color c = Color(0, 0, 0, 0.f), int align = ALIGN_LEFT | ALIGN_MIDDLE)
    {
        if (s.empty()) return;
        if (c.alpha == 0.f) c = ink;
        fontSize(size); fillColor(c); textAlign(align); NanoVG::text(x, y, s.c_str(), nullptr);
    }
    // bold (the headings and the name)
    void textB(float x, float y, const std::string& s, float size, Color c, int align = ALIGN_LEFT | ALIGN_MIDDLE)
    {
        fontFaceId(fontBold); text(x, y, s, size, c, align); fontFaceId(font);
    }
    void titleBar(float x, float y, float w, const std::string& left, const std::string& right = "")
    {
        // a heading ruled underneath (line drawing), not a filled bar
        textB(x + 2, y + 13, left, 21, ink);
        if (!right.empty()) text(x + w - 2, y + 13, right, 21, ink2, ALIGN_RIGHT | ALIGN_MIDDLE);
        hline(x, y + 26, w);
    }
    void button(float x, float y, float w, float h, const std::string& label, bool on, std::function<void()> f, float size = 21.f)
    {
        if (on) fill(x, y, w, h, ink);
        frame(x, y, w, h);
        text(x + w / 2, y + h / 2 + 1, label, size, on ? lcd : ink, ALIGN_CENTER | ALIGN_MIDDLE);
        if (f) addHit(x, y, w, h, std::move(f));
    }
    void segBar(float x, float y, float w, float h, int n, float frac)
    {
        const float gap = 2.f, sw = (w - gap * (n - 1)) / n;
        const int on = (int)std::round(std::max(0.f, std::min(1.f, frac)) * n);
        for (int i = 0; i < n; ++i) fill(x + i * (sw + gap), y, sw, h, i < on ? ink : ghost);
    }
    void knob(float cx, float cy, int param, const std::string& label, const std::string& value)
    {
        const ParamInfo& pi = paramInfo(param);
        const float frac = (pv[param] - pi.min) / (pi.max - pi.min);
        beginPath(); circle(cx, cy, 30); strokeColor(ink); strokeWidth(1.5f); stroke();
        const float a = (-135.f + 270.f * frac - 90.f) * (float)M_PI / 180.f;
        beginPath(); moveTo(cx + std::cos(a) * 8, cy + std::sin(a) * 8); lineTo(cx + std::cos(a) * 26, cy + std::sin(a) * 26); strokeWidth(2); stroke();
        text(cx, cy + 46, label, 19, ink, ALIGN_CENTER | ALIGN_MIDDLE);
        fontSize(20);
        Rectangle<float> b; textBounds(0, 0, value.c_str(), nullptr, b);
        const float vw = b.getWidth() + 12;
        frame(cx - vw / 2, cy + 58, vw, 22, 1.f);
        text(cx, cy + 69, value, 20, ink, ALIGN_CENTER | ALIGN_MIDDLE);
        addDrag(cx - 34, cy - 34, 68, 68 + 48, param);
    }
    void stepper(float x, float y, float w, const std::string& label, const std::string& value,
                 std::function<void()> minus, std::function<void()> plus, float valueW = 110.f)
    {
        text(x, y + 20, label, 20);
        const float bx = x + w - (44 + 6 + valueW + 6 + 44);
        button(bx, y, 44, 40, "-", false, std::move(minus), 26);
        frame(bx + 50, y, valueW, 40);
        text(bx + 50 + valueW / 2, y + 21, value, 20, ink, ALIGN_CENTER | ALIGN_MIDDLE);
        button(bx + 56 + valueW, y, 44, 40, "+", false, std::move(plus), 26);
    }
    void paramStepper(float x, float y, float w, const std::string& label, int param, float step, const char* f, float mul = 1.f, float valueW = 110.f)
    {
        stepper(x, y, w, label, fmt(f, pv[param] * mul),
                [this, param, step] { nudge(param, -step); }, [this, param, step] { nudge(param, step); }, valueW);
    }
    void hline(float x, float y, float w, bool dashed = false)
    {
        beginPath();
        if (dashed) for (float xx = x; xx < x + w; xx += 10) { moveTo(xx, y); lineTo(std::min(x + w, xx + 5), y); }
        else { moveTo(x, y); lineTo(x + w, y); }
        strokeColor(dashed ? ink2 : ink); strokeWidth(1.5f); stroke();
    }
    void vline(float x, float y, float h) { beginPath(); moveTo(x, y); lineTo(x, y + h); strokeColor(ink); strokeWidth(1.5f); stroke(); }

    // ======================================================= parameter helpers
    void setParam(int id, float v)
    {
        if (id < 0 || id >= kOutFirst) return;
        if (pv[id] == v) return;
        pv[id] = v;
        setParameterValue((uint32_t)id, v);
        if (!loading) dirty = true;
        if (id == kParamTuning || id == kParamEdoN || id == kParamARef || id == kParamAnchorKey) syncTuning();
    }
    void toggle(int id) { editParameter((uint32_t)id, true); setParam(id, pv[id] >= 0.5f ? 0.f : 1.f); editParameter((uint32_t)id, false); }
    void nudge(int id, float step)
    {
        const ParamInfo& pi = paramInfo(id);
        editParameter((uint32_t)id, true);
        setParam(id, std::max(pi.min, std::min(pi.max, pv[id] + step)));
        editParameter((uint32_t)id, false);
    }
    void setStateValue(int s, const std::string& v)
    {
        st[s] = v;
        setState(kStateKeys[s], v.c_str());
        if (s == kStateScalaScl || s == kStateScalaKbm) syncTuning();
        if (!loading && s != kStatePresetName) dirty = true;
    }
    void syncTuning()
    {
        tuning.system = (int)pv[kParamTuning];
        tuning.customN = (int)pv[kParamEdoN];
        tuning.anchor = (int)pv[kParamAnchorKey];
        tuning.aRef = pv[kParamARef];
        parseScl(st[kStateScalaScl], tuning.scl);
        tuning.kbm = ScalaKbm();
        if (!st[kStateScalaKbm].empty()) parseKbm(st[kStateScalaKbm], tuning.kbm);
    }
    bool on(int id) const { return pv[id] >= 0.5f; }

    // ============================================================== header
    void drawHeader()
    {
        textB(24, 36, "OPUS XIV", 40, ink);
        text(206, 28, "PASI OPUS 14", 18, ink2);
        text(206, 46, fmt("4 DIVISIONS · %.0f STOPS", (double)organ::kNumOrganStops), 18, ink2);
        static const char* const themes[3] = { "DRAFTING", "BLUEPRINT", "NIGHT" };
        button(470, 16, 120, 38, themes[theme], false, [this] { setTheme(theme + 1); }, 18);
        // combination / preset bar
        const float px = kW - 22 - (40 + 6 + 340 + 6 + 40 + 6 + 74);
        button(px, 16, 40, 38, "<", false, [this] { stepPreset(-1); }, 24);
        frame(px + 46, 16, 340, 38);
        std::string label = presetLabel;
        if (label.size() > 30) label = label.substr(0, 30);
        text(px + 58, 36, label, 21);
        if (dirty) text(px + 46 + 330, 36, "*", 24, ink, ALIGN_RIGHT | ALIGN_MIDDLE);
        addHit(px + 46, 16, 340, 38, [this] { screen = kScrPresets; scanUserPresets(); });
        button(px + 392, 16, 40, 38, ">", false, [this] { stepPreset(1); }, 24);
        button(px + 438, 16, 74, 38, "SAVE", true, [this] { screen = kScrPresets; scanUserPresets(); nameFocus = true; });
        hline(9, 66, kW - 18);
    }

    // the drawers, along the foot of the sheet: a tab opens its drawer over the console, again closes it
    void drawDrawerTabs()
    {
        const float y = kDrawerBottom + 6;
        hline(9, kDrawerBottom, kW - 18);
        text(22, y + 19, screen == kScrConsole ? "DRAWER ^" : "DRAWER v", 18, ink2);
        static const int order[7] = { kScrWind, kScrTuning, kScrRoom, kScrExpression, kScrVoicing, kScrWorkshop, kScrPresets };
        float x = 112;
        for (int i : order)
        {
            fontSize(20);
            Rectangle<float> b; textBounds(0, 0, kScreenNames[i], nullptr, b);
            const float w = b.getWidth() / kFontScale + 26;
            button(x, y + 2, w, 34, kScreenNames[i], screen == i, [this, i] { screen = screen == i ? (int)kScrConsole : i; if (screen == kScrPresets) scanUserPresets(); }, 20);
            x += w + 8;
        }
        const int room = std::max(0, std::min(kNumRooms - 1, (int)pv[kParamRoom]));
        std::string info = std::string(kTuningLabels[(int)pv[kParamTuning]]) + fmt(" · A=%.1f", pv[kParamARef]) + fmt(" · %.0f °C · ", pv[kParamPipeTemp]) + kRoomLabels[room]
                           + (pv[kOutClip] > 1.f ? (on(kParamLimiter) ? " · LIMITING" : " · CLIP!") : "");
        text(kW - 22, y + 19, info, 19, ink, ALIGN_RIGHT | ALIGN_MIDDLE);
    }

    // ============================================================= console
    // The console of Opus 14 as a line drawing: the stop jambs either side (Pedal and I Hauptwerk on the
    // left, III Unterwerk and II Oberwerk on the right), drawknobs lit when drawn; the music desk between
    // them shows the keys sounding on each keyboard, the chests' wind, the tremulant, the swell box, the
    // couplers and the combination pistons.
    void drawJamb(float x, float w, const int (&divs)[2])
    {
        float y = 78;
        const float cw = w / 5.f, rowH = 76.f;
        for (int d : divs)
        {
            std::vector<int> stops;
            for (int s = 0; s < kNumStops; ++s)
            {
                const StopDef& sd = kStops[(size_t)s];
                if ((sd.type == kStopRank || sd.type == kStopMixture) && sd.division == d) stops.push_back(s);
                if (sd.type == kStopTremulant && (int)pv[kParamTremTarget] == d) stops.push_back(s);
            }
            int drawn = 0; for (int s : stops) if (on(s)) ++drawn;
            textB(x, y + 10, divisionName(d), 21, ink);
            text(x + w, y + 10, fmt("%.0f / ", drawn) + fmt("%.0f DRAWN", (double)stops.size()), 18, ink2, ALIGN_RIGHT | ALIGN_MIDDLE);
            hline(x, y + 24, w, true);
            y += 32;
            for (size_t i = 0; i < stops.size(); ++i)
            {
                const int s = stops[i];
                const StopDef& sd = kStops[(size_t)s];
                const float cx = x + cw * ((float)(i % 5) + 0.5f), cy = y + (float)(i / 5) * rowH + 22;
                const bool isOn = on(s);
                if (isOn)
                {
                    beginPath(); circle(cx, cy, 20); fillColor(ink); NanoVG::fill();
                    beginPath(); circle(cx, cy, 24); strokeColor(ink); strokeWidth(1.5f); stroke();
                }
                else { beginPath(); circle(cx, cy, 20); strokeColor(ink); strokeWidth(1.5f); stroke(); }
                const bool trem = sd.type == kStopTremulant;
                text(cx, cy + 1, trem ? std::string("~") : std::string(sd.foot), trem ? 26 : 19, isOn ? lcd : ink, ALIGN_CENTER | ALIGN_MIDDLE);
                fontSize(14); fillColor(ink); textAlign(ALIGN_CENTER | ALIGN_TOP); textLineHeight(0.95f);
                const std::string name = trem ? std::string("TREMULANT") : std::string(sd.label);
                textBox(cx - cw / 2 + 2, cy + 27, cw - 4, name.c_str(), nullptr);
                addHit(cx - cw / 2, cy - 24, cw, rowH - 4, [this, s] {
                    toggle(s);
                    if (kStops[s].type == kStopRank || kStops[s].type == kStopMixture) selRank = kStops[s].rankFirst;
                });
            }
            y += (float)((stops.size() + 4) / 5) * rowH + 8;
        }
    }

    void keyboard(float x, float y, float w, float h, int lo, int n, int div)
    {
        auto black = [](int k) { const int m = k % 12; return m == 1 || m == 3 || m == 6 || m == 8 || m == 10; };
        auto lit = [this, div](int k) {
            for (int i = 0; i < kNumShownNotes; ++i)
            {
                const float* nn = &pv[kOutNote0 + i * 4];
                if (nn[0] < 0) continue;
                if ((int)nn[0] == k && (((int)nn[1] % 1000) / 100) == div) return true;
            }
            return false;
        };
        int nw = 0; for (int i = 0; i < n; ++i) if (!black(lo + i)) ++nw;
        const float ww = w / (float)nw;
        int wi = 0;
        for (int i = 0; i < n; ++i)
        {
            const int k = lo + i;
            if (black(k)) continue;
            const float kx = x + wi * ww;
            if (lit(k)) fill(kx, y, ww, h, ink);
            beginPath(); rect(kx, y, ww, h); strokeColor(ink); strokeWidth(1.f); stroke();
            ++wi;
        }
        wi = 0;
        for (int i = 0; i < n; ++i)
        {
            const int k = lo + i;
            if (!black(k)) { ++wi; continue; }
            const float bx = x + wi * ww - 0.32f * ww, bw = 0.64f * ww;
            fill(bx, y, bw, h * 0.6f, lit(k) ? ink : lcd);
            beginPath(); rect(bx, y, bw, h * 0.6f); strokeColor(ink); strokeWidth(1.f); stroke();
        }
    }

    void drawConsole()
    {
        static const int left[2] = { kPedal, kGreat }, right[2] = { kSwell, kPositive };
        drawJamb(20, 332, left);
        vline(364, 66, kDrawerBottom - 66);
        vline(916, 66, kDrawerBottom - 66);
        drawJamb(928, 332, right);

        // music desk
        const float x = 380, w = 520;
        float y = 78;
        textB(x, y + 10, "SOUNDING", 21, ink);
        text(x + w, y + 10, fmt("%.0f PIPES", pv[kOutPipes]) + fmt(" · DSP %.0f%%", pv[kOutCpu]), 18, ink2, ALIGN_RIGHT | ALIGN_MIDDLE);
        y += 30;
        static const int kb[4] = { kSwell, kPositive, kGreat, kPedal };
        const int chestOut[4] = { kOutChestSw, kOutChestPos, kOutChestGt, kOutChestPd };
        for (int i = 0; i < 4; ++i)
        {
            const int d = kb[i];
            text(x, y + 8, divisionName(d), 18);
            text(x + w, y + 8, fmt("CHEST %.0f MM", pv[chestOut[i]]), 18, ink2, ALIGN_RIGHT | ALIGN_MIDDLE);
            keyboard(x, y + 18, w, 40, 36, d == kPedal ? 30 : 61, d);
            segBar(x, y + 63, w, 5, 24, pv[chestOut[i]] / (kChestNominal[d] * 1.1f));
            y += 80;
        }
        hline(x, y + 2, w);
        y += 14;
        // tremulant, swell box, wind
        const float cw = w / 3.f;
        {
            const bool tOn = on(kNumStops - 1);
            text(x, y + 8, std::string("TREMULANT · ") + divisionShort((int)pv[kParamTremTarget]), 18, ink2);
            text(x, y + 36, tOn ? std::string(kTremKindLabels[(int)pv[kParamTremKind]]) + fmt(" %.1f HZ", pv[kParamTremSpeed]) : std::string("OFF"), 28);
            // its beat drawn as a wave, its height the depth
            beginPath();
            const float amp = tOn ? 4.f + 40.f * pv[kParamTremDepth] : 0.f;
            for (int j = 0; j <= 60; ++j) { const float xx = x + j * (cw - 16) / 60.f, yy = y + 64 - amp / 2.f * std::sin(j / 60.f * 6.f * (float)M_PI); if (j) lineTo(xx, yy); else moveTo(xx, yy); }
            strokeColor(ink); strokeWidth(1.5f); stroke();
            addHit(x, y, cw - 8, 80, [this] { toggle(kNumStops - 1); });
        }
        {
            const float sx = x + cw;
            text(sx, y + 8, "SWELL BOX · III", 18, ink2);
            text(sx, y + 36, fmt("%.0f%% OPEN", pv[kParamSwell] * 100), 28);
            const int open = (int)std::round(pv[kParamSwell] * 12);
            const float sw = (cw - 16) / 12.f;
            for (int j = 0; j < 12; ++j) { if (j >= open) fill(sx + j * sw, y + 54, sw - 2, 20, ink); beginPath(); rect(sx + j * sw, y + 54, sw - 2, 20); strokeColor(ink); strokeWidth(1.f); stroke(); }
            addDrag(sx, y, cw - 8, 80, kParamSwell);
        }
        {
            const float wx = x + 2 * cw;
            text(wx, y + 8, "WIND", 18, ink2);
            text(wx, y + 36, on(kParamBlowerOn) ? "BLOWER ON" : "BLOWER OFF", 28);
            text(wx, y + 64, fmt("RESERVOIR %.0f MM", pv[kOutReservoir]), 18, ink2);
            addHit(wx, y, cw, 80, [this] { toggle(kParamBlowerOn); });
        }
        // the notes sounding: key, where it came from, its foot pressure and its pitch
        {
            const float ny = 540, nw = w / (float)kNumShownNotes;
            for (int i = 0; i < kNumShownNotes; ++i)
            {
                const float* n = &pv[kOutNote0 + i * 4];
                const float nx = x + i * nw;
                if (n[0] < 0) { text(nx, ny, "--", 18, ghost); continue; }
                const int k = (int)n[0], src = (int)n[1];
                const int dd = (src % 1000) / 100, rest = src % 100, zone = rest >= 16 ? 1 : 0, ch = rest - zone * 16;
                const KeyPitch nk = tuning.keyPitch(k, (int)(zone ? pv[kParamMapHi] : pv[kParamMapLo]), on(kParamEnharmonic));
                const std::string from = src >= 1000 ? std::string("CPL ") + divisionShort(dd) : std::string(zone ? "HI" : "LO") + fmt(" CH%.0f", ch);
                text(nx, ny - 8, std::string(nk.name) + std::to_string(k / 12 - 1) + " " + from, 17);
                text(nx, ny + 10, fmt("%.0f MM ", n[2]) + fmt("%+.0f C", n[3]) + (n[2] > 100 ? " OVBL" : ""), 17, ink2);
            }
        }
        // couplers
        y = 568;
        text(x, y, "COUPLERS", 18, ink2);
        int c = 0;
        const float tw = (w - 5 * 6) / 6.f;
        for (int s = 0; s < kNumStops; ++s)
        {
            if (kStops[(size_t)s].type != kStopCoupler) continue;
            std::string l = kStops[(size_t)s].label;
            for (size_t p; (p = l.find(" / ")) != std::string::npos;) l.replace(p, 3, "/");
            for (size_t p; (p = l.find("PEDAL")) != std::string::npos;) l.replace(p, 5, "P");
            const float bx = x + c * (tw + 6);
            const bool isOn = on(s);
            beginPath(); roundedRect(bx, y + 14, tw, 42, 8);
            if (isOn) { fillColor(ink); NanoVG::fill(); }
            strokeColor(ink); strokeWidth(1.5f); stroke();
            text(bx + tw / 2, y + 36, l, 21, isOn ? lcd : ink, ALIGN_CENTER | ALIGN_MIDDLE);
            addHit(bx, y + 14, tw, 42, [this, s] { toggle(s); });
            ++c;
        }
        // combination pistons: 1 - 7, SET (then a piston stores), GC (general cancel), + (presets)
        y = 648;
        text(x, y, setMode ? "COMBINATIONS · PRESS A PISTON TO STORE" : "COMBINATIONS", 18, setMode ? ink : ink2);
        const float pw = (w - 9 * 6) / 10.f;
        for (int i = 0; i < 10; ++i)
        {
            const float bx = x + i * (pw + 6), cx = bx + pw / 2, cy = y + 40;
            std::string l = i < 7 ? fmt("%.0f", i + 1) : i == 7 ? "SET" : i == 8 ? "GC" : "+";
            const bool lit = i < 7 ? (i == lastPiston) : i == 7 ? setMode : false;
            beginPath(); ellipse(cx, cy, pw / 2, 22);
            if (lit) { fillColor(ink); NanoVG::fill(); }
            strokeColor(ink); strokeWidth(1.5f); stroke();
            text(cx, cy + 1, l, 21, lit ? lcd : ink, ALIGN_CENTER | ALIGN_MIDDLE);
            addHit(bx, cy - 22, pw, 44, [this, i] {
                if (i < 7) { if (setMode) { storePiston(i); setMode = false; toast = fmt("COMBINATION %.0f STORED", i + 1); } else recallPiston(i); lastPiston = i; }
                else if (i == 7) setMode = !setMode;
                else if (i == 8) { for (int s = 0; s < kNumStops; ++s) if (on(s)) toggle(s); lastPiston = -1; toast = "GENERAL CANCEL"; }
                else { screen = kScrPresets; scanUserPresets(); }
            });
        }
        // a key that cannot sound says why
        const int sk = (int)pv[kOutSilentKey];
        if (sk >= 1000)
        {
            const int key = sk % 1000;
            text(x, kDrawerBottom - 12, std::string("KEY ") + noteName12(key) + (sk >= 2000 ? ": NO STOP DRAWN ON ITS DIVISION" : ": OUTSIDE THE COMPASS"), 18, ink);
        }
    }

    // ============================================================ workshop
    // The pipe as built: its drawing and dimensions. Every dimension can be changed; the pipe is then
    // rebuilt from its geometry with the voicer's own models (PipeEdit.hpp) and sounds as built that way.
    struct WsResult { bool ok = false, reed = false; std::string why; FluePipeSpec flue{}; ReedPipeSpec rd{}; int rank = -1, key = -1; std::string text; };
    WsResult rebuildSelected(int rank, int key, PipeEdit e, std::string text)
    {
        WsResult r; r.rank = rank; r.key = key; r.text = std::move(text);
        if (const FluePipeSpec* b = organFlueSpec(rank, key)) { r.ok = edit::editFlue(rank, key, *b, e, r.flue, r.why); }
        else if (const ReedPipeSpec* b = organReedSpec(rank, key)) { r.reed = true; r.ok = edit::editReed(rank, key, *b, e, r.rd, r.why); }
        else r.why = "no pipe at this key";
        return r;
    }
    void setEdit(int field, float value)
    {
        PipeEdit& e = pipeEdits[selRank * 128 + selKey];
        e.v[field] = value;
        if (!e.any()) pipeEdits.erase(selRank * 128 + selKey);
        setStateValue(kStatePipeEdits, writePipeEdits(pipeEdits));
        wsKey = -1;
    }
    void drawWorkshop()
    {
        const float top = 84;
        // rank list, one division at a time
        static const int tabDiv[4] = { kPedal, kGreat, kPositive, kSwell };
        for (int t = 0; t < 4; ++t) {
            const int dv = tabDiv[t];
            button(20 + t * 56, top, 52, 26, divisionShort(dv), kRanks[selRank].division == dv, [this, dv] {
                for (int r = 0; r < kNumRanks; ++r) if (kRanks[r].division == dv) { selRank = r; break; }
            }, 19);
        }
        float y = top + 32;
        for (int r = 0; r < kNumRanks; ++r)
        {
            if (kRanks[r].division != kRanks[selRank].division) continue;
            const bool sel = r == selRank;
            int nEd = 0; for (const auto& e : pipeEdits) if (e.first / 128 == r) ++nEd;
            if (sel) fill(20, y, 220, 21, ink);
            std::string nm = kRanks[r].name; if (nm.size() > 22) nm = nm.substr(0, 22);
            text(26, y + 11, nm, 19, sel ? lcd : ink);
            if (nEd) text(234, y + 11, fmt("%.0f*", nEd), 17, sel ? lcd : ink2, ALIGN_RIGHT | ALIGN_MIDDLE);
            addHit(20, y, 220, 21, [this, r] { selRank = r; });
            y += 21;
        }
        vline(250, 76, kH - 90);

        // key: chosen here, or following the keys played
        const int d = kRanks[selRank].division;
        const int lo = d == kPedal ? kPedalLow : kManualLow, hi = d == kPedal ? kPedalHigh : kManualHigh;
        if (wsFollow && pv[kOutLiveKey] > 0) selKey = (int)pv[kOutLiveKey];
        selKey = std::max(lo, std::min(hi, selKey));
        const float mx = 266, mw = 360;
        const FluePipeSpec* bf = organFlueSpec(selRank, selKey);
        const ReedPipeSpec* br = organReedSpec(selRank, selKey);
        titleBar(mx, top, mw, std::string(kRanks[selRank].name), noteName12(selKey));
        button(mx, top + 34, 40, 34, "<", false, [this, lo] { wsFollow = false; selKey = std::max(lo, selKey - 1); }, 22);
        text(mx + 150, top + 52, std::string("KEY ") + noteName12(selKey) + (bf ? fmt("  %.1f HZ", bf->target) : br ? fmt("  %.1f HZ", br->target) : std::string("")), 21, ink, ALIGN_CENTER | ALIGN_MIDDLE);
        button(mx + 256, top + 34, 40, 34, ">", false, [this, hi] { wsFollow = false; selKey = std::min(hi, selKey + 1); }, 22);
        button(mx + 302, top + 34, 58, 34, "FOLLOW", wsFollow, [this] { wsFollow = !wsFollow; }, 17);

        // the edited pipe, rebuilt when the pipe or its edit changes (in the background: a harmonic pipe
        // takes a few seconds to match)
        const auto it = pipeEdits.find(selRank * 128 + selKey);
        const PipeEdit e = it == pipeEdits.end() ? PipeEdit() : it->second;
        const std::string etext = it == pipeEdits.end() ? std::string() : writePipeEdits({ { it->first, it->second } });
        const bool edited = it != pipeEdits.end();
        if (edited && (wsResult.rank != selRank || wsResult.key != selKey || wsResult.text != etext) && !wsFuture.valid())
        {
            const int r = selRank, k = selKey;
            wsFuture = std::async(std::launch::async, [this, r, k, e, etext] { return rebuildSelected(r, k, e, etext); });
        }
        const bool have = edited && wsResult.ok && wsResult.rank == selRank && wsResult.key == selKey && wsResult.text == etext;
        const bool waiting = edited && !have && wsFuture.valid();

        // drawing
        const float dx = mx, dy = top + 84, dw = mw, dh = 560;
        frame(dx, dy, dw, dh, 1.f);
        if (!bf && !br) text(dx + dw / 2, dy + dh / 2, "NO PIPE AT THIS KEY", 21, ink2, ALIGN_CENTER | ALIGN_MIDDLE);
        else if (bf) drawFluePipe(dx, dy, dw, dh, have ? wsResult.flue : *bf);
        else drawReedPipe(dx, dy, dw, dh, have ? wsResult.rd : *br);

        // dimensions
        const float x = 644, w = 616;
        titleBar(x, top, w, "DIMENSIONS", edited ? "EDITED" : "AS VOICED");
        text(x + 250, top + 44, "AS VOICED", 17, ink2, ALIGN_RIGHT | ALIGN_MIDDLE);
        text(x + 452, top + 44, "NOW", 17, ink2, ALIGN_CENTER | ALIGN_MIDDLE);
        text(x + w, top + 44, "CHANGE", 17, ink2, ALIGN_RIGHT | ALIGN_MIDDLE);
        float ry = top + 58;
        auto row = [&](const char* label, double base, int field, const char* unit, double mul, const char* f) {
            const double ratio = field == kEdBeard ? 1.0 : e.v[field];
            text(x, ry + 17, label, 19);
            text(x + 250, ry + 17, fmt(f, base * mul) + unit, 19, ink2, ALIGN_RIGHT | ALIGN_MIDDLE);
            std::string now;
            if (field == kEdBeard) now = e.v[kEdBeard] < 0 ? (base > 0 ? fmt("%.2f", base) : std::string("NONE")) : (e.v[kEdBeard] == 0 ? std::string("NONE") : fmt("%.2f", e.v[kEdBeard]));
            else now = fmt(f, base * mul * ratio) + unit;
            stepper(x + 270, ry, 346 - 92, "", now,
                    [this, field, ratio, base] { if (field == kEdBeard) { const float c = pipeEdits.count(selRank * 128 + selKey) && pipeEdits[selRank * 128 + selKey].v[kEdBeard] >= 0 ? pipeEdits[selRank * 128 + selKey].v[kEdBeard] : (float)base; setEdit(kEdBeard, std::max(0.f, c - 0.05f)); } else setEdit(field, (float)(ratio / 1.01)); },
                    [this, field, ratio, base] { if (field == kEdBeard) { const float c = pipeEdits.count(selRank * 128 + selKey) && pipeEdits[selRank * 128 + selKey].v[kEdBeard] >= 0 ? pipeEdits[selRank * 128 + selKey].v[kEdBeard] : (float)base; setEdit(kEdBeard, std::min(2.f, c + 0.05f)); } else setEdit(field, (float)(ratio * 1.01)); }, 120.f);
            if (field != kEdBeard) text(x + w, ry + 17, std::fabs(ratio - 1.0) < 1e-6 ? std::string("-") : fmt("%+.1f%%", 100.0 * (ratio - 1.0)), 19, ink, ALIGN_RIGHT | ALIGN_MIDDLE);
            ry += 44;
        };
        if (bf)
        {
            row("BODY LENGTH", bf->length, kEdBodyLength, " MM", 1000, "%.1f");
            row("BODY DIAMETER", 2 * bf->radius, kEdBodyDiameter, " MM", 1000, "%.1f");
            row("MOUTH WIDTH", bf->Wm, kEdMouthWidth, " MM", 1000, "%.1f");
            row("CUT-UP", bf->W, kEdCutup, " MM", 1000, "%.2f");
            row("FLUE", bf->h, kEdFlue, " MM", 1000, "%.3f");
            row("WINDWAY LENGTH", bf->languid, kEdLanguid, " MM", 1000, "%.1f");
            row("FOOT PRESSURE", bf->footPa / 9.80665, kEdFootPressure, " MM", 1, "%.1f");
            row("BEARD (X CUT-UP)", bf->beard, kEdBeard, "", 1, "%.2f");
        }
        else if (br)
        {
            row("TONGUE THICKNESS", br->t, kEdTongueThick, " MM", 1000, "%.3f");
            row("TONGUE LENGTH", br->L, kEdTongueLength, " MM", 1000, "%.1f");
            row("TONGUE WIDTH", br->w, kEdTongueWidth, " MM", 1000, "%.2f");
            row("TONGUE CURVE (OPENING)", br->x0, kEdTongueCurve, " MM", 1000, "%.3f");
            row("RESONATOR LENGTH", br->length, kEdResLength, " MM", 1000, "%.1f");
            row("RESONATOR DIAMETER", 2 * br->lowerR, kEdResDiameter, " MM", 1000, "%.1f");
            row("BOOT PRESSURE", br->footPa / 9.80665, kEdBootPressure, " MM", 1, "%.1f");
        }
        // what follows from them
        ry += 6;
        hline(x, ry, w, true);
        ry += 10;
        auto info = [&](const std::string& k, const std::string& v) { text(x, ry + 10, k, 19, ink2); text(x + w, ry + 10, v, 19, ink, ALIGN_RIGHT | ALIGN_MIDDLE); ry += 24; };
        if (bf)
        {
            const FluePipeSpec& sp = have ? wsResult.flue : *bf;
            static const char* const shapes[4] = { "OPEN", "STOPPED", "CHIMNEY", "CONICAL" };
            info("SHAPE · MATERIAL", std::string(shapes[std::max(0, std::min(3, (int)sp.shape))]) + " · " + (sp.wood > 0.5f ? std::string("WOOD") : kRanks[selRank].metalTin > 0 ? fmt("%.0f%% TIN", kRanks[selRank].metalTin) : std::string("METAL")));
            info("TOE HOLE", sp.toe > 0 ? fmt("%.1f MM", sp.toe * 1000) : std::string("OPEN"));
            std::string res; for (int i = 0; i < std::min(3, sp.nModes); ++i) res += (i ? " · " : "") + fmt("%.1f", sp.f[i]);
            info("RESONANCES (HZ)", res);
            const double u = std::sqrt(2.0 * sp.footPa / 1.2);
            info("JET SPEED · TRANSIT", fmt("%.1f M/S · ", u) + fmt("%.2f MS", 1000.0 * sp.transit / u));
            const PipeGeometry g = pipeGeometry(*voicing, selRank, selKey, sp.target);
            const BuildCheck bc = checkPipe(g);
            const bool ok = bc.mouthOk && bc.cutupOk && bc.sizeOk && bc.pressureOk;
            info("BUILD CHECK", ok ? "BUILDABLE" : (!bc.cutupOk ? "CUT-UP WON'T SPEAK" : !bc.mouthOk ? "MOUTH TOO WIDE" : !bc.sizeOk ? "SIZE NOT BUILDABLE" : "WIND OUT OF RANGE"));
        }
        else if (br)
        {
            const ReedPipeSpec& sp = have ? wsResult.rd : *br;
            info("TONGUE FREQUENCY", fmt("%.1f HZ", sp.fr));
            std::string res; for (int i = 0; i < std::min(3, sp.nModes); ++i) res += (i ? " · " : "") + fmt("%.1f", sp.f[i]);
            info("RESONATOR RESONANCES (HZ)", res);
            info("RESONATOR TOP DIAMETER", fmt("%.1f MM", 2000.0 * sp.upperR));
            info("BOOT", fmt("%.2f L", sp.bootVolume * 1000.0) + (sp.shallotL > 0 ? fmt(" · SHALLOT %.0f MM", sp.shallotL * 1000) : std::string("")));
        }
        // state of the edit
        ry += 8;
        frame(x, ry, w, 56, 1.5f);
        std::string status = !edited ? "AS VOICED: THE PIPE THAT PASSED THE VOICER'S CHECKS"
                           : waiting ? "REBUILDING THE PIPE FROM ITS GEOMETRY..."
                           : have ? "EDITED: REBUILT FROM ITS GEOMETRY; NOT CHECKED BY THE VOICER"
                           : "CANNOT EDIT THIS PIPE: " + upper(wsResult.why);
        text(x + 10, ry + 18, status, 18);
        text(x + 10, ry + 40, "DRAW THE STOP AND PLAY THE KEY TO HEAR IT; THE TUNER BRINGS IT TO PITCH", 17, ink2);
        ry += 66;
        button(x, ry, 200, 38, "RESET THIS PIPE", false, [this] { pipeEdits.erase(selRank * 128 + selKey); setStateValue(kStatePipeEdits, writePipeEdits(pipeEdits)); wsKey = -1; }, 18);
        button(x + 208, ry, 200, 38, "RESET THIS RANK", false, [this] {
            for (auto i = pipeEdits.begin(); i != pipeEdits.end();) i = (i->first / 128 == selRank) ? pipeEdits.erase(i) : std::next(i);
            setStateValue(kStatePipeEdits, writePipeEdits(pipeEdits)); wsKey = -1; }, 18);
        button(x + 416, ry, 200, 38, fmt("RESET ALL (%.0f)", (double)pipeEdits.size()), false, [this] { pipeEdits.clear(); setStateValue(kStatePipeEdits, ""); wsKey = -1; }, 18);
    }

    // a flue pipe drawn to scale (the width exaggerated when a tall pipe would be a line, and said so)
    void drawFluePipe(float x, float y, float w, float h, const FluePipeSpec& sp)
    {
        const double L = sp.length, D = 2 * sp.radius, foot = std::min(0.35, std::max(0.08, 0.25 * L));
        const double total = L + foot;
        double sc = (h - 70) / total;
        double wx = 1.0;
        if (D * sc < 22) wx = 22.0 / (D * sc);
        if (D * sc * wx > w * 0.5) sc = w * 0.5 / (D * wx);
        const float cx = x + w * 0.42f, by = y + h - 30;                  // toe at the bottom
        const float bodyW = (float)(D * sc * wx), footH = (float)(foot * sc), bodyH = (float)(L * sc);
        const float mouthY = by - footH;                                  // mouth at the top of the foot
        strokeColor(ink); strokeWidth(1.5f);
        // foot: a cone from the toe up to the body
        beginPath(); moveTo(cx - 3, by); lineTo(cx - bodyW / 2, mouthY); moveTo(cx + 3, by); lineTo(cx + bodyW / 2, mouthY); stroke();
        // body (conical: narrower at the top)
        const float topW = sp.shape == 3.f ? bodyW * 0.5f : bodyW;
        beginPath(); moveTo(cx - bodyW / 2, mouthY); lineTo(cx - topW / 2, mouthY - bodyH); moveTo(cx + bodyW / 2, mouthY); lineTo(cx + topW / 2, mouthY - bodyH); stroke();
        if (sp.shape == 1.f || sp.shape == 2.f) { fill(cx - topW / 2 - 2, mouthY - bodyH - 4, topW + 4, 6, ink); }
        if (sp.shape == 2.f) { beginPath(); rect(cx - topW * 0.12f, mouthY - bodyH - 26, topW * 0.24f, 22); stroke(); }
        // mouth: its width across the front, the cut-up above the lower lip
        const float mwid = (float)std::min((double)bodyW, sp.Wm / (M_PI * D) * bodyW * M_PI / 2.0);
        const float cut = std::max(3.f, (float)(sp.W * sc));
        beginPath(); rect(cx - mwid / 2, mouthY - cut, mwid, cut); stroke();
        // dimension lines
        auto dim = [&](float x1, float y1, float x2, float y2, const std::string& t, bool vertical) {
            beginPath(); moveTo(x1, y1); lineTo(x2, y2); strokeColor(ink2); strokeWidth(1.f); stroke();
            if (vertical) text(x1 + 6, (y1 + y2) / 2, t, 17, ink2); else text((x1 + x2) / 2, y1 - 10, t, 17, ink2, ALIGN_CENTER | ALIGN_MIDDLE);
        };
        const float rx = cx + std::max(bodyW, topW) / 2 + 18;
        dim(rx, mouthY, rx, mouthY - bodyH, fmt(L >= 1 ? "%.3f M" : "%.0f MM", L >= 1 ? L : L * 1000), true);
        dim(cx - bodyW / 2, mouthY - bodyH - 36, cx + bodyW / 2, mouthY - bodyH - 36, fmt("D %.0f MM", D * 1000), false);
        text(cx - bodyW / 2 - 8, mouthY - cut / 2, fmt("MOUTH %.0f", sp.Wm * 1000) + fmt(" x %.1f MM", sp.W * 1000), 17, ink2, ALIGN_RIGHT | ALIGN_MIDDLE);
        text(x + 8, y + h - 14, wx > 1.01 ? fmt("TO SCALE; WIDTH x%.1f", wx) : std::string("TO SCALE"), 16, ink2);
    }

    // a reed pipe: the boot (with the tongue on its shallot inside), the resonator above
    void drawReedPipe(float x, float y, float w, float h, const ReedPipeSpec& sp)
    {
        const double L = sp.length, bootH = std::max(0.12, 3.0 * sp.L), total = L + bootH;
        double sc = (h - 70) / total, wx = 1.0;
        const double Dmax = 2 * std::max(sp.lowerR, sp.upperR);
        if (Dmax * sc < 22) wx = 22.0 / (Dmax * sc);
        if (Dmax * sc * wx > w * 0.5) sc = w * 0.5 / (Dmax * wx);
        const float cx = x + w * 0.42f, by = y + h - 30;
        const float bh = (float)(bootH * sc), bw = std::max(30.f, (float)(2 * sp.lowerR * sc * wx) + 16);
        strokeColor(ink); strokeWidth(1.5f);
        beginPath(); moveTo(cx - 3, by); lineTo(cx - bw / 2, by - bh * 0.35f); lineTo(cx - bw / 2, by - bh); lineTo(cx + bw / 2, by - bh); lineTo(cx + bw / 2, by - bh * 0.35f); lineTo(cx + 3, by); stroke();
        // shallot and tongue inside the boot
        const float tl = (float)(sp.L * sc);
        beginPath(); rect(cx - 4, by - bh, 8, std::min(bh * 0.9f, tl + 6)); strokeColor(ink2); strokeWidth(1.f); stroke();
        beginPath(); moveTo(cx + 5, by - bh); lineTo(cx + 5 + 3, by - bh + std::min(bh * 0.9f, tl)); stroke();
        // resonator
        const float lw = (float)(2 * sp.lowerR * sc * wx), uw = (float)(2 * sp.upperR * sc * wx), rh = (float)(L * sc);
        beginPath(); moveTo(cx - lw / 2, by - bh); lineTo(cx - uw / 2, by - bh - rh); lineTo(cx + uw / 2, by - bh - rh); lineTo(cx + lw / 2, by - bh); strokeColor(ink); strokeWidth(1.5f); stroke();
        const float rx = cx + std::max(lw, uw) / 2 + 18;
        beginPath(); moveTo(rx, by - bh); lineTo(rx, by - bh - rh); strokeColor(ink2); strokeWidth(1.f); stroke();
        text(rx + 6, by - bh - rh / 2, fmt(L >= 1 ? "%.3f M" : "%.0f MM", L >= 1 ? L : L * 1000), 17, ink2);
        text(cx, by - bh - rh - 14, fmt("TOP D %.0f MM", 2000 * sp.upperR), 17, ink2, ALIGN_CENTER | ALIGN_MIDDLE);
        text(cx - bw / 2 - 8, by - bh * 0.6f, fmt("TONGUE %.0f MM", sp.L * 1000), 17, ink2, ALIGN_RIGHT | ALIGN_MIDDLE);
        text(x + 8, y + h - 14, wx > 1.01 ? fmt("TO SCALE; WIDTH x%.1f", wx) : std::string("TO SCALE"), 16, ink2);
    }

    // ========================================================== expression
    void drawExpression()
    {
        const float top = 84;
        // ---------------------------------------------------------- inputs
        float x = 20, w = 290, y = top;
        text(x, y + 12, "NOTE INPUTS (MPE ZONES)", 22);
        y += 30;
        // channels notes actually arrive on (all zones), from what Pipe received - not the layout
        {
            const int recent = (int)pv[kOutViaHi];
            std::string chans;
            int first = -1;
            for (int c = 0; c <= 16; ++c)
            {
                const bool on = c < 16 && (recent & (1 << c));
                if (on && first < 0) first = c;
                if (!on && first >= 0)
                {
                    if (!chans.empty()) chans += ",";
                    chans += first == c - 1 ? fmt("%.0f", first + 1) : fmt("%.0f", first + 1) + "-" + fmt("%.0f", c);
                    first = -1;
                }
            }
            titleBar(x, y, w, "NOTES ARRIVING ON", chans.empty() ? std::string("NO NOTES YET") : "CH " + chans);
            y += 30;
            if (recent & (1 << 16))
            {
                // a fact, shown as it is: several notes held at once on the same channel
                fill(x, y, w, 44, ink);
                text(x + 8, y + 12, "SEVERAL NOTES SHARE A CHANNEL:", 19, lcd);
                text(x + 8, y + 32, "IS MPE ON FOR PIPE IN THE DAW?", 19, lcd);
                y += 50;
            }
        }
        for (int z = 0; z < 2; ++z)
        {
            const int param = z == 0 ? kParamZoneLoDiv : kParamZoneHiDiv;
            const int members = (int)pv[z == 0 ? kOutMembersLo : kOutMembersHi];
            const int in = (int)pv[z == 0 ? kOutSeenLo : kOutSeenHi];
            titleBar(x, y, w, z == 0 ? "LOWER ZONE" : "UPPER ZONE", std::string("> ") + kZoneDivLabels[(int)pv[param]]);
            addHit(x, y, w, 26, [this, param] { setParam(param, (float)(((int)pv[param] + 1) % 4)); });
            float ry = y + 30;
            auto row = [&](const char* k, const std::string& v) { text(x + 6, ry + 10, k, 20, ink2); text(x + w - 6, ry + 10, v, 20, ink, ALIGN_RIGHT | ALIGN_MIDDLE); ry += 22; };
            if (z == 0) row("LAYOUT", members > 0 ? fmt("MASTER 1, NOTES 2-%.0f", members + 1) : std::string("OFF"));
            else row("LAYOUT", members > 0 ? fmt("MASTER 16, NOTES %.0f-15", 15 - members) : std::string("OFF (NO MCM)"));
            if (z == 0 || members > 0)
            {
                row("SET BY", in & kInMcm ? "MPE CONFIG MESSAGE" : "DEFAULT (RP-053 2.1.3)");
                row("BEND RANGE", fmt("±%.0f ST", pv[z == 0 ? kOutBendLo : kOutBendHi]));
                std::string pr, at;
                if (in & kInMemberPressure) pr += "CH PRESSURE ";
                if (in & kInNoteExpression) pr += "HOST PER-NOTE ";
                if (in & kInHiRes) pr += "14-BIT";
                if (in & kInCC74) at += "CC74 ";
                if (in & kInPolyPressure) at += "POLY AT ";
                if (in & (kInMasterPressure | kInNonMpePressure)) at += "CH AT";
                row("PRESSURE", pr.empty() ? std::string("NONE RECEIVED") : pr);
                row("2ND STAGE", at.empty() ? std::string("NONE RECEIVED") : at);
            }
            frame(x, y, w, ry - y + 6);
            y = ry + 14;
        }
        const int split = (int)pv[kParamPedalSplit];
        stepper(x, y, w, "PEDAL SPLIT", split > 0 ? std::string("< ") + noteName12(split) : std::string("OFF"),
                [this] { nudge(kParamPedalSplit, -1); }, [this] { float v = pv[kParamPedalSplit]; nudge(kParamPedalSplit, v < 1 ? 48 : 1); }, 100);
        y += 52;
        // live sources of the last note touched
        const int key = (int)pv[kOutLiveKey];
        titleBar(x, y, w, "LIVE SOURCES", key >= 0 ? noteName12(key + 36) + fmt(" CH %.0f", pv[kOutLiveChannel] + 1) : std::string("NO NOTE"));
        y += 30;
        auto live = [&](const char* k, float v, float max, bool bipolar = false, const char* f = "%.0f") {
            text(x + 6, y + 9, k, 19, ink2);
            if (v < 0.f && !bipolar) text(x + w - 6, y + 9, "NOT SENT", 19, ink2, ALIGN_RIGHT | ALIGN_MIDDLE);
            else
            {
                segBar(x + 120, y + 3, 110, 12, 10, bipolar ? 0.5f + v / (2.f * max) : v / max);
                text(x + w - 6, y + 9, fmt(f, v), 19, ink, ALIGN_RIGHT | ALIGN_MIDDLE);
            }
            y += 21;
        };
        live("STRIKE", key >= 0 ? pv[kOutLiveStrike] : -1.f, 127.f);
        live("PRESSURE", key >= 0 ? pv[kOutLivePressure] : -1.f, 127.f);
        live("AFTERTOUCH", key >= 0 ? pv[kOutLiveAftertouch] : -1.f, 127.f);
        live("CC74", key >= 0 ? pv[kOutLiveCC74] : -1.f, 127.f);
        live("BEND", pv[kOutLiveBend], 200.f, true, "%+.0fC");
        vline(x + w + 12, 76, kH - 90);

        // ---------------------------------------------------------- matrix
        x = 336; w = 600;
        titleBar(x, top, w, "MODULATION MATRIX", "CLICK A CELL TO CHANGE IT");
        y = top + 32;
        const float cN = x, cOn = x + 30, cSrc = x + 74, cZone = x + 214, cCurve = x + 264, cAmt = x + 318, cDst = x + 456;
        text(cN, y + 9, "#", 19, ink2); text(cOn, y + 9, "ON", 19, ink2); text(cSrc, y + 9, "SOURCE", 19, ink2);
        text(cZone, y + 9, "ZONE", 19, ink2); text(cCurve, y + 9, "CURVE", 19, ink2);
        text(cAmt, y + 9, "AMOUNT (DRAG)", 19, ink2); text(cDst, y + 9, "DESTINATION", 19, ink2);
        y += 22;
        for (int i = 0; i < kNumModRows; ++i)
        {
            ModRow& r = matrix.rows[i];
            const bool sel = i == selRow;
            const float rh = 29;
            if (sel) fill(x, y, w, rh, ink);
            else hline(x, y + rh, w, true);
            const Color c = sel ? lcd : (r.on ? ink : ink2);
            text(cN + 4, y + rh / 2, fmt("%.0f", i + 1), 21, c);
            addHit(cN, y, 28, rh, [this, i] { selRow = i; });
            text(cOn, y + rh / 2, r.on ? "[X]" : "[ ]", 21, c);
            addHit(cOn, y, 40, rh, [this, i] { selRow = i; matrix.rows[i].on = !matrix.rows[i].on; commitMatrix(); });
            text(cSrc, y + rh / 2, ModMatrix::sourceLabel(r), 21, c);
            addHit(cSrc, y, 136, rh, [this, i] {
                selRow = i; ModRow& m = matrix.rows[i];
                m.src = (m.src + 1) % kNumModSources;
                m.restMid = m.src == kSrcBend;
                commitMatrix(); });
            const bool global = modDestGlobal(r.dest);
            text(cZone, y + rh / 2, global && r.src == kSrcCC ? "-" : kModZoneLabels[r.zone], 21, c);
            addHit(cZone, y, 46, rh, [this, i] { selRow = i; matrix.rows[i].zone = (matrix.rows[i].zone + 1) % kNumZones; commitMatrix(); });
            text(cCurve, y + rh / 2, kModCurveLabels[r.curve], 21, c);
            addHit(cCurve, y, 50, rh, [this, i] { selRow = i; matrix.rows[i].curve = (matrix.rows[i].curve + 1) % kNumCurves; commitMatrix(); });
            // amount: bipolar bar
            const float bx = cAmt, bw = 70, mid = bx + bw / 2;
            fill(bx, y + rh / 2 - 1, bw, 2, sel ? lcd : ghost);
            const float ax = mid + r.amount * bw / 2;
            fill(std::min(mid, ax), y + 8, std::fabs(ax - mid), rh - 16, c);
            fill(mid - 1, y + 5, 2, rh - 10, c);
            text(bx + bw + 8, y + rh / 2, fmt("%+.2f", r.amount), 20, c);
            hits.push_back({ bx - 4, y, 130, rh, [this, i] { selRow = i; dragBase = matrix.rows[i].amount; }, -1,
                             [this, i](float dy) { matrix.rows[i].amount = std::max(-1.f, std::min(1.f, std::round((dragBase + dy / 150.f) * 100.f) / 100.f)); commitMatrix(); } });
            text(cDst, y + rh / 2, kModDestLabels[r.dest], 21, c);
            addHit(cDst, y, x + w - cDst, rh, [this, i] { selRow = i; matrix.rows[i].dest = (matrix.rows[i].dest + 1) % kNumModDests; commitMatrix(); });
            y += rh;
        }
        text(x, y + 14, "ROWS ON ONE DESTINATION ADD. A SOURCE NOT SENT LEAVES THE VOICED DEFAULT.", 19, ink2);
        y += 32;

        // row editor
        ModRow& r = matrix.rows[selRow];
        titleBar(x, y, w, fmt("ROW %.0f  ", selRow + 1) + ModMatrix::sourceLabel(r) + " > " + kModDestLabels[r.dest],
                 learnArmed ? "LEARNING: MOVE A CONTROL" : "");
        y += 32;
        const float hw = w / 2 - 10;
        auto rowStep = [&](float sx, float sy, const std::string& label, const std::string& value, std::function<void()> minus, std::function<void()> plus) {
            stepper(sx, sy, hw, label, value, std::move(minus), std::move(plus), 90);
        };
        rowStep(x, y, "RANGE FROM", fmt("%.0f%%", r.lo * 100), [this] { auto& m = matrix.rows[selRow]; m.lo = std::max(0.f, m.lo - 0.05f); commitMatrix(); },
                [this] { auto& m = matrix.rows[selRow]; m.lo = std::min(m.hi - 0.05f, m.lo + 0.05f); commitMatrix(); });
        rowStep(x + hw + 20, y, "RANGE TO", fmt("%.0f%%", r.hi * 100), [this] { auto& m = matrix.rows[selRow]; m.hi = std::max(m.lo + 0.05f, m.hi - 0.05f); commitMatrix(); },
                [this] { auto& m = matrix.rows[selRow]; m.hi = std::min(1.f, m.hi + 0.05f); commitMatrix(); });
        y += 46;
        rowStep(x, y, "SMOOTHING", fmt("%.0f MS", r.smoothMs), [this] { auto& m = matrix.rows[selRow]; m.smoothMs = std::max(0.f, m.smoothMs - 5.f); commitMatrix(); },
                [this] { auto& m = matrix.rows[selRow]; m.smoothMs = std::min(500.f, m.smoothMs + 5.f); commitMatrix(); });
        rowStep(x + hw + 20, y, "AMOUNT", fmt("%+.2f", r.amount), [this] { auto& m = matrix.rows[selRow]; m.amount = std::max(-1.f, m.amount - 0.05f); commitMatrix(); },
                [this] { auto& m = matrix.rows[selRow]; m.amount = std::min(1.f, m.amount + 0.05f); commitMatrix(); });
        y += 46;
        if (r.src == kSrcCC)
            rowStep(x, y, "CC NUMBER", fmt("%.0f", r.cc), [this] { auto& m = matrix.rows[selRow]; m.cc = (m.cc + 127) % 128; commitMatrix(); },
                    [this] { auto& m = matrix.rows[selRow]; m.cc = (m.cc + 1) % 128; commitMatrix(); });
        else text(x, y + 20, "CC NUMBER  (CC SOURCE ONLY)", 20, ink2);
        const float bx2 = x + hw + 20, bw2 = (hw - 12) / 3;
        button(bx2, y, bw2, 40, r.restMid ? "REST MID" : "REST 0", r.restMid, [this] { matrix.rows[selRow].restMid = !matrix.rows[selRow].restMid; commitMatrix(); }, 20);
        button(bx2 + bw2 + 6, y, bw2, 40, "LEARN", learnArmed, [this] {
            learnArmed = !learnArmed; learnFirst = -1; learnBest = -1;
            learnLast = (int)pv[kOutLearn] / 1024; }, 20);
        button(bx2 + 2 * (bw2 + 6), y, bw2, 40, "CLEAR", false, [this] { matrix.rows[selRow] = ModRow(); commitMatrix(); }, 20);
        y += 50;
        button(x, y, 150, 32, "ALL DEFAULTS", false, [this] { matrix.setDefaults(); commitMatrix(); toast = "MATRIX RESET TO DEFAULTS"; }, 19);
        text(x + 166, y + 16, modDestGlobal(r.dest) ? "GLOBAL: FOLLOWS A CC OR THE LAST NOTE PLAYED" : "PER NOTE: EACH PALLET HAS ITS OWN VALUE", 19, ink2);
        y += 42;
        hline(x, y, w, true);
        y += 8;
        miniStep(x, y, "FAST", kParamPalletFast, 1.f, "%.0fMS");
        miniStep(x + 200, y, "SOFT", kParamPalletSlow, 5.f, "%.0fMS");
        miniStep(x + 400, y, "CLOSE", kParamReleaseMs, 1.f, "%.0fMS");
        y += 30;
        text(x, y + 12, "PALLET TIMES: STRIKE AND RELEASE VELOCITY MOVE BETWEEN THESE", 19, ink2);
        vline(x + w + 12, 76, kH - 90);

        // ---------------------------------------------------------- right
        x = 962; w = 298; y = top;
        titleBar(x, y, w, "DESTINATIONS NOW");
        y += 32;
        auto dst = [&](const char* k, const std::string& v, float frac) {
            text(x + 6, y + 9, k, 19, ink2);
            if (frac >= 0.f) segBar(x + 128, y + 3, 90, 12, 8, frac);
            text(x + w - 6, y + 9, v, 19, ink, ALIGN_RIGHT | ALIGN_MIDDLE);
            y += 21;
        };
        const bool note = key >= 0;
        dst("PALLET", note ? fmt("%.0f%%", pv[kOutDstPallet]) : "-", note ? pv[kOutDstPallet] / 100.f : 0.f);
        dst("WIND BOOST", note ? fmt("%+.0fMM", pv[kOutDstBoost]) : "-", note ? pv[kOutDstBoost] / std::max(1.f, pv[kParamPressMax] - 78.f) : 0.f);
        dst("FOOT", note ? fmt("%.0fMM", pv[kOutDstFoot]) : "-", note ? pv[kOutDstFoot] / 160.f : 0.f);
        dst("JET AIM", note ? fmt("%+.2f", pv[kOutDstJet]) : "-", note ? 0.5f + 0.5f * pv[kOutDstJet] : 0.5f);
        dst("PITCH", note ? fmt("%+.0fC", pv[kOutDstPitch]) : "-", -1.f);
        dst("CHIFF", note ? fmt("%.0f%%", pv[kOutDstChiff] * 100) : "-", note ? pv[kOutDstChiff] / 2.f : 0.f);
        dst("SWELL BOX", fmt("%.0f%%", pv[kOutSwellEff]), pv[kOutSwellEff] / 100.f);
        dst("TREMULANT", pv[kOutTremEff] < 0 ? std::string("OFF") : fmt("%.0f%%", pv[kOutTremEff]), std::max(0.f, pv[kOutTremEff]) / 100.f);
        y += 10;
        titleBar(x, y, w, "BOOST CEILING", "LOUDER, FLUES SHARPEN");
        y += 34;
        miniStep(x, y, "MAX", kParamPressMax, 2.f, "%.0fMM");
        y += 36;
        button(x, y, w, 36, std::string(on(kParamStrict) ? "[X]" : "[ ]") + " STRICT PHYSICAL LIMITS", false, [this] { toggle(kParamStrict); }, 20);
        y += 44;
        titleBar(x, y, w, "BEND CEILING / FAMILY");
        y += 32;
        struct L { const char* n; int p; const char* how; };
        const L ls[4] = { { "STOPPED FLUTE", kParamBendStopped, "SLIDING STOPPER" }, { "OPEN FLUE", kParamBendOpen, "MOUTH SHADE" },
                          { "STRING", kParamBendString, "SHADE, NARROW SCALE" }, { "REED", kParamBendReed, "PRESSURE ONLY" } };
        for (int i = 0; i < 4; ++i)
        {
            text(x, y + 10, ls[i].n, 20);
            const int p = ls[i].p;
            button(x + w - 140, y - 2, 30, 24, "-", false, [this, p] { nudge(p, -5); }, 20);
            text(x + w - 70, y + 10, fmt("±%.0fC", pv[p]), 20, ink, ALIGN_CENTER | ALIGN_MIDDLE);
            button(x + w - 30, y - 2, 30, 24, "+", false, [this, p] { nudge(p, 5); }, 20);
            text(x, y + 32, ls[i].how, 18, ink2);
            segBar(x + 170, y + 26, w - 170, 10, 10, pv[p] / 400.f);
            y += 50;
        }
        if (!on(kParamStrict)) text(x, y + 6, "OFF: FULL CONTROLLER RANGE", 19);
    }

    // matrix helpers
    void commitMatrix() { setStateValue(kStateMatrix, matrix.serialize()); }
    void learnPoll()
    {
        if (!learnArmed) return;
        const int v = (int)pv[kOutLearn];
        const int counter = v / 1024;
        if (counter != learnLast)
        {
            learnLast = counter;
            const int code = v % 1024;
            const int src = code / 128;
            // pressure arrives with every key press: anything else that moves wins
            auto rank = [](int s) { return s == kSrcCC ? 4 : s == kSrcCC74 ? 3 : s == kSrcBend ? 2 : s == kSrcAftertouch ? 1 : 0; };
            if (learnBest < 0 || rank(src) > rank(learnBest / 128)) learnBest = code;
            if (learnFirst < 0) learnFirst = (int)idleCount;
        }
        if (learnFirst >= 0 && (int)idleCount - learnFirst > 30)   // about a second of listening
        {
            ModRow& r = matrix.rows[selRow];
            r.src = learnBest / 128;
            if (r.src == kSrcCC) r.cc = learnBest % 128;
            r.restMid = r.src == kSrcBend;
            r.on = true;
            learnArmed = false; learnFirst = -1;
            toast = "LEARNED " + ModMatrix::sourceLabel(r);
            commitMatrix();
            repaint();
        }
    }
    void miniStep(float x, float y, const char* label, int p, float step, const char* f)
    {
        text(x, y + 12, label, 19, ink2);
        button(x + 66, y, 26, 24, "-", false, [this, p, step] { nudge(p, -step); }, 20);
        text(x + 124, y + 12, fmt(f, pv[p]), 20, ink, ALIGN_CENTER | ALIGN_MIDDLE);
        button(x + 158, y, 26, 24, "+", false, [this, p, step] { nudge(p, step); }, 20);
    }

    // ================================================================ wind
    void drawWind()
    {
        const float top = 84;
        titleBar(20, top, 890, "WIND SYSTEM", fmt("FLEX %.0f · ", pv[kParamStability] * 100) + fmt("%.0f PIPES SPEAKING", pv[kOutPipes]));
        // diagram
        const float ox = 30, oy = top + 40;
        strokeColor(ink); strokeWidth(1.5f);
        auto box = [&](float x, float y, float w, float h) { beginPath(); rect(x, y, w, h); strokeColor(ink); strokeWidth(1.5f); stroke(); };
        auto line = [&](float x1, float y1, float x2, float y2) { beginPath(); moveTo(x1, y1); lineTo(x2, y2); strokeColor(ink); strokeWidth(1.5f); stroke(); };
        box(ox, oy + 120, 120, 80);
        beginPath(); circle(ox + 60, oy + 155, 22); stroke();
        // the fan: spokes turn with its speed
        {
            const float sp = pv[kOutFan];
            for (int i = 0; i < 4; ++i)
            {
                const float a = (float)i * (float)M_PI / 2.f + (float)fanAngle;
                beginPath(); moveTo(ox + 60, oy + 155); lineTo(ox + 60 + std::cos(a) * 18, oy + 155 + std::sin(a) * 18); strokeColor(ink); strokeWidth(1.5f); stroke();
            }
            fanAngle += 0.35 * sp;
            text(ox + 60, oy + 215, on(kParamBlowerOn) ? (sp > 0.99f ? std::string("BLOWER") : fmt("RUN-UP %.0f%%", sp * 100))
                                                       : (sp > 0.01f ? fmt("COASTING %.0f%%", sp * 100) : std::string("BLOWER OFF")), 21, ink, ALIGN_CENTER | ALIGN_MIDDLE);
        }
        line(ox + 120, oy + 160, ox + 170, oy + 160);
        box(ox + 170, oy + 115, 190, 90);
        beginPath(); moveTo(ox + 170, oy + 115);
        for (int i = 1; i <= 12; ++i) lineTo(ox + 170 + i * 190.f / 12, oy + (i % 2 ? 100.f : 115.f));
        stroke();
        if ((int)pv[kParamResLoad] == kLoadWeights) { fill(ox + 215, oy + 75, 40, 22, ink); fill(ox + 275, oy + 75, 40, 22, ink); }
        else for (int sx : { 225, 295 })      // springs pulling the lid down
        {
            beginPath(); moveTo(ox + sx, oy + 60);
            for (int i = 1; i <= 8; ++i) lineTo(ox + sx + (i % 2 ? 8.f : -8.f), oy + 60 + i * 5.f);
            strokeColor(ink); strokeWidth(2); stroke();
        }
        text(ox + 265, oy + 132, "MAIN RESERVOIR", 21, ink, ALIGN_CENTER | ALIGN_MIDDLE);
        text(ox + 265, oy + 162, fmt("%.0f MM", pv[kOutReservoir]), 30, ink, ALIGN_CENTER | ALIGN_MIDDLE);
        // lid height: how much wind is stored
        segBar(ox + 182, oy + 186, 166, 12, 12, pv[kOutLid] / WindSystem::kWorkHeight);
        {
            const float left = pv[kOutWindLeft];
            std::string s = "WIND STORED";
            if (left >= 0.f) s = left >= 120.f ? fmt("LASTS ABOUT %.0f MIN", left / 60.f) : fmt("LASTS ABOUT %.0f S", left);
            text(ox + 265, oy + 228, s, 20, ink, ALIGN_CENTER | ALIGN_MIDDLE);
        }
        line(ox + 360, oy + 160, ox + 400, oy + 160);
        line(ox + 400, oy + 55, ox + 400, oy + 265);
        static const int divs[4] = { kGreat, kPositive, kSwell, kPedal };
        const int chestOut[4] = { kOutChestGt, kOutChestPos, kOutChestSw, kOutChestPd };
        for (int i = 0; i < 4; ++i)
        {
            const float cy = oy + 18 + i * 78;
            line(ox + 400, cy + 20, ox + 440, cy + 20);
            box(ox + 440, cy, 100, 40);
            const bool trem = on(kNumStops - 1) && (int)pv[kParamTremTarget] == divs[i];
            text(ox + 490, cy + 21, trem ? "REG+TREM" : "REG", 21, ink, ALIGN_CENTER | ALIGN_MIDDLE);
            line(ox + 540, cy + 20, ox + 580, cy + 20);
            box(ox + 580, cy - 6, 270, 58);
            text(ox + 592, cy + 12, std::string(divisionName(divs[i])), 20);
            text(ox + 838, cy + 12, fmt("%.0f MM", pv[chestOut[i]]), 20, ink, ALIGN_RIGHT | ALIGN_MIDDLE);
            segBar(ox + 592, cy + 28, 246, 12, 16, pv[chestOut[i]] / (kChestNominal[divs[i]] * 1.1f));
        }
        // sag graph
        const float gy = oy + 330, gh = 100, gx = 30, gw = 870;
        hline(20, gy - 14, 890, true);
        text(gx, gy + 4, "I HAUPTWERK CHEST PRESSURE", 21);
        button(gx + 250, gy - 8, 80, 26, "4 S", !histLong, [this] { histLong = false; }, 19);
        button(gx + 336, gy - 8, 80, 26, "2 MIN", histLong, [this] { histLong = true; }, 19);
        const float* hist = histLong ? windHistLong : windHist;
        const int pos = histLong ? windPosLong : windPos;
        float mn = 1e9f, mx = -1e9f;
        for (int i = 0; i < kHist; ++i) { mn = std::min(mn, hist[i]); mx = std::max(mx, hist[i]); }
        text(gx + gw, gy + 4, fmt("NOMINAL 78 · MIN %.0f", mn) + fmt(" · MAX %.0f", mx), 20, ink2, ALIGN_RIGHT | ALIGN_MIDDLE);
        const float base = gy + 24 + gh;
        vline(gx, gy + 24, gh); hline(gx, base, gw);
        const float bw = gw / kHist;
        // short view: the top of the range, where sag and bounce show; long view: all the way to empty
        const float lo = histLong ? 0.f : 50.f, span = histLong ? 90.f : 40.f;
        for (int i = 0; i < kHist; ++i)
        {
            const float v = hist[(pos + i) % kHist];
            const float h = std::max(0.f, std::min(gh, (v - lo) / span * gh));
            fill(gx + 3 + i * bw, base - h, std::max(1.f, bw - 2), h, ink);
        }
        text(gx, base + 18, histLong ? "-2 MIN" : "-4.0 S", 20, ink2); text(gx + gw, base + 18, "NOW", 20, ink2, ALIGN_RIGHT | ALIGN_MIDDLE);

        // reservoir and blower settings
        const float ry = base + 40;
        titleBar(20, ry, 890, "RESERVOIR AND BLOWER", "WHAT HAPPENS WHEN THE BLOWER STOPS");
        text(30, ry + 44, "LID LOADED BY", 20, ink2);
        button(30, ry + 58, 150, 40, "WEIGHTS", (int)pv[kParamResLoad] == kLoadWeights, [this] { setParam(kParamResLoad, (float)kLoadWeights); }, 20);
        button(190, ry + 58, 150, 40, "SPRINGS", (int)pv[kParamResLoad] == kLoadSprings, [this] { setParam(kParamResLoad, (float)kLoadSprings); }, 20);
        text(30, ry + 116, (int)pv[kParamResLoad] == kLoadWeights ? "STEADY, THEN FAILS SUDDENLY" : "SINKS AS THE LID COMES DOWN", 19, ink2);
        knob(450, ry + 70, kParamResSize, "SIZE (FULL ORGAN)", fmt("%.0f S", pv[kParamResSize]));
        knob(620, ry + 70, kParamSpinDown, "BLOWER COAST-DOWN", fmt("%.0f S", pv[kParamSpinDown]));
        knob(790, ry + 70, kParamWindLeak, "LEAKAGE", fmt("%.1f%%", pv[kParamWindLeak]));
        vline(924, 76, kH - 90);

        // controls
        const float x = 940, w = 320;
        titleBar(x, top, w, "WIND CONTROLS");
        button(x, top + 34, w, 40, on(kParamBlowerOn) ? "BLOWER ON" : "BLOWER OFF", on(kParamBlowerOn), [this] { toggle(kParamBlowerOn); });
        const float ky = top + 130;
        knob(x + 53, ky, kParamStability, "FLEXIBLE", fmt("%.0f%%", pv[kParamStability] * 100));
        knob(x + 160, ky, kParamReservoir, "RESERVOIR", fmt("%.0f MM", pv[kParamReservoir]));
        knob(x + 267, ky, kParamBlowerNoise, "NOISE", fmt("%.0f DB", pv[kParamBlowerNoise]));
        knob(x + 53, ky + 130, kParamTremSpeed, "TREM SPEED", fmt("%.1f HZ", pv[kParamTremSpeed]));
        knob(x + 160, ky + 130, kParamTremDepth, "TREM DEPTH", fmt("%.0f%%", pv[kParamTremDepth] * 100));
        knob(x + 267, ky + 130, kParamConcussion, "CONCUSSION", fmt("%.0f%%", pv[kParamConcussion] * 100));
        titleBar(x, ky + 230, w, "TREMULANT ON", on(kNumStops - 1) ? "ON" : "OFF");
        addHit(x, ky + 230, w, 26, [this] { toggle(kNumStops - 1); });
        static const int td[4] = { kGreat, kPositive, kSwell, kPedal };
        for (int i = 0; i < 4; ++i)
            button(x + i * 81, ky + 262, 77, 40, divisionShort(td[i]), (int)pv[kParamTremTarget] == td[i], [this, i] { setParam(kParamTremTarget, (float)td[i]); });
        for (int i = 0; i < kNumTremKinds; ++i)
            button(x + i * 162, ky + 308, 158, 40, kTremKindLabels[i], (int)pv[kParamTremKind] == i, [this, i] { setParam(kParamTremKind, (float)i); });
        const float sy = ky + 362;
        frame(x, sy, w, 110);
        text(x + 10, sy + 18, fmt("BLOWER CAPACITY %.0f UNITS", kBlowerCapacity), 20);
        text(x + 10, sy + 42, fmt("DEMAND NOW %.0f%%", pv[kOutDemand] * 100), 20);
        text(x + 10, sy + 66, "UNIT = PRINCIPAL 8' C4 FLOW", 20, ink2);
        const bool over = pv[kOutDemand] > 1.f;
        fill(x + 8, sy + 80, w - 16, 24, ink);
        text(x + 16, sy + 92, over ? "OVERLOAD: WIND IS ROBBED" : "SUPPLY OK", 20, lcd);
    }

    // ============================================================= voicing
    void drawVoicing()
    {
        const float top = 84;
        // rank list, one division at a time (78 ranks do not fit one column)
        static const int tabDiv[4] = { kPedal, kGreat, kPositive, kSwell };
        for (int t = 0; t < 4; ++t) {
            const int dv = tabDiv[t];
            button(20 + t * 56, top, 52, 26, divisionShort(dv), kRanks[selRank].division == dv, [this, dv] {
                for (int r = 0; r < kNumRanks; ++r) if (kRanks[r].division == dv) { selRank = r; break; }
            }, 19);
        }
        float y = top + 32;
        for (int r = 0; r < kNumRanks; ++r)
        {
            if (kRanks[r].division != kRanks[selRank].division) continue;
            const bool sel = r == selRank;
            if (sel) fill(20, y, 220, 21, ink);
            std::string nm = kRanks[r].name; if (nm.size() > 24) nm = nm.substr(0, 24);
            text(26, y + 11, nm, 19, sel ? lcd : ink);
            addHit(20, y, 220, 21, [this, r] { selRank = r; });
            y += 21;
        }
        vline(250, 76, kH - 90);

        // per-key graphs
        const float x = 266, w = 640;
        const int d = kRanks[selRank].division;
        const int lo = d == kPedal ? kPedalLow : kManualLow, hi = d == kPedal ? kPedalHigh : kManualHigh;
        selKey = std::max(lo, std::min(hi, selKey));
        // voicing is set per place in the compass (a 12-TET pitch slot), whatever the keys are mapped to
        const double f = 440.0 * std::pow(2.0, (selKey - 69) / 12.0) * rankRatio(selRank, selKey);
        titleBar(x, top, w, std::string(kRanks[selRank].name) + "  " + noteName12(lo) + "-" + noteName12(hi), std::string("KEY ") + noteName12(selKey) + fmt("  %.1f HZ", f));
        text(x, top + 44, "REGULATION PER KEY (CLICK TO SELECT)", 20);
        text(x + w, top + 44, "DB", 20, ink2, ALIGN_RIGHT | ALIGN_MIDDLE);
        const float gy = top + 60, gh = 170;
        vline(x, gy, gh); hline(x, gy + gh, w);
        const int nk = hi - lo + 1;
        const float bw = (w - 6) / nk;
        for (int k = lo; k <= hi; ++k)
        {
            const float db = voicing->rank[selRank].key[kVRegulation][k];
            const float h = std::max(2.f, std::min(gh - 4, (db + 24.f) / 30.f * gh));
            const float bx = x + 4 + (k - lo) * bw;
            fill(bx, gy + gh - h, std::max(1.f, bw - 2), h, k == selKey ? ink : ink2);
            addHit(bx, gy, bw, gh, [this, k] { selKey = k; });
        }
        for (int k = lo; k <= hi; k += 12) text(x + 4 + (k - lo) * bw, gy + gh + 14, noteName12(k), 19, ink2);

        // scale graph: diameter across the compass vs normal scale
        const float sy = gy + gh + 40, sh = 210;
        hline(x, sy - 8, w, true);
        text(x, sy + 8, "SCALE (DIAMETER) ACROSS COMPASS", 20);
        text(x + w, sy + 8, "- - NORMAL   ── THIS RANK", 20, ink2, ALIGN_RIGHT | ALIGN_MIDDLE);
        const float px = x, py = sy + 24, ph = sh - 30;
        vline(px, py, ph); hline(px, py + ph, w);
        auto dia = [&](int k, double scaleHT, double halving) {
            const KeyPitch q = tuning.keyPitch(k, (int)pv[kParamMapLo], false);
            const double semis = 12.0 * std::log2(q.hz * rankRatio(selRank, k) / 65.406);
            return 155.0 * std::pow(2.0, -(semis - scaleHT) / halving);
        };
        double dmax = 1, dmin = 1e9;
        for (int k = lo; k <= hi; ++k) { dmax = std::max(dmax, std::max(dia(k, 0, 16), dia(k, voicing->rank[selRank].scaleHT, voicing->rank[selRank].halving))); dmin = std::min(dmin, std::min(dia(k, 0, 16), dia(k, voicing->rank[selRank].scaleHT, voicing->rank[selRank].halving))); }
        auto yOf = [&](double dd) { return (float)(py + ph - 4 - (std::log(dd) - std::log(dmin)) / std::max(1e-6, std::log(dmax) - std::log(dmin)) * (ph - 10)); };
        for (int pass = 0; pass < 2; ++pass)
        {
            beginPath();
            for (int k = lo; k <= hi; ++k)
            {
                const double dd = pass == 0 ? dia(k, 0, 16) : dia(k, voicing->rank[selRank].scaleHT, voicing->rank[selRank].halving);
                const float xx = px + 4 + (k - lo) * bw + bw / 2;
                if (pass == 0 && (k % 2)) { moveTo(xx, yOf(dd)); continue; }
                if (k == lo) moveTo(xx, yOf(dd)); else lineTo(xx, yOf(dd));
            }
            strokeColor(ink); strokeWidth(pass == 0 ? 1.f : 1.5f); stroke();
        }
        const double dsel = dia(selKey, voicing->rank[selRank].scaleHT, voicing->rank[selRank].halving);
        fill(px + 4 + (selKey - lo) * bw + bw / 2 - 6, yOf(dsel) - 6, 12, 12, ink);
        text(px + 10 + (selKey - lo) * bw + bw / 2, yOf(dsel) - 14, fmt("%.0f MM", dsel), 20);
        text(px + 8, py + 12, fmt("%.0f MM", dmax), 19, ink2);
        text(px + w - 4, py + ph - 14, fmt("%.0f MM", dmin), 19, ink2, ALIGN_RIGHT | ALIGN_MIDDLE);
        vline(918, 76, kH - 90);

        // parameters
        const float rx = 932, rw = 330;
        titleBar(rx, top, rw, "PIPE PARAMETERS");
        y = top + 32;
        text(rx, y + 20, "APPLY TO", 20, ink2);
        static const char* sc[3] = { "NOTE", "OCTAVE", "RANK" };
        for (int i = 0; i < 3; ++i) button(rx + 98 + i * 78, y, 74, 40, sc[i], applyScope == i, [this, i] { applyScope = i; }, 20);
        y += 48;
        RankVoicing& rv = voicing->rank[selRank];
        const bool reed = kRanks[selRank].kind == kKindReed;
        auto vStep = [&](const char* label, int field, float step, const char* f, float lo2, float hi2) {
            const float val = rv.key[field][selKey];
            stepper(rx, y, rw, label, fmt(f, val), [this, field, step, lo2, hi2] { editVoicing(field, -step, lo2, hi2); },
                    [this, field, step, lo2, hi2] { editVoicing(field, step, lo2, hi2); }, 100);
            y += 46;
        };
        stepper(rx, y, rw, "SCALE", fmt("%+.0f HT", rv.scaleHT), [this] { editRank(0, -1); }, [this] { editRank(0, 1); }, 100); y += 46;
        stepper(rx, y, rw, "HALVING", fmt("%.0fTH", rv.halving), [this] { editRank(1, -1); }, [this] { editRank(1, 1); }, 100); y += 46;
        vStep("REGULATION", kVRegulation, 0.5f, "%+.1f DB", -24.f, 6.f);
        vStep("TOE WIND", kVToe, 2.f, "%.0f MM", 30.f, reed ? 250.f : 160.f);
        if (!reed)
        {
            vStep("MOUTH", kVMouth, 0.01f, "%.2f CIRC", 0.15f, 0.30f);
            vStep("CUT-UP", kVCutup, 0.01f, "%.2f W", 0.12f, 0.45f);
            vStep("LANGUID", kVLanguid, 0.1f, "%+.1f", -1.f, 1.f);
            vStep("NICKING", kVNicking, 1.f, "%.0f", 0.f, 2.f);
            vStep("CHIFF", kVChiff, 0.05f, "%.2f", 0.f, 1.f);
        }
        const PipeGeometry g = pipeGeometry(*voicing, selRank, selKey, f);
        const BuildCheck bc = checkPipe(g);
        y += 4;
        frame(rx, y, rw, 128);
        fill(rx + 6, y + 6, rw - 12, 22, ink);
        text(rx + 12, y + 17, "BUILD CHECK", 20, lcd);
        float cy = y + 40;
        auto chk = [&](const std::string& k, bool ok) { text(rx + 12, cy, k, 20); text(rx + rw - 12, cy, ok ? "OK" : "NO", 20, ink, ALIGN_RIGHT | ALIGN_MIDDLE); cy += 22; };
        chk(reed ? "TONGUE + RESONATOR" : fmt("MOUTH %.0f MM <= 2/7 CIRC", g.mouthWidthMM), bc.mouthOk);
        chk(reed ? "REED SPEAKS" : fmt("JET TRANSIT %.2f MS", g.jetTransitMs), bc.cutupOk);
        chk(fmt("LENGTH %.2f M", g.lengthM) + fmt(" DIA %.0f MM", g.diameterMM), bc.sizeOk);
        chk(fmt("TOE WIND %.0f MM", g.toeMM), bc.pressureOk);
        button(rx, y + 136, rw, 36, "RESET RANK TO FACTORY", false, [this] { resetRank(); }, 20);
    }
    void editVoicing(int field, float delta, float lo, float hi)
    {
        const int d = kRanks[selRank].division;
        const int klo = d == kPedal ? kPedalLow : kManualLow, khi = d == kPedal ? kPedalHigh : kManualHigh;
        int a = selKey, b = selKey;
        if (applyScope == 1) { a = selKey - ((selKey % 12) + 12) % 12; b = a + 11; }
        if (applyScope == 2) { a = 0; b = kNumKeys - 1; }
        (void)klo; (void)khi;
        for (int k = std::max(0, a); k <= std::min(kNumKeys - 1, b); ++k)
            voicing->rank[selRank].key[field][k] = std::max(lo, std::min(hi, voicing->rank[selRank].key[field][k] + delta));
        setStateValue(kStateVoicing, voicing->serialize());
    }
    void editRank(int what, float delta)
    {
        RankVoicing& rv = voicing->rank[selRank];
        if (what == 0) rv.scaleHT = std::max(-12.f, std::min(12.f, rv.scaleHT + delta));
        else rv.halving = std::max(12.f, std::min(20.f, rv.halving + delta));
        setStateValue(kStateVoicing, voicing->serialize());
    }
    void resetRank()
    {
        Voicing def; def.setDefaults();
        voicing->rank[selRank] = def.rank[selRank];
        setStateValue(kStateVoicing, voicing->serialize());
    }

    // ============================================================== tuning
    void drawTuning()
    {
        const float top = 84;
        // systems
        float y = top;
        static const int groups[3][2] = { { 0, 5 }, { 5, 12 }, { 12, 13 } };
        static const char* gnames[3] = { "HISTORICAL", "EQUAL DIVISIONS", "SCALA FILE" };
        for (int g = 0; g < 3; ++g)
        {
            text(26, y + 12, gnames[g], 20, ink2);
            y += 24;
            for (int s = groups[g][0]; s < groups[g][1]; ++s)
            {
                const bool sel = (int)pv[kParamTuning] == s;
                if (sel) fill(20, y, 230, 28, ink);
                std::string label = kTuningLabels[s];
                if (s == kTunCustomEdo) label = fmt("CUSTOM %.0f-EDO", pv[kParamEdoN]);
                if (s == kTunScala) label = st[kStateScalaName].empty() ? "NO FILE LOADED" : st[kStateScalaName].substr(0, 20);
                text(26, y + 14, label, 21, sel ? lcd : ink);
                addHit(20, y, 230, 28, [this, s] { setParam(kParamTuning, (float)s); });
                y += 29;
            }
        }
        y += 6;
        button(20, y, 112, 36, "LOAD .SCL", false, [this] { openFile(kFileScl, "Load Scala scale (.scl)"); }, 20);
        button(138, y, 112, 36, "LOAD .KBM", false, [this] { openFile(kFileKbm, "Load Scala keyboard map (.kbm)"); }, 20);
        y += 44;
        stepper(20, y, 230, "EDO", fmt("%.0f", pv[kParamEdoN]), [this] { nudge(kParamEdoN, -1); }, [this] { nudge(kParamEdoN, 1); }, 50);
        vline(262, 76, kH - 90);

        // detail
        const float x = 278, w = 640;
        const double step = tuning.stepCents();
        titleBar(x, top, w, std::string(kTuningLabels[(int)pv[kParamTuning]]) + (tuning.isEdo() ? fmt("  STEP %.2f C", step) : std::string()),
                 fmt("A4 = %.1f HZ", pv[kParamARef]));
        // intervals of the tuning itself: for an EDO the step nearest each just interval
        // (whatever the keys are mapped to), otherwise the notes C-G, C-E, C-Eb
        const KeyPitch c4 = tuning.keyPitch(60, kMapSubset, false);
        struct Iv { const char* name; int key; double just; };
        const Iv ivs[3] = { { "FIFTH C-G", 67, 701.955 }, { "MAJOR THIRD C-E", 64, 386.314 }, { "MINOR THIRD C-EB", 63, 315.641 } };
        for (int i = 0; i < 3; ++i)
        {
            const float bx = x + i * 216, by = top + 34;
            frame(bx, by, 208, 84);
            double c;
            if (tuning.isEdo())
            {
                const int n = tuning.edoN();
                c = 1200.0 * std::lround(n * ivs[i].just / 1200.0) / n;
            }
            else c = 1200.0 * std::log2(tuning.keyPitch(ivs[i].key, kMapSubset, false).hz / c4.hz);
            text(bx + 8, by + 16, ivs[i].name, 20, ink2);
            text(bx + 8, by + 42, fmt("%.1f C", c), 28);
            text(bx + 8, by + 68, fmt("JUST %+.1f C", c - ivs[i].just), 20);
        }
        // A ref
        float ry = top + 130;
        stepper(x, ry, w, "A4 REFERENCE", fmt("%.1f HZ", pv[kParamARef]), [this] { nudge(kParamARef, -0.5f); }, [this] { nudge(kParamARef, 0.5f); }, 120);
        ry += 52;
        // ruler
        text(x, ry + 10, "ONE OCTAVE · 12-TET ABOVE, THIS TUNING BELOW", 20);
        const float rx0 = x + 10, rw = w - 20, ryl = ry + 60;
        hline(rx0, ryl, rw);
        beginPath();
        for (int i = 0; i <= 12; ++i) { const float xx = rx0 + rw * i / 12.f; moveTo(xx, ryl - 30); lineTo(xx, ryl); }
        strokeColor(ink); strokeWidth(1.5f); stroke();
        beginPath();
        int nsteps = 0;
        if (tuning.isEdo()) { nsteps = tuning.edoN(); for (int i = 0; i <= nsteps; ++i) { const float xx = rx0 + rw * i / (float)nsteps; moveTo(xx, ryl); lineTo(xx, ryl + 28); } }
        else if (tuning.system == kTunScala && tuning.scl.valid())
        {
            const double per = tuning.scl.cents.back();
            moveTo(rx0, ryl); lineTo(rx0, ryl + 28);
            for (double c : tuning.scl.cents) { const float xx = rx0 + rw * (float)(c / per); moveTo(xx, ryl); lineTo(xx, ryl + 28); }
        }
        else for (int k = 60; k <= 72; ++k) { const double c = 1200.0 * std::log2(tuning.keyPitch(k, kMapSubset, false).hz / c4.hz); const float xx = rx0 + rw * (float)(c / 1200.0); moveTo(xx, ryl); lineTo(xx, ryl + 28); }
        strokeWidth(2); stroke();
        ry = ryl + 48;
        // key mapping
        hline(x, ry, w, true);
        text(x, ry + 18, "KEY MAPPING", 21);
        const bool subsetOk = Tuning::subsetUsable(tuning.edoN());
        text(x + w, ry + 18, tuning.isEdo() && !subsetOk ? fmt("%.0f-EDO HAS NO 12-NOTE SUBSET", (double)tuning.edoN()) : std::string("SEQUENTIAL REACHES EVERY STEP"),
             19, ink2, ALIGN_RIGHT | ALIGN_MIDDLE);
        ry += 36;
        const int maps[3] = { kParamMapLo, kParamMapHi, kParamMapPed };
        static const char* mapNames[3] = { "LOWER ZONE", "UPPER ZONE", "PEDAL SPLIT" };
        for (int m = 0; m < 3; ++m)
        {
            text(x, ry + 20, mapNames[m], 21);
            for (int i = 0; i < 3; ++i)
            {
                const int p = maps[m];
                // what the zone really plays: an EDO without a 12-note subset is always sequential
                const int eff = tuning.effectiveMap((int)pv[p]);
                if (i == kMapSubset && tuning.isEdo() && !subsetOk)
                {
                    frame(x + 160 + i * 150, ry, 144, 40);
                    text(x + 160 + i * 150 + 72, ry + 21, "SUBSET N/A", 20, ink2, ALIGN_CENTER | ALIGN_MIDDLE);
                    continue;
                }
                button(x + 160 + i * 150, ry, 144, 40, kMapLabels[i], eff == i || (!tuning.isEdo() && (int)pv[p] == i), [this, p, i] { setParam(p, (float)i); }, 20);
            }
            ry += 46;
        }
        {
            // which key plays middle C, and which keys then cover the organ's compass
            const int a = (int)pv[kParamAnchorKey];
            stepper(x, ry, 330, "C4 ON KEY", fmt("%.0f  ", (double)a) + noteName12(a),
                    [this] { nudge(kParamAnchorKey, -1); }, [this] { nudge(kParamAnchorKey, 1); }, 120);
            int lo = -1, hi = -1;
            for (int k = 0; k < 128; ++k)
            {
                const KeyPitch q = tuning.keyPitch(k, (int)pv[kParamMapLo], false);
                if (q.valid && q.keyFloat >= kManualLow - 0.5f && q.keyFloat <= kManualHigh + 0.5f) { if (lo < 0) lo = k; hi = k; }
            }
            text(x + w, ry + 12, "MANUAL COMPASS C2-A6", 19, ink2, ALIGN_RIGHT | ALIGN_MIDDLE);
            // how many keys the whole compass needs; more than MIDI has means some of it is always out of reach
            const int need = tuning.pipesInRange(440.0 * std::pow(2.0, (kManualLow - 69) / 12.0), 440.0 * std::pow(2.0, (kManualHigh - 69) / 12.0));
            std::string reach;
            if (lo < 0) reach = "OUT OF REACH";
            else if (hi - lo + 1 < need) reach = fmt("NEEDS %.0f KEYS; ", (double)need) + fmt("KEYS %.0f-", (double)lo) + fmt("%.0f REACH PART", (double)hi);
            else reach = fmt("ON KEYS %.0f-", (double)lo) + fmt("%.0f", (double)hi) + fmt(" (%.0f KEYS)", (double)need);
            text(x + w, ry + 32, reach, 20, ink, ALIGN_RIGHT | ALIGN_MIDDLE);
            ry += 46;
        }
        button(x, ry + 4, w, 36, std::string(on(kParamEnharmonic) ? "[X]" : "[ ]") + " FLATS INSTEAD OF SHARPS (SUBSET; HOLD CC66 TO SWAP LIVE)", false, [this] { toggle(kParamEnharmonic); }, 20);
        text(x, ry + 60, "SEQUENTIAL: EACH KEY IS THE NEXT STEP; THE OCTAVE IS N KEYS WIDE.", 19, ink2);
        text(x, ry + 80, "SUBSET: 12 KEYS PLAY 12 STEPS ALONG THE FIFTHS; OCTAVE STAYS 12 KEYS.", 19, ink2);
        vline(930, 76, kH - 90);

        // per-pipe tuning
        const float px = 944, pw = 316;
        titleBar(px, top, pw, "PER-PIPE TUNING");
        text(px, top + 44, std::string(divisionShort(kRanks[selRank].division)) + " " + kRanks[selRank].name, 21);
        button(px + pw - 76, top + 30, 36, 30, "<", false, [this] { selRank = (selRank + kNumRanks - 1) % kNumRanks; }, 20);
        button(px + pw - 36, top + 30, 36, 30, ">", false, [this] { selRank = (selRank + 1) % kNumRanks; }, 20);
        float ty = top + 70;
        text(px + 4, ty, "KEY", 19, ink2); text(px + 70, ty, "NAME", 19, ink2); text(px + 140, ty, "HZ", 19, ink2); text(px + pw - 4, ty, "OFFS", 19, ink2, ALIGN_RIGHT | ALIGN_MIDDLE);
        ty += 14;
        for (int k = selKey - 3; k <= selKey + 3; ++k)
        {
            if (k < 0 || k > 127) { ty += 25; continue; }
            const KeyPitch q = tuning.keyPitch(k, (int)pv[kParamMapLo], on(kParamEnharmonic));
            const bool sel = k == selKey;
            if (sel) fill(px, ty, pw, 24, ink);
            const Color c = sel ? lcd : ink;
            const float off = voicing->pipeTune[selRank][q.pipeIndex];
            text(px + 4, ty + 12, std::to_string(k), 20, c);
            text(px + 70, ty + 12, q.name, 20, c);
            text(px + 140, ty + 12, fmt("%.2f", q.hz * rankRatio(selRank, k) * std::pow(2.0, off / 1200.0)), 20, c);
            text(px + pw - 4, ty + 12, fmt("%+.1f", off), 20, c, ALIGN_RIGHT | ALIGN_MIDDLE);
            addHit(px, ty, pw, 24, [this, k] { selKey = k; });
            ty += 25;
        }
        ty += 6;
        const KeyPitch qs = tuning.keyPitch(selKey, (int)pv[kParamMapLo], on(kParamEnharmonic));
        const int pidx = qs.pipeIndex;
        stepper(px, ty, pw, "OFFSET", fmt("%+.1f C", voicing->pipeTune[selRank][pidx]),
                [this, pidx] { tunePipe(pidx, -0.5f); }, [this, pidx] { tunePipe(pidx, 0.5f); }, 90);
        ty += 48;
        button(px, ty, pw, 36, "RESET THIS RANK'S OFFSETS", false, [this] {
            for (int p = 0; p < kNumPipeSlots; ++p) voicing->pipeTune[selRank][p] = 0.f;
            setStateValue(kStatePipeTune, voicing->serializeTuning()); }, 20);
        ty += 44;
        paramStepper(px, ty, pw, "PIPE TEMP", kParamPipeTemp, 1.f, "%.0f C", 1.f, 90); ty += 46;
        paramStepper(px, ty, pw, "AGE DRIFT", kParamAgeDrift, 0.5f, "±%.1f C", 1.f, 90); ty += 50;
        // build check
        // the compass is fixed in pitch (C2-A6), whatever the keys are mapped to
        const double flo = 440.0 * std::pow(2.0, (kManualLow - 69) / 12.0), fhi = 440.0 * std::pow(2.0, (kManualHigh - 69) / 12.0);
        const int pipes = tuning.pipesInRange(flo, fhi);
        frame(px, ty, pw, 92);
        fill(px + 6, ty + 6, pw - 12, 22, ink);
        text(px + 12, ty + 17, "BUILD CHECK", 20, lcd);
        text(px + 12, ty + 42, fmt("PIPES / MANUAL RANK  %.0f", pipes) + (pipes > 58 ? fmt(" (12-TET 58)", 0) : std::string()), 20);
        text(px + 12, ty + 66, pipes > 58 ? fmt("CHEST LENGTH +%.0f%%", (pipes / 58.0 - 1) * 100) : std::string("STANDARD CHEST"), 20);
        if (tuning.edoN() == 31) text(px + 12, ty + 84, "PRECEDENT: FOKKER ORGAN, 1950", 19, ink2);
    }
    void tunePipe(int pidx, float delta)
    {
        float& v = voicing->pipeTune[selRank][pidx];
        v = std::max(-100.f, std::min(100.f, v + delta));
        setStateValue(kStatePipeTune, voicing->serializeTuning());
    }

    // ================================================================ room
    void drawRoom()
    {
        const float top = 84;
        const int room = (int)pv[kParamRoom];
        const int status = (int)pv[kOutRoomStatus];
        const int usedPos = (int)pv[kOutRoomPos];
        titleBar(20, top, 220, "BUILDING");
        float y = top + 30;
        auto entry = [&](int r, const std::string& sub) {
            const bool sel = room == r;
            if (sel) fill(20, y, 220, 44, ink);
            frame(20, y, 220, 44);
            std::string l = kRoomLabels[r];
            if (l.rfind("LITE ", 0) == 0) l = l.substr(5);
            text(28, y + 14, l, 20, sel ? lcd : ink);
            text(28, y + 33, sub, 18, sel ? lcd : ink2);
            addHit(20, y, 220, 44, [this, r] { setParam(kParamRoom, (float)r); });
            y += 47;
        };
        text(20, y + 9, "MEASURED (CONVOLUTION)", 19, ink2); y += 20;
        for (int r = kRoomStAndrews; r <= kRoomJackLyons; ++r)
        {
            const irmeta::Position* p = firstPosition(r);
            entry(r, p ? fmt("T30 %.1f S", 0.5f * (p->t30Low + p->t30High)) + fmt(" · %.0f POS", positionCount(r)) : std::string("NOT IN THIS BUILD"));
        }
        entry(kRoomCustom, st[kStateCustomIr].empty() ? std::string("NO FILE LOADED") : upper(fs::path(st[kStateCustomIr]).stem().string()).substr(0, 22));
        text(20, y + 9, "LITE (ALGORITHMIC)", 19, ink2); y += 20;
        for (int r = kRoomLiteVillage; r <= kRoomDry; ++r)
        {
            const RoomShape& rs = kRoomShapes[roomLiteIndex(r)];
            entry(r, rs.rt60 > 0 ? fmt("RT60 %.1f S", rs.rt60) : std::string("NO ROOM"));
        }
        titleBar(20, y + 4, 220, "QUALITY");
        for (int q = 0; q < 3; ++q) button(20 + q * 74, y + 34, 70, 36, kQualityLabels[q], (int)pv[kParamQuality] == q, [this, q] { setParam(kParamQuality, (float)q); }, 20);
        text(20, y + 84, "ECO ALSO DOUBLES IR BLOCK", 18, ink2);
        vline(252, 76, kH - 90);

        // ------------------------------------------------------------ plan
        const float x = 266, w = 640;
        const bool conv = roomIsConvolution(room);
        std::string right;
        if (conv)
        {
            switch (status)
            {
            case 1: right = "PREPARING IR..."; break;
            case 2: right = "NO DATA: USING LITE ABBEY"; break;
            case 3: right = "CAN'T READ THAT FILE"; break;
            case 4: right = "LOAD A FILE BELOW"; break;
            default: right = "CONVOLUTION"; break;
            }
        }
        else right = "ALGORITHMIC";
        titleBar(x, top, w, kRoomLabels[room], right);
        const float px = x + 10, py = top + 60, pw = w - 60, ph = 170;
        const irmeta::Position* pos = conv && roomIsMeasured(room) ? positionFor(room, usedPos) : nullptr;
        const float roomLen = !conv ? kRoomShapes[roomLiteIndex(room)].lengthM
                                    : std::max(12.f, 1.5f * maxDistance(room));
        beginPath(); rect(px, py, pw, ph); strokeColor(ink); strokeWidth(1.5f); stroke();
        beginPath(); rect(px + 14, py + ph / 2 - 45, 40, 90); stroke();
        for (int i = 1; i < 6; ++i) { beginPath(); moveTo(px + 14, py + ph / 2 - 45 + i * 15.f); lineTo(px + 54, py + ph / 2 - 45 + i * 15.f); strokeWidth(2); stroke(); }
        text(px + 4, py - 12, conv ? "SOURCE · ORGAN" : "WEST GALLERY · ORGAN CASE", 20);
        text(px + pw, py - 12, conv && roomIsMeasured(room) ? "PLAN NOT TO SCALE" : (conv ? "" : fmt("%.0f", kRoomShapes[roomLiteIndex(room)].lengthM) + fmt(" X %.0f M", kRoomShapes[roomLiteIndex(room)].widthM)), 20, ink2, ALIGN_RIGHT | ALIGN_MIDDLE);
        // measured positions as ghost dots
        if (conv && roomIsMeasured(room))
            for (int i = 0; i < irmeta::kCount; ++i)
            {
                const irmeta::Position& q = irmeta::kPositions[i];
                if (q.room != room) continue;
                const float qx = px + 54 + (pw - 70) * std::min(1.f, q.distance / roomLen);
                const float qy = py + ph / 2 + (q.index % 2 ? -1.f : 1.f) * (q.index == 0 ? 0.f : 22.f + 10.f * (q.index / 2));
                beginPath(); circle(qx, qy, 6); strokeColor(ink2); strokeWidth(2); stroke();
            }
        const float listenD = pos ? pos->distance : pv[kParamDistance];
        const float lx = px + 54 + (pw - 70) * std::min(1.f, listenD / std::max(10.f, roomLen));
        fill(lx - 5, py + ph / 2 - 18, 10, 10, ink);
        fill(lx - 5, py + ph / 2 + 8, 10, 10, ink);
        beginPath(); moveTo(px + 54, py + ph / 2); lineTo(lx - 6, py + ph / 2 - 13); moveTo(px + 54, py + ph / 2); lineTo(lx - 6, py + ph / 2 + 13); strokeColor(ink); strokeWidth(2); stroke();
        text(std::min(lx + 12, px + pw - 200), py + ph / 2 + 34, std::string(kMicLabels[(int)pv[kParamMicType]]) + fmt("  %.1f M", listenD), 21);
        std::string under;
        if (pos) under = std::string("MEASURED AT ") + pos->name + fmt(", %.1f M", pos->distance) + " · DISTANCE PICKS NEAREST";
        else if (room == kRoomCustom) under = "CUSTOM: DISTANCE SETS THE MODEL'S DIRECT SOUND";
        else under = "PIPES PLACED ON C / C# SIDES OF THE CASE";
        text(px + 4, py + ph + 20, under, 19, ink2);

        // decay
        const float dy = py + ph + 42;
        hline(x, dy, w, true);
        text(x, dy + 18, "DECAY", 21);
        text(x + w, dy + 18, "── 125-500 HZ   - - 1-4 KHZ", 20, ink2, ALIGN_RIGHT | ALIGN_MIDDLE);
        const float gx = x, gy = dy + 36, gw = w, gh = 130;
        vline(gx, gy, gh); hline(gx, gy + gh, gw);
        float rtLo = 0.f, rtHi = 0.f;
        if (!conv) { const RoomShape& rs = kRoomShapes[roomLiteIndex(room)]; rtLo = rs.rt60; rtHi = rs.rt60 * rs.hfRatio; }
        else if (pos) { rtLo = pos->t30Low; rtHi = pos->t30High; }
        if (rtLo > 0.f)
        {
            const float tmax = std::max(2.f, std::max(rtLo, rtHi) * 1.3f);
            for (int pass = 0; pass < 2; ++pass)
            {
                const float rt = pass == 0 ? rtLo : rtHi;
                beginPath();
                for (int i = 0; i <= 60; ++i)
                {
                    const float t = tmax * i / 60.f;
                    const float yy = gy + gh * std::min(1.f, t / rt);
                    if (i == 0) moveTo(gx, yy); else if (pass == 1 && (i % 2)) moveTo(gx + gw * i / 60.f, yy); else lineTo(gx + gw * i / 60.f, yy);
                }
                strokeColor(ink); strokeWidth(pass == 0 ? 1.5f : 1.f); stroke();
            }
            text(gx + gw, gy + gh + 14, fmt("%.1f S", tmax), 19, ink2, ALIGN_RIGHT | ALIGN_MIDDLE);
            text(gx + 10, gy + 14, conv ? fmt("T30 %.2f S", rtLo) + fmt(" / %.2f S (MEASURED)", rtHi) : fmt("RT60 %.1f S", rtLo), 20);
        }
        else text(gx + 10, gy + gh / 2, room == kRoomCustom ? "DECAY COMES FROM YOUR FILE" : "NO REVERBERATION", 20, ink2);

        // custom impulse response + options
        float cy = gy + gh + 30;
        hline(x, cy, w, true);
        cy += 10;
        button(x, cy, 170, 38, "LOAD IR FILE...", room == kRoomCustom, [this] { openFile(kFileIr, "Load impulse response (WAV)"); }, 20);
        text(x + 184, cy + 12, st[kStateCustomIr].empty() ? std::string("NO CUSTOM FILE") : upper(fs::path(st[kStateCustomIr]).filename().string()).substr(0, 40), 20);
        text(x + 184, cy + 30, "WAV: MONO, STEREO, OR 4-CH B-FORMAT", 18, ink2);
        for (int o = 0; o < 2; ++o) button(x + w - 180 + o * 92, cy, 88, 38, kIrOrderLabels[o], (int)pv[kParamIrOrder] == o, [this, o] { setParam(kParamIrOrder, (float)o); }, 20);
        cy += 48;
        button(x, cy, 360, 34, std::string(on(kParamIrTrim) ? "[X]" : "[ ]") + " DIRECT SOUND FROM THE MODEL", false, [this] { toggle(kParamIrTrim); }, 20);
        text(x + 372, cy + 17, on(kParamIrTrim) ? "IR'S OWN DIRECT CUT" : "IR PLAYS AS RECORDED", 19, ink2);
        cy += 46;
        text(x, cy + 8, "IMPULSE RESPONSES: OPENAIR, AUDIOLAB, UNIVERSITY OF YORK (CC BY 4.0)", 18, ink2);
        vline(918, 76, kH - 90);

        // ------------------------------------------------------- listener
        const float rx = 932, rw = 330;
        titleBar(rx, top, rw, "LISTENER");
        const float ky = top + 76;
        knob(rx + 55, ky, kParamDistance, "DISTANCE", fmt("%.0f M", pv[kParamDistance]));
        knob(rx + 165, ky, kParamMicHeight, "HEIGHT", fmt("%.1f M", pv[kParamMicHeight]));
        knob(rx + 275, ky, kParamWidth, "WIDTH", fmt("%.0f%%", pv[kParamWidth] * 100));
        knob(rx + 55, ky + 128, kParamRoomMix, "ROOM MIX", fmt("%.0f%%", pv[kParamRoomMix] * 100));
        knob(rx + 165, ky + 128, kParamPreDelay, "PRE-DELAY", fmt("%.0f MS", pv[kParamPreDelay]));
        knob(rx + 275, ky + 128, kParamOutputGain, "OUTPUT", fmt("%+.0f DB", pv[kParamOutputGain]));
        titleBar(rx, ky + 226, rw, "MICS");
        for (int m = 0; m < 3; ++m) button(rx + m * 112, ky + 258, 106, 40, kMicLabels[m], (int)pv[kParamMicType] == m, [this, m] { setParam(kParamMicType, (float)m); }, 20);
        text(rx, ky + 314, (int)pv[kParamMicType] == 2 ? "BINAURAL: FIXED HEAD, FOR HEADPHONES" : (int)pv[kParamMicType] == 0 ? "AB: SPACED PAIR, WIDE AND DIFFUSE" : "ORTF: CARDIOIDS AT ±55°", 18, ink2);
        const float oy = ky + 332;
        frame(rx, oy, rw, 120);
        fill(rx + 6, oy + 6, rw - 12, 22, ink);
        text(rx + 12, oy + 17, "OUTPUT", 20, lcd);
        auto dbfs = [](float v) { return v > 1e-6f ? 20.f * std::log10(v) : -120.f; };
        text(rx + 12, oy + 44, "L", 20); segBar(rx + 30, oy + 36, rw - 42, 16, 20, (dbfs(pv[kOutPeakL]) + 60.f) / 60.f);
        text(rx + 12, oy + 66, "R", 20); segBar(rx + 30, oy + 58, rw - 42, 16, 20, (dbfs(pv[kOutPeakR]) + 60.f) / 60.f);
        text(rx + 12, oy + 94, fmt("PEAK %.1f DBFS", dbfs(std::max(pv[kOutPeakL], pv[kOutPeakR]))), 20);
        // before the limiter: CLIP when it would overload, LIMIT when the limiter is catching it
        if (pv[kOutClip] > 1.f || pv[kOutLimitDb] > 0.5f)
        {
            const bool lim = on(kParamLimiter);
            fill(rx + rw - 140, oy + 40, 132, 32, ink);
            text(rx + rw - 74, oy + 57, lim ? fmt("LIMIT -%.1f DB", std::max(0.f, pv[kOutLimitDb])) : std::string("CLIP!"), 19, lcd, ALIGN_CENTER | ALIGN_MIDDLE);
        }
        button(rx + rw - 140, oy + 80, 132, 32, on(kParamLimiter) ? "LIMITER ON" : "LIMITER OFF", on(kParamLimiter), [this] { toggle(kParamLimiter); }, 19);
    }

    // measured-room metadata (generated into IrMeta.hpp by the build)
    static const irmeta::Position* firstPosition(int room)
    {
        for (int i = 0; i < irmeta::kCount; ++i) if (irmeta::kPositions[i].room == room) return &irmeta::kPositions[i];
        return nullptr;
    }
    static const irmeta::Position* positionFor(int room, int index)
    {
        for (int i = 0; i < irmeta::kCount; ++i) if (irmeta::kPositions[i].room == room && irmeta::kPositions[i].index == index) return &irmeta::kPositions[i];
        return firstPosition(room);
    }
    static float positionCount(int room)
    {
        int n = 0;
        for (int i = 0; i < irmeta::kCount; ++i) n += irmeta::kPositions[i].room == room;
        return (float)n;
    }
    static float maxDistance(int room)
    {
        float d = 0.f;
        for (int i = 0; i < irmeta::kCount; ++i) if (irmeta::kPositions[i].room == room) d = std::max(d, irmeta::kPositions[i].distance);
        return d;
    }

    // ============================================================= presets
    struct Entry { std::string name, tag, tuning, path; int factory = -1; };
    std::vector<Entry> userPresets;
    void scanUserPresets()
    {
        userPresets.clear();
        std::error_code ec;
        const std::string dir = userPresetDir();
        if (!fs::exists(dir, ec)) return;
        for (const auto& e : fs::directory_iterator(dir, ec))
        {
            if (e.path().extension() != ".pipe") continue;
            PresetData p;
            if (!decodePreset(readFile(e.path().string()), p)) continue;
            Entry en;
            en.name = p.name.empty() ? upper(e.path().stem().string()) : p.name;
            en.tag = "USER";
            en.tuning = p.params.count(kParamTuning) ? kTuningLabels[std::max(0, std::min(kNumTuningSystems - 1, (int)p.params[kParamTuning]))] : "-";
            en.path = e.path().string();
            userPresets.push_back(en);
        }
        std::sort(userPresets.begin(), userPresets.end(), [](const Entry& a, const Entry& b) { return a.name < b.name; });
    }
    std::vector<Entry> visibleList() const
    {
        std::vector<Entry> v;
        if (bank == 0)
            for (int i = 0; i < kNumFactoryPresets; ++i)
            {
                if (tagFilter > 0 && std::strcmp(kFactory[i].tag, kTags[tagFilter]) != 0) continue;
                Entry e; e.name = kFactory[i].name; e.tag = kFactory[i].tag; e.tuning = kFactory[i].tuningLabel; e.factory = i;
                v.push_back(e);
            }
        else v = userPresets;
        return v;
    }
    static constexpr const char* kTags[5] = { "ALL", "BAROQUE", "ROMANTIC", "MICROTONAL", "EXPRESSIVE" };

    void drawPresets()
    {
        const float top = 84;
        titleBar(20, top, 200, "BANK");
        button(20, top + 32, 200, 44, fmt("FACTORY  %.0f", kNumFactoryPresets), bank == 0, [this] { bank = 0; selPreset = 0; });
        button(20, top + 82, 200, 44, fmt("USER  %.0f", (double)userPresets.size()), bank == 1, [this] { bank = 1; selPreset = 0; scanUserPresets(); });
        titleBar(20, top + 142, 200, "TAG");
        for (int t = 0; t < 5; ++t)
        {
            const bool sel = tagFilter == t;
            if (sel) fill(20, top + 174 + t * 36.f, 200, 34, ink);
            text(30, top + 191 + t * 36.f, kTags[t], 21, sel ? lcd : ink);
            addHit(20, top + 174 + t * 36.f, 200, 34, [this, t] { tagFilter = t; selPreset = 0; });
        }
        text(20, top + 370, "USER FOLDER:", 19, ink2);
        std::string dir = upper(userPresetDir());
        if (dir.size() > 22) dir = "..." + dir.substr(dir.size() - 19);
        text(20, top + 390, dir, 19, ink2);
        vline(232, 76, kH - 90);

        // list
        const float x = 246, w = 640;
        const auto list = visibleList();
        titleBar(x, top, w, bank == 0 ? fmt("FACTORY · %.0f PRESETS", (double)list.size()) : fmt("USER · %.0f PRESETS", (double)list.size()), "* = EDITED");
        text(x + 8, top + 42, "#", 20, ink2); text(x + 58, top + 42, "NAME", 20, ink2); text(x + 360, top + 42, "TUNING", 20, ink2); text(x + 520, top + 42, "TAG", 20, ink2);
        float y = top + 56;
        const int maxRows = 14;
        selPreset = std::max(0, std::min((int)list.size() - 1, selPreset));
        const int first = std::max(0, std::min(selPreset - maxRows / 2, (int)list.size() - maxRows));
        for (int i = first; i < (int)list.size() && i < first + maxRows; ++i)
        {
            const bool sel = i == selPreset;
            if (sel) fill(x, y, w, 34, ink);
            const Color c = sel ? lcd : ink;
            text(x + 8, y + 17, bank == 0 ? fmt("P%02.0f", list[(size_t)i].factory + 1) : fmt("U%02.0f", i + 1), 21, c);
            text(x + 58, y + 17, list[(size_t)i].name.substr(0, 26), 21, c);
            text(x + 360, y + 17, list[(size_t)i].tuning, 21, c);
            text(x + 520, y + 17, list[(size_t)i].tag, 21, c);
            const Entry e = list[(size_t)i];
            addHit(x, y, w, 34, [this, i, e] {
                if (selPreset == i && (std::chrono::steady_clock::now() - lastClick) < std::chrono::milliseconds(400)) loadEntry(e);
                selPreset = i; lastClick = std::chrono::steady_clock::now(); });
            beginPath(); moveTo(x, y + 34); lineTo(x + w, y + 34); strokeColor(ghost); strokeWidth(1); stroke();
            y += 36;
        }
        const float by = kH - 70;
        button(x, by, 154, 44, "LOAD", true, [this] { const auto l = visibleList(); if (!l.empty()) loadEntry(l[(size_t)selPreset]); });
        button(x + 162, by, 154, 44, "IMPORT FILE", false, [this] { openFile(kFileImport, "Import Pipe preset (.pipe)"); });
        button(x + 324, by, 154, 44, "EXPORT FILE", false, [this] { saveFile(); });
        button(x + 486, by, 154, 44, "DELETE", false, [this] {
            const auto l = visibleList();
            if (bank != 1 || l.empty()) { toast = "FACTORY PRESETS CAN'T BE DELETED"; return; }
            std::error_code ec; fs::remove(l[(size_t)selPreset].path, ec);
            toast = "DELETED " + l[(size_t)selPreset].name; scanUserPresets(); });
        vline(898, 76, kH - 90);

        // save panel
        const float sx = 912, sw = 350;
        titleBar(sx, top, sw, "SAVE CURRENT SOUND");
        text(sx, top + 44, "NAME (CLICK, THEN TYPE)", 20, ink2);
        if (nameFocus) fill(sx, top + 58, sw, 44, ink);
        frame(sx, top + 58, sw, 44);
        text(sx + 10, top + 81, saveName + (nameFocus ? "_" : ""), 24, nameFocus ? lcd : ink);
        addHit(sx, top + 58, sw, 44, [this] { nameFocus = true; });
        text(sx, top + 122, "INCLUDE", 20, ink2);
        y = top + 136;
        for (int g = 0; g < kNumGroups; ++g)
        {
            text(sx, y + 13, std::string(include[g] ? "[X] " : "[ ] ") + kGroupLabels[g], 21);
            if (g == kGroupRouting) text(sx + sw, y + 13, "SETUP", 19, ink2, ALIGN_RIGHT | ALIGN_MIDDLE);
            addHit(sx, y, sw, 28, [this, g] { include[g] = !include[g]; });
            y += 30;
        }
        y += 6;
        button(sx, y, sw / 2 - 4, 44, "SAVE AS NEW", true, [this] { saveUser(false); });
        button(sx + sw / 2 + 4, y, sw / 2 - 4, 44, "OVERWRITE", false, [this] { saveUser(true); });
        y += 60;
        titleBar(sx, y, sw, "COMBINATIONS (STOPS ONLY)");
        y += 32;
        for (int i = 0; i < 8; ++i)
        {
            const bool isSet = i == 7;
            const std::string label = isSet ? "SET" : std::to_string(i + 1);
            const bool lit = isSet ? setMode : (i == lastPiston);
            button(sx + i * 44.f, y, 40, 44, label, lit, [this, i, isSet] {
                if (isSet) { setMode = !setMode; return; }
                if (setMode) { storePiston(i); setMode = false; toast = fmt("COMBINATION %.0f STORED", i + 1); }
                else recallPiston(i);
                lastPiston = i; }, 21);
        }
        text(sx, y + 64, "SET THEN A NUMBER TO STORE.", 19, ink2);
    }

    void loadEntry(const Entry& e)
    {
        if (e.factory >= 0)
        {
            const PresetData p = factoryPreset(e.factory);
            applyPreset(p, p.groups);
            presetLabel = std::string("P") + fmt("%02.0f ", e.factory + 1) + e.name;
        }
        else
        {
            PresetData p;
            if (!decodePreset(readFile(e.path), p)) { toast = "COULD NOT READ PRESET"; return; }
            applyPreset(p, p.groups);
            presetLabel = e.name;
        }
        setStateValue(kStatePresetName, presetLabel);
        dirty = false;
        toast = "LOADED " + presetLabel;
    }

    void applyPreset(const PresetData& p, unsigned groups)
    {
        loading = true;
        for (const auto& kv : p.params)
        {
            const int g = paramGroup(kv.first);
            if (g < 0 || !(groups & (1u << g))) continue;
            editParameter((uint32_t)kv.first, true);
            setParam(kv.first, kv.second);
            editParameter((uint32_t)kv.first, false);
        }
        for (const auto& kv : p.states)
        {
            const int g = stateGroup(kv.first);
            if (g < 0 || !(groups & (1u << g))) continue;
            setStateValue(kv.first, kv.second);
            if (kv.first == kStateVoicing) voicing->parseVoicing(kv.second);
            if (kv.first == kStatePipeTune) voicing->parseTuning(kv.second);
        }
        loading = false;
        dirty = false;
    }

    PresetData currentPreset(const std::string& name) const
    {
        PresetData p;
        p.name = name;
        for (int g = 0; g < kNumGroups; ++g) if (include[g]) p.groups |= 1u << g;
        for (int i = 0; i < kOutFirst; ++i) { const int g = paramGroup(i); if (g >= 0 && include[g]) p.params[i] = pv[i]; }
        for (int s = 0; s < kNumStates; ++s) { const int g = stateGroup(s); if (g >= 0 && include[g]) p.states[s] = st[s]; }
        return p;
    }

    bool writeUserPreset(const std::string& name, const std::string& data)
    {
        std::error_code ec;
        fs::create_directories(userPresetDir(), ec);
        std::string file;
        for (char c : name) file.push_back(std::isalnum((unsigned char)c) || c == '-' || c == '_' || c == ' ' ? c : '_');
        if (file.empty()) file = "PRESET";
        std::ofstream f(fs::path(userPresetDir()) / (file + ".pipe"), std::ios::binary);
        f << data;
        return f.good();
    }

    void saveUser(bool overwrite)
    {
        std::string name = saveName.empty() ? std::string("MY ORGAN") : saveName;
        if (!overwrite)
        {
            // never clobber: add a number when the name is taken
            std::error_code ec;
            std::string base = name; int n = 2;
            while (fs::exists(fs::path(userPresetDir()) / (name + ".pipe"), ec)) name = base + " " + std::to_string(n++);
        }
        if (writeUserPreset(name, encodePreset(currentPreset(name))))
        {
            presetLabel = name;
            setStateValue(kStatePresetName, name);
            dirty = false;
            toast = "SAVED " + name;
            scanUserPresets();
        }
        else toast = "COULD NOT WRITE TO THE PRESET FOLDER";
    }

    void saveFile()
    {
        fileAction = kFileExport;
        FileBrowserOptions o;
        o.saving = true;
        o.title = "Export Pipe preset";
        static std::string def; def = (saveName.empty() ? std::string("preset") : saveName) + ".pipe";
        o.defaultName = def.c_str();
        openFileBrowser(o);
    }

    void openFile(int action, const char* title)
    {
        fileAction = action;
        FileBrowserOptions o;
        o.title = title;
        openFileBrowser(o);
    }

    void stepPreset(int dir)
    {
        bank = 0;
        int cur = -1;
        for (int i = 0; i < kNumFactoryPresets; ++i) if (presetLabel.find(kFactory[i].name) != std::string::npos) cur = i;
        const int next = ((cur < 0 ? (dir > 0 ? -1 : 0) : cur) + dir + kNumFactoryPresets) % kNumFactoryPresets;
        Entry e; e.name = kFactory[next].name; e.factory = next;
        loadEntry(e);
    }

    void storePiston(int i)
    {
        std::string bits;
        for (int s = 0; s < kNumStops; ++s) bits.push_back(on(s) ? '1' : '0');
        std::vector<std::string> lines(7);
        std::istringstream in(st[kStateCombos]); std::string l;
        while (std::getline(in, l)) { const int n = std::atoi(l.c_str()); if (n >= 1 && n <= 7 && l.size() > 2) lines[(size_t)n - 1] = l.substr(2); }
        lines[(size_t)i] = bits;
        std::string out;
        for (int k = 0; k < 7; ++k) if (!lines[(size_t)k].empty()) out += std::to_string(k + 1) + " " + lines[(size_t)k] + "\n";
        setStateValue(kStateCombos, out);
    }
    void recallPiston(int i)
    {
        std::istringstream in(st[kStateCombos]); std::string l;
        while (std::getline(in, l))
        {
            if (std::atoi(l.c_str()) != i + 1 || l.size() < 2 + (size_t)kNumStops) continue;
            for (int s = 0; s < kNumStops; ++s) { editParameter((uint32_t)s, true); setParam(s, l[2 + (size_t)s] == '1' ? 1.f : 0.f); editParameter((uint32_t)s, false); }
            toast = fmt("COMBINATION %.0f", i + 1);
            return;
        }
        toast = fmt("COMBINATION %.0f IS EMPTY: SET IT FIRST", i + 1);
    }

    // =============================================================== data
    enum { kFileNone = 0, kFileScl, kFileKbm, kFileImport, kFileExport, kFileIr };
    static constexpr int kHist = 120;

    // Three grounds for the same line drawing: drafting (dark ink on paper), blueprint, night.
    // `lcd` is the ground (the name is from the LCD design), `ghost` the unlit segments.
    Color ink = Color(31, 43, 56), ink2 = Color(77, 90, 104), lcd = Color(243, 240, 231),
          ghost = Color(206, 205, 201), frameCol = Color(217, 212, 199);
    int theme = 0;
    void setTheme(int t)
    {
        theme = t % 3;
        if (theme == 0) { ink = Color(31, 43, 56); ink2 = Color(77, 90, 104); lcd = Color(243, 240, 231); ghost = Color(206, 205, 201); frameCol = Color(217, 212, 199); }
        if (theme == 1) { ink = Color(230, 240, 247); ink2 = Color(169, 195, 216); lcd = Color(22, 58, 92); ghost = Color(60, 92, 122); frameCol = Color(13, 35, 56); }
        if (theme == 2) { ink = Color(217, 220, 214); ink2 = Color(154, 161, 156); lcd = Color(23, 26, 29); ghost = Color(58, 62, 64); frameCol = Color(11, 13, 14); }
    }
    static constexpr float kDrawerTop = 70.f, kDrawerBottom = 748.f;

    FontId font = -1, fontBold = -1;
    float pv[kNumParams];
    std::string st[kNumStates];
    std::unique_ptr<Voicing> voicing;
    Tuning tuning;
    int screen = kScrConsole;
    int selRank = 1, selKey = 60, applyScope = 0;   // I Octave 8'
    int dragHit = -1; float dragStartY = 0, dragStartV = 0;
    float windHist[kHist]; int windPos = 0; unsigned idleCount = 0;
    float windHistLong[kHist]; int windPosLong = 0; bool histLong = false; double fanAngle = 0;
    std::chrono::steady_clock::time_point histShortT{}, histLongT{};
    std::string presetLabel = "P01 PLEIN JEU GT+PED";
    bool dirty = false, loading = false;
    std::string toast;
    int bank = 0, tagFilter = 0, selPreset = 0;
    std::chrono::steady_clock::time_point lastClick;
    bool include[kNumGroups];
    std::string saveName;
    bool nameFocus = false, setMode = false;
    int lastPiston = -1;
    int fileAction = kFileNone;
    ModMatrix matrix;
    int selRow = 0;
    float dragBase = 0.f;
    bool learnArmed = false;
    std::map<int, PipeEdit> pipeEdits;
    std::future<WsResult> wsFuture;
    WsResult wsResult;
    bool wsFollow = false;
    int wsKey = -1;
    int learnLast = 0, learnFirst = -1, learnBest = -1;

    DISTRHO_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PipeUI)
};

constexpr const char* PipeUI::kTags[5];

UI* createUI()
{
    return new PipeUI();
}

END_NAMESPACE_DISTRHO
