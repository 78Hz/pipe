// Pipe - physically modelled pipe organ
// The interface: one backlit LCD with seven screens.
#include "DistrhoUI.hpp"
#include "../src/engine/Params.hpp"
#include "../src/engine/Presets.hpp"
#include "../src/engine/Tuning.hpp"
#include "../src/engine/Voicing.hpp"
#include "../src/engine/Wind.hpp"
#include "../src/engine/Room.hpp"
#include "../src/engine/Matrix.hpp"
#include "../src/engine/Wav.hpp"
#include "IrMeta.hpp"
#include "FontVT323.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <sstream>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <vector>

START_NAMESPACE_DISTRHO

using namespace pipe;
namespace fs = std::filesystem;

static constexpr float kW = 1280.f, kH = 800.f;

enum Screen { kScrConsole = 0, kScrExpression, kScrWind, kScrVoicing, kScrTuning, kScrRoom, kScrPresets, kNumScreens };
static const char* const kScreenNames[kNumScreens] = { "CONSOLE", "EXPRESSION", "WIND", "VOICING", "TUNING", "ROOM", "PRESETS" };

static std::string userPresetDir()
{
    const char* home = std::getenv("USERPROFILE");
    if (!home || !*home) home = std::getenv("HOME");
    fs::path p = home ? fs::path(home) : fs::temp_directory_path();
    if (fs::exists(p / "Documents")) p /= "Documents";
    return (p / "Pipe Presets").string();
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
        font = createFontFromMemory("vt323", pipe_res::kFontVT323, (uint)pipe_res::kFontVT323Size, false);
        const double sf = getScaleFactor();
        if (sf != 1.0) setSize((uint)(kW * sf), (uint)(kH * sf));
        setGeometryConstraints((uint)(kW * 0.5 * sf), (uint)(kH * 0.5 * sf), true, false);
        for (float& v : windHist) v = 0.f;
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
        if (index == kParamTuning || index == kParamEdoN || index == kParamARef) syncTuning();
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
            if (i == kStateMatrix) matrix.parse(st[i]);
        }
        repaint();
    }

    void uiIdle() override
    {
        learnPoll();
        // 4 s of wind history for the sag graph
        if (++idleCount % 2 == 0)
        {
            windHist[windPos] = pv[kOutChestGt];
            windPos = (windPos + 1) % kHist;
            if (screen == kScrWind) repaint();
        }
    }

    // --------------------------------------------------------------- drawing
    void onNanoDisplay() override
    {
        hits.clear();
        const float s = getWidth() / kW;
        scale(s, s);
        fontFaceId(font);

        // frame + backlit glass
        fill(0, 0, kW, kH, frameCol);
        beginPath(); roundedRect(8, 8, kW - 16, kH - 16, 6); fillColor(lcd); NanoVG::fill();
        // scanlines
        beginPath();
        for (float y = 10; y < kH - 10; y += 3) { moveTo(10, y); lineTo(kW - 10, y); }
        strokeColor(Color(31, 42, 18, 0.06f)); strokeWidth(1); stroke();
        beginPath(); roundedRect(8, 8, kW - 16, kH - 16, 6); strokeColor(Color(147, 166, 64)); strokeWidth(2); stroke();

        drawHeader();
        switch (screen)
        {
        case kScrConsole: drawConsole(); break;
        case kScrExpression: drawExpression(); break;
        case kScrWind: drawWind(); break;
        case kScrVoicing: drawVoicing(); break;
        case kScrTuning: drawTuning(); break;
        case kScrRoom: drawRoom(); break;
        case kScrPresets: drawPresets(); break;
        default: break;
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
    void frame(float x, float y, float w, float h, float t = 2.f)
    {
        beginPath(); rect(x + t * 0.5f, y + t * 0.5f, w - t, h - t); strokeColor(ink); strokeWidth(t); stroke();
    }
    void text(float x, float y, const std::string& s, float size = 20.f, Color c = Color(31, 42, 18), int align = ALIGN_LEFT | ALIGN_MIDDLE)
    {
        fontSize(size); fillColor(c); textAlign(align); NanoVG::text(x, y, s.c_str(), nullptr);
    }
    void titleBar(float x, float y, float w, const std::string& left, const std::string& right = "")
    {
        fill(x, y, w, 26, ink);
        text(x + 8, y + 13, left, 21, lcd);
        if (!right.empty()) text(x + w - 8, y + 13, right, 21, lcd, ALIGN_RIGHT | ALIGN_MIDDLE);
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
        beginPath(); circle(cx, cy, 30); strokeColor(ink); strokeWidth(3); stroke();
        const float a = (-135.f + 270.f * frac - 90.f) * (float)M_PI / 180.f;
        beginPath(); moveTo(cx, cy); lineTo(cx + std::cos(a) * 26, cy + std::sin(a) * 26); strokeWidth(4); stroke();
        text(cx, cy + 46, label, 19, ink, ALIGN_CENTER | ALIGN_MIDDLE);
        fontSize(20);
        Rectangle<float> b; textBounds(0, 0, value.c_str(), nullptr, b);
        const float vw = b.getWidth() + 12;
        fill(cx - vw / 2, cy + 58, vw, 22, ink);
        text(cx, cy + 69, value, 20, lcd, ALIGN_CENTER | ALIGN_MIDDLE);
        addDrag(cx - 34, cy - 34, 68, 68 + 48, param);
    }
    void stepper(float x, float y, float w, const std::string& label, const std::string& value,
                 std::function<void()> minus, std::function<void()> plus, float valueW = 110.f)
    {
        text(x, y + 20, label, 20);
        const float bx = x + w - (44 + 6 + valueW + 6 + 44);
        button(bx, y, 44, 40, "-", false, std::move(minus), 26);
        fill(bx + 50, y, valueW, 40, ink);
        text(bx + 50 + valueW / 2, y + 21, value, 20, lcd, ALIGN_CENTER | ALIGN_MIDDLE);
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
        strokeColor(dashed ? ink2 : ink); strokeWidth(2); stroke();
    }
    void vline(float x, float y, float h) { beginPath(); moveTo(x, y); lineTo(x, y + h); strokeColor(ink); strokeWidth(2); stroke(); }

    // ======================================================= parameter helpers
    void setParam(int id, float v)
    {
        if (id < 0 || id >= kOutFirst) return;
        if (pv[id] == v) return;
        pv[id] = v;
        setParameterValue((uint32_t)id, v);
        if (!loading) dirty = true;
        if (id == kParamTuning || id == kParamEdoN || id == kParamARef) syncTuning();
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
        tuning.aRef = pv[kParamARef];
        parseScl(st[kStateScalaScl], tuning.scl);
        tuning.kbm = ScalaKbm();
        if (!st[kStateScalaKbm].empty()) parseKbm(st[kStateScalaKbm], tuning.kbm);
    }
    bool on(int id) const { return pv[id] >= 0.5f; }

    // ============================================================== header
    void drawHeader()
    {
        text(26, 40, "PIPE", 48);
        float x = 132;
        for (int i = 0; i < kNumScreens; ++i)
        {
            fontSize(21);
            Rectangle<float> b; textBounds(0, 0, kScreenNames[i], nullptr, b);
            const float w = b.getWidth() + 22;
            button(x, 18, w, 44, kScreenNames[i], screen == i, [this, i] { screen = i; if (i == kScrPresets) scanUserPresets(); });
            x += w + 6;
        }
        // preset bar
        const float px = kW - 24 - (44 + 6 + 270 + 6 + 44 + 6 + 70);
        button(px, 18, 44, 44, "<", false, [this] { stepPreset(-1); }, 26);
        frame(px + 50, 18, 270, 44);
        std::string label = presetLabel;
        if (label.size() > 24) label = label.substr(0, 24);
        text(px + 60, 41, label, 21);
        if (dirty) text(px + 50 + 260, 41, "*", 24, ink, ALIGN_RIGHT | ALIGN_MIDDLE);
        addHit(px + 50, 18, 270, 44, [this] { screen = kScrPresets; scanUserPresets(); });
        button(px + 326, 18, 44, 44, ">", false, [this] { stepPreset(1); }, 26);
        button(px + 376, 18, 70, 44, "SAVE", true, [this] { screen = kScrPresets; scanUserPresets(); nameFocus = true; });
        hline(10, 72, kW - 20);
    }

    // ============================================================= console
    void drawConsole()
    {
        const float top = 84, colW = 296, gap = 12;
        static const int order[3] = { kPedal, kGreat, kSwell };
        for (int c = 0; c < 3; ++c)
        {
            const int d = order[c];
            const float x = 20 + c * (colW + gap);
            std::string meta;
            if ((int)pv[kParamZoneLoDiv] == d) meta = "LOWER ZONE";
            else if ((int)pv[kParamZoneHiDiv] == d) meta = "UPPER ZONE";
            else if (d == kPedal && pv[kParamPedalSplit] > 0) meta = std::string("SPLIT <") + noteName12((int)pv[kParamPedalSplit]);
            else meta = d == kSwell ? "ENCLOSED" : "COUPLED";
            titleBar(x, top, colW, divisionName(d), meta);
            float y = top + 32;
            for (int s = 0; s < kNumStops; ++s)
            {
                if (kStops[s].division != d) continue;
                const bool isOn = on(s);
                if (isOn) fill(x, y, colW, 44, ink);
                frame(x, y, colW, 44);
                text(x + 10, y + 23, kStops[s].label, 23, isOn ? lcd : ink);
                text(x + colW - 10, y + 23, kStops[s].foot, 21, isOn ? lcd : ink, ALIGN_RIGHT | ALIGN_MIDDLE);
                addHit(x, y, colW, 44, [this, s] {
                    toggle(s);
                    if (kStops[s].type == kStopRank || kStops[s].type == kStopMixture) selRank = kStops[s].rankFirst;
                });
                y += 49;
            }
        }
        // inspector
        const float ix = 944, iw = 316;
        vline(932, 76, 556);
        const int key = selKey;
        const KeyPitch kp = tuning.keyPitch(key, (int)pv[kParamMapLo], on(kParamEnharmonic));
        const double f = kp.hz * rankRatio(selRank, key);
        const PipeGeometry g = pipeGeometry(*voicing, selRank, key, f);
        titleBar(ix, top, iw, std::string("PIPE ") + (kRanks[selRank].division == kGreat ? "GT " : kRanks[selRank].division == kSwell ? "SW " : "PD ") + kRanks[selRank].name);
        // key selector
        button(ix, top + 34, 40, 34, "<", false, [this] { selKey = std::max(0, selKey - 1); }, 22);
        text(ix + iw / 2, top + 52, std::string("KEY ") + noteName12(key) + "  " + fmt("%.1f HZ", f), 21, ink, ALIGN_CENTER | ALIGN_MIDDLE);
        button(ix + iw - 40, top + 34, 40, 34, ">", false, [this] { selKey = std::min(127, selKey + 1); }, 22);
        // pipe drawing (to scale within the box)
        const float bx = ix + 8, by = top + 80, bh = 190;
        const double maxLen = std::max(0.3, g.lengthM);
        const float pw = (float)std::max(8.0, std::min(40.0, g.diameterMM / 4));
        const float ph = (float)(bh - 40);
        beginPath(); rect(bx + 20 - pw / 2 + 18, by, pw, ph); strokeColor(ink); strokeWidth(3); stroke();
        if (g.stopped) fill(bx + 38 - pw / 2, by - 6, pw, 8, ink);
        if (!g.reed) fill(bx + 38 - pw / 2, by + ph - 18, pw, 10, ink);
        beginPath(); moveTo(bx + 38 - pw / 2, by + ph); lineTo(bx + 38 + pw / 2, by + ph); lineTo(bx + 41, by + bh); lineTo(bx + 35, by + bh); closePath(); stroke();
        (void)maxLen;
        float ty = by + 4;
        auto row = [&](const char* k, const std::string& v) { text(ix + 82, ty + 10, k, 21, ink2); text(ix + iw - 4, ty + 10, v, 21, ink, ALIGN_RIGHT | ALIGN_MIDDLE); ty += 24; };
        row("LENGTH", fmt(g.lengthM >= 1 ? "%.2f M" : "%.3f M", g.lengthM));
        row("DIAMETER", fmt("%.0f MM", g.diameterMM));
        if (!g.reed) { row("MOUTH", fmt("%.0f MM", g.mouthWidthMM)); row("CUT-UP", fmt("%.1f MM", g.cutupMM)); }
        row("TOE WIND", fmt("%.0f MM WC", g.toeMM));
        row("METAL", kRanks[selRank].metalTin > 0 ? fmt("%.0f%% TIN", kRanks[selRank].metalTin) : std::string("WOOD"));
        row("PITCH", fmt("%.1f HZ", f));
        const BuildCheck bc = checkPipe(g);
        const bool ok = bc.mouthOk && bc.cutupOk && bc.sizeOk && bc.pressureOk;
        frame(ix, by + bh + 8, iw, 32);
        text(ix + 10, by + bh + 25, ok ? "[OK] BUILDABLE" : (!bc.cutupOk ? "[!] CUT-UP WON'T SPEAK" : !bc.mouthOk ? "[!] MOUTH TOO WIDE" : !bc.sizeOk ? "[!] SIZE NOT BUILDABLE" : "[!] WIND OUT OF RANGE"), 21);
        float wy = by + bh + 52;
        text(ix, wy + 10, "GREAT CHEST", 21, ink2);
        text(ix + iw, wy + 10, fmt("%.0f / 78 MM", pv[kOutChestGt]), 21, ink, ALIGN_RIGHT | ALIGN_MIDDLE);
        segBar(ix, wy + 24, iw, 14, 20, pv[kOutChestGt] / 85.f);
        wy += 52;
        hline(ix, wy, iw, true);
        text(ix, wy + 18, "SWELL BOX", 21, ink2);
        text(ix, wy + 44, fmt("%.0f%%", pv[kParamSwell] * 100), 34);
        addDrag(ix, wy + 4, iw / 2, 60, kParamSwell);
        text(ix + iw / 2, wy + 18, "TREMULANT", 21, ink2);
        text(ix + iw / 2, wy + 44, on(kNumStops - 1) ? "ON" : "OFF", 34);
        addHit(ix + iw / 2, wy + 4, iw / 2, 60, [this] { toggle(kNumStops - 1); });

        // sounding notes strip
        const float sy = 640;
        hline(10, sy - 6, kW - 20);
        text(20, sy + 12, "SOUNDING  NOTE > PIPE PHYSICS", 21);
        std::string info = std::string(kTuningLabels[(int)pv[kParamTuning]]) + fmt("  A=%.1f", pv[kParamARef]) +
                           fmt("   %.0f PIPES", pv[kOutPipes]) + fmt("   DSP %.0f%%", pv[kOutCpu]);
        text(kW - 20, sy + 12, info, 21, ink, ALIGN_RIGHT | ALIGN_MIDDLE);
        for (int i = 0; i < kNumShownNotes; ++i)
        {
            const float x = 20 + i * 248;
            const float* n = &pv[kOutNote0 + i * 4];
            if (n[0] < 0) { text(x, sy + 50, "--", 28, ghost); continue; }
            const int k = (int)n[0], src = (int)n[1];
            const int d = (src % 1000) / 100, rest = src % 100, zone = rest >= 16 ? 1 : 0, ch = rest - zone * 16;
            const KeyPitch nk = tuning.keyPitch(k, (int)(zone ? pv[kParamMapHi] : pv[kParamMapLo]), on(kParamEnharmonic));
            text(x, sy + 48, std::string(nk.name) + std::to_string(k / 12 - 1), 30);
            std::string srcLabel = src >= 1000 ? std::string("CPL>") + divisionName(d) : std::string(zone ? "HI" : "LO") + " CH" + std::to_string(ch);
            text(x + 228, sy + 48, srcLabel, 20, ink2, ALIGN_RIGHT | ALIGN_MIDDLE);
            segBar(x, sy + 66, 228, 10, 12, n[2] / 120.f);
            text(x, sy + 94, fmt("%.0fMM ", n[2]) + fmt("%+.0fC", n[3]) + (n[2] > 100 ? " !OVBL" : ""), 21);
        }
    }

    // ========================================================== expression
    void drawExpression()
    {
        const float top = 84;
        // ---------------------------------------------------------- inputs
        float x = 20, w = 290, y = top;
        text(x, y + 12, "NOTE INPUTS (MPE ZONES)", 22);
        y += 30;
        for (int z = 0; z < 2; ++z)
        {
            const int param = z == 0 ? kParamZoneLoDiv : kParamZoneHiDiv;
            const int members = (int)pv[z == 0 ? kOutMembersLo : kOutMembersHi];
            const int seen = (int)pv[z == 0 ? kOutSeenLo : kOutSeenHi];
            titleBar(x, y, w, z == 0 ? "LOWER ZONE" : "UPPER ZONE", std::string("> ") + kZoneDivLabels[(int)pv[param]]);
            addHit(x, y, w, 26, [this, param] { setParam(param, (float)(((int)pv[param] + 1) % 4)); });
            float ry = y + 30;
            auto row = [&](const char* k, const std::string& v) { text(x + 6, ry + 10, k, 20, ink2); text(x + w - 6, ry + 10, v, 20, ink, ALIGN_RIGHT | ALIGN_MIDDLE); ry += 22; };
            row("MODE", seen & kSeenMcm ? "MPE (MCM SEEN)" : "MPE DEFAULT");
            if (z == 0) row("CHANNELS", members > 0 ? fmt("1 + 2-%.0f", members + 1) : std::string("1"));
            else row("CHANNELS", members > 0 ? fmt("16 + %.0f-15", 15 - members) : std::string("NONE (SEND MCM)"));
            row("BEND RANGE", fmt("±%.0f ST", pv[z == 0 ? kOutBendLo : kOutBendHi]));
            std::string s;
            if (seen & kSeenVelocity) s += "VEL ";
            if (seen & kSeenPressure) s += "PRES ";
            if (seen & kSeenPolyAT) s += "PAT ";
            if (seen & kSeenBend) s += "BEND ";
            if (seen & kSeenSlide) s += "Y ";
            if (seen & kSeenRelease) s += "REL";
            row("SEEN", s.empty() ? "NO DATA YET" : s);
            frame(x, y, w, ry - y + 6);
            y = ry + 14;
        }
        const int split = (int)pv[kParamPedalSplit];
        stepper(x, y, w, "PEDAL SPLIT", split > 0 ? std::string("< ") + noteName12(split) : std::string("OFF"),
                [this] { nudge(kParamPedalSplit, -1); }, [this] { float v = pv[kParamPedalSplit]; nudge(kParamPedalSplit, v < 1 ? 48 : 1); }, 100);
        y += 52;
        // live sources of the last note touched
        const int key = (int)pv[kOutLiveKey];
        titleBar(x, y, w, "LIVE SOURCES", key >= 0 ? noteName12(key + 36) + ((int)pv[kOutLiveZone] == 0 ? " LO" : " HI") : std::string("NO NOTE"));
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
        live("POLY AT", key >= 0 ? pv[kOutLivePolyAT] : -1.f, 127.f);
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
        titleBar(x, y, w, "BOOST CEILING", "OVERBLOW >100MM");
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
            auto rank = [](int s) { return s == kSrcCC ? 4 : s == kSrcCC74 ? 3 : s == kSrcBend ? 2 : s == kSrcPolyAT ? 1 : 0; };
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
        strokeColor(ink); strokeWidth(3);
        auto box = [&](float x, float y, float w, float h) { beginPath(); rect(x, y, w, h); strokeColor(ink); strokeWidth(3); stroke(); };
        auto line = [&](float x1, float y1, float x2, float y2) { beginPath(); moveTo(x1, y1); lineTo(x2, y2); strokeColor(ink); strokeWidth(3); stroke(); };
        box(ox, oy + 120, 120, 80);
        beginPath(); circle(ox + 60, oy + 155, 22); stroke();
        text(ox + 60, oy + 215, on(kParamBlowerOn) ? "BLOWER" : "BLOWER OFF", 21, ink, ALIGN_CENTER | ALIGN_MIDDLE);
        line(ox + 120, oy + 160, ox + 170, oy + 160);
        box(ox + 170, oy + 115, 190, 90);
        beginPath(); moveTo(ox + 170, oy + 115);
        for (int i = 1; i <= 12; ++i) lineTo(ox + 170 + i * 190.f / 12, oy + (i % 2 ? 100.f : 115.f));
        stroke();
        fill(ox + 215, oy + 75, 40, 22, ink); fill(ox + 275, oy + 75, 40, 22, ink);
        text(ox + 265, oy + 140, "MAIN RESERVOIR", 21, ink, ALIGN_CENTER | ALIGN_MIDDLE);
        text(ox + 265, oy + 175, fmt("%.0f MM", pv[kOutReservoir]), 30, ink, ALIGN_CENTER | ALIGN_MIDDLE);
        line(ox + 360, oy + 160, ox + 400, oy + 160);
        line(ox + 400, oy + 55, ox + 400, oy + 265);
        static const int divs[3] = { kGreat, kSwell, kPedal };
        const int chestOut[3] = { kOutChestGt, kOutChestSw, kOutChestPd };
        for (int i = 0; i < 3; ++i)
        {
            const float cy = oy + 30 + i * 105;
            line(ox + 400, cy + 25, ox + 440, cy + 25);
            box(ox + 440, cy, 100, 50);
            const bool trem = on(kNumStops - 1) && (int)pv[kParamTremTarget] == divs[i];
            text(ox + 490, cy + 26, trem ? "REG+TREM" : "REG", 21, ink, ALIGN_CENTER | ALIGN_MIDDLE);
            line(ox + 540, cy + 25, ox + 580, cy + 25);
            box(ox + 580, cy - 10, 270, 70);
            text(ox + 592, cy + 14, std::string(divisionName(divs[i])) + " CHEST", 21);
            text(ox + 838, cy + 14, fmt("%.0f MM", pv[chestOut[i]]), 21, ink, ALIGN_RIGHT | ALIGN_MIDDLE);
            segBar(ox + 592, cy + 32, 246, 14, 16, pv[chestOut[i]] / (kChestNominal[divs[i]] * 1.1f));
        }
        // sag graph
        const float gy = oy + 340, gh = 150, gx = 30, gw = 870;
        hline(20, gy - 14, 890, true);
        text(gx, gy + 4, "GREAT CHEST PRESSURE · LAST 4 S", 21);
        float mn = 1e9f, mx = -1e9f;
        for (float v : windHist) { mn = std::min(mn, v); mx = std::max(mx, v); }
        text(gx + gw, gy + 4, fmt("NOMINAL 78 · MIN %.0f", mn) + fmt(" · MAX %.0f", mx), 20, ink2, ALIGN_RIGHT | ALIGN_MIDDLE);
        const float base = gy + 24 + gh;
        vline(gx, gy + 24, gh); hline(gx, base, gw);
        const float bw = gw / kHist;
        for (int i = 0; i < kHist; ++i)
        {
            const float v = windHist[(windPos + i) % kHist];
            const float h = std::max(0.f, std::min(gh, (v - 50.f) / 40.f * gh));
            fill(gx + 3 + i * bw, base - h, std::max(1.f, bw - 2), h, ink);
        }
        text(gx, base + 18, "-4.0 S", 20, ink2); text(gx + gw, base + 18, "NOW", 20, ink2, ALIGN_RIGHT | ALIGN_MIDDLE);
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
        static const int td[3] = { kGreat, kSwell, kPedal };
        for (int i = 0; i < 3; ++i)
            button(x + i * 108, ky + 262, 104, 40, divisionName(td[i]), (int)pv[kParamTremTarget] == td[i], [this, i] { setParam(kParamTremTarget, (float)td[i]); });
        const float sy = ky + 316;
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
        // rank list
        text(26, top + 10, "RANK", 20, ink2);
        float y = top + 24;
        for (int r = 0; r < kNumRanks; ++r)
        {
            const bool sel = r == selRank;
            if (sel) fill(20, y, 220, 21, ink);
            const char* div = kRanks[r].division == kGreat ? "GT " : kRanks[r].division == kSwell ? "SW " : "PD ";
            text(26, y + 11, std::string(div) + kRanks[r].name, 20, sel ? lcd : ink);
            addHit(20, y, 220, 21, [this, r] { selRank = r; });
            y += 21;
        }
        vline(250, 76, kH - 90);

        // per-key graphs
        const float x = 266, w = 640;
        const int d = kRanks[selRank].division;
        const int lo = d == kPedal ? kPedalLow : kManualLow, hi = d == kPedal ? kPedalHigh : kManualHigh;
        selKey = std::max(lo, std::min(hi, selKey));
        const KeyPitch kp = tuning.keyPitch(selKey, (int)pv[kParamMapLo], on(kParamEnharmonic));
        const double f = kp.hz * rankRatio(selRank, selKey);
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
            fill(bx, gy + gh - h, std::max(1.f, bw - 2), h, k == selKey ? ink : Color(77, 95, 34));
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
            strokeColor(ink); strokeWidth(pass == 0 ? 2 : 3); stroke();
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
        // intervals: measured from C4 using the subset mapping
        const KeyPitch c4 = tuning.keyPitch(60, kMapSubset, false);
        struct Iv { const char* name; int key; double just; };
        const Iv ivs[3] = { { "FIFTH C-G", 67, 701.955 }, { "MAJOR THIRD C-E", 64, 386.314 }, { "MINOR THIRD C-EB", 63, 315.641 } };
        for (int i = 0; i < 3; ++i)
        {
            const float bx = x + i * 216, by = top + 34;
            frame(bx, by, 208, 84);
            const KeyPitch k2 = tuning.keyPitch(ivs[i].key, kMapSubset, false);
            const double c = 1200.0 * std::log2(k2.hz / c4.hz);
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
        strokeColor(ink); strokeWidth(3); stroke();
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
        text(x + w, ry + 18, "12-KEY BOARDS CAN'T REACH EVERY STEP", 19, ink2, ALIGN_RIGHT | ALIGN_MIDDLE);
        ry += 36;
        const int maps[3] = { kParamMapLo, kParamMapHi, kParamMapPed };
        static const char* mapNames[3] = { "LOWER ZONE", "UPPER ZONE", "PEDAL SPLIT" };
        for (int m = 0; m < 3; ++m)
        {
            text(x, ry + 20, mapNames[m], 21);
            for (int i = 0; i < 3; ++i)
            {
                const int p = maps[m];
                button(x + 160 + i * 150, ry, 144, 40, kMapLabels[i], (int)pv[p] == i, [this, p, i] { setParam(p, (float)i); }, 20);
            }
            ry += 46;
        }
        button(x, ry + 4, w, 36, std::string(on(kParamEnharmonic) ? "[X]" : "[ ]") + " FLATS INSTEAD OF SHARPS (HOLD CC66 TO SWAP LIVE)", false, [this] { toggle(kParamEnharmonic); }, 20);
        text(x, ry + 60, "SUBSET: EACH KEY PLAYS ONE PIPE OF THE CHAIN OF FIFTHS.", 19, ink2);
        text(x, ry + 80, "SEQUENTIAL: EACH KEY IS THE NEXT STEP (GRID CONTROLLERS).", 19, ink2);
        vline(930, 76, kH - 90);

        // per-pipe tuning
        const float px = 944, pw = 316;
        titleBar(px, top, pw, "PER-PIPE TUNING");
        text(px, top + 44, std::string(kRanks[selRank].division == kGreat ? "GT " : kRanks[selRank].division == kSwell ? "SW " : "PD ") + kRanks[selRank].name, 21);
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
        const double flo = tuning.keyPitch(kManualLow, kMapSubset, false).hz, fhi = tuning.keyPitch(kManualHigh, kMapSubset, false).hz;
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
        beginPath(); rect(px, py, pw, ph); strokeColor(ink); strokeWidth(3); stroke();
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
                strokeColor(ink); strokeWidth(pass == 0 ? 3 : 2); stroke();
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
            beginPath(); moveTo(x, y + 34); lineTo(x + w, y + 34); strokeColor(Color(61, 77, 31, 0.5f)); strokeWidth(1); stroke();
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

    const Color ink = Color(31, 42, 18), ink2 = Color(61, 77, 31), lcd = Color(195, 211, 107),
                ghost = Color(174, 191, 90), frameCol = Color(20, 21, 23);

    FontId font = -1;
    float pv[kNumParams];
    std::string st[kNumStates];
    std::unique_ptr<Voicing> voicing;
    Tuning tuning;
    int screen = kScrConsole;
    int selRank = 7, selKey = 60, applyScope = 0;
    int dragHit = -1; float dragStartY = 0, dragStartV = 0;
    float windHist[kHist]; int windPos = 0; unsigned idleCount = 0;
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
    int learnLast = 0, learnFirst = -1, learnBest = -1;

    DISTRHO_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PipeUI)
};

constexpr const char* PipeUI::kTags[5];

UI* createUI()
{
    return new PipeUI();
}

END_NAMESPACE_DISTRHO
