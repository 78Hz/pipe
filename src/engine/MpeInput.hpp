// Pipe - physically modelled pipe organ
// MIDI and MPE input.
//
// A direct implementation of the MIDI 1.0 channel-voice rules and of MIDI Polyphonic Expression,
// MMA/AMEI RP-053 v1.0 ("the spec"; section numbers below refer to it). Nothing here depends on
// which controller or host is attached, and nothing is inferred from timing: every message means
// what the specification says it means.
//
// Zones (§2.1). The Lower Zone has its Master Channel on channel 1 and Member Channels 2..(1+n);
// the Upper Zone has its Master Channel on channel 16 and Member Channels (15-n)..15. Before any
// MPE Configuration Message (MCM) arrives, Pipe uses the power-on configuration recommended in
// §2.1.3: Lower Zone, Member Channels 2-16. Channels in no zone behave as ordinary MIDI 1.0
// channels.
//
// What each message controls, per note:
//   Member Channel   Channel Pressure  -> that channel's notes' PRESSURE (per-note pressure, §2.5)
//                    CC74              -> that channel's notes' CC74 (§2.6)
//                    Pitch Bend        -> that channel's notes' pitch, x Member range (§2.4)
//   Master Channel   Pitch Bend        -> added to every note in the zone, x Master range (§2.4)
//                    Channel Pressure  -> AFTERTOUCH of every note in the zone (zone-level, §3.2)
//                    CC74              -> CC74 of notes in the zone whose own channel sends none
//                    notes             -> played (§3.2), controlled by the Master Channel's messages
//   Non-MPE channel  Channel Pressure  -> AFTERTOUCH of that channel's notes (MIDI 1.0 aftertouch)
//                    Pitch Bend / CC74 -> that channel's notes (MIDI 1.0, bend range default ±2)
//   Any channel      Poly Key Pressure -> AFTERTOUCH of that key's note (MIDI 1.0). The spec reserves
//                                         it on Member Channels (§2.5); if a sender uses it there
//                                         anyway it is still per-key aftertouch.
//   Host per-note expression (CLAP note expressions, delivered by the plugin wrapper as private
//   events, see kPrivateNoteExpression) -> that note's PRESSURE, CC74 or pitch, as CLAP defines.
//
// Channel messages affect every note sounding on their channel (§2.2.1). Values are tracked on
// every channel whether or not a note is playing, so Pitch Bend and CC74 sent before a Note On set
// the new note's starting pitch and timbre (§3.3). A note stops following its channel at Note Off
// (§3.3). Pitch Bend Sensitivity received on any Member Channel applies to all Member Channels of
// that zone (§2.4); an MCM resets the zone's bend ranges to ±48 (members) and ±2 (master) (§2.4),
// stops the notes on the channels it changes and resets their controls (§2.1.4).
//
// Whether a source exists for a note: MIDI cannot say whether a controller sends pressure at all,
// and treating "never sent" as "zero" would silence every note from a controller without pressure.
// So PRESSURE exists for a note once its zone has received Channel Pressure on a Member Channel
// since the last reset; CC74 exists once a CC74 has arrived on the note's channel (or, for a member
// note, on its Master Channel); AFTERTOUCH exists once aftertouch has arrived for that note's
// channel, zone or key. A source that does not exist leaves the voiced default in the matrix.
//
// MPE+ (Haken Audio): a CC87 immediately before Channel Pressure, CC74 or Pitch Bend carries
// 7 further bits of resolution for it.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace pipe {

// Private event from the plugin wrapper: [0xF4, type, channel, key, v0, v1, v2] (7 bytes);
// value = (v0 | v1 << 7 | v2 << 14) / 2097151 (0..1). type 1 pressure 0..1, 2 brightness 0..1,
// 3 tuning (value*240-120 semitones). Status 0xF4 is undefined in MIDI 1.0 and never sent by devices.
constexpr uint8_t kPrivateNoteExpression = 0xF4;
enum { kExprPressure = 1, kExprBrightness = 2, kExprTuning = 3 };

// Diagnostics: which messages each zone has received (since the last reset) - bit set
enum InputBits {
    kInMemberPressure = 1,   // Channel Pressure on Member Channels (MPE per-note pressure)
    kInNoteExpression = 2,   // host per-note expression (CLAP)
    kInMasterPressure = 4,   // Channel Pressure on the Master Channel (zone-level)
    kInPolyPressure = 8,     // Polyphonic Key Pressure
    kInCC74 = 16,
    kInHiRes = 32,           // MPE+ CC87
    kInNonMpePressure = 64,  // Channel Pressure on a channel in no zone
    kInMcm = 128,            // an MPE Configuration Message
    kInBend = 256,
    kInVelocity = 512,
    kInRelease = 1024,
};

class MpeInput {
public:
    struct Listener {
        virtual ~Listener() = default;
        virtual void onNoteOn(int ch, int key, int vel, int zone) = 0;    // zone 0 lower (and non-MPE), 1 upper
        virtual void onNoteOff(int ch, int key, int vel) = 0;
        virtual void onAllNotesOff(int chMask) = 0;                       // bit per channel
        virtual void onController(int ch, int cc, int val, int zone) = 0; // CCs that are not MPE controls
        virtual void onLearn(int src, int cc) = 0;                        // src: Matrix ModSource
    };

    struct Channel {
        float bend = 0.f;            // -1..1
        float bendRange = 2.f;       // semitones (MIDI 1.0 default; MPE layout sets 48 / 2)
        float pressure = 0.f;  bool pressureSent = false;
        float cc74 = 0.f;      bool cc74Sent = false;
        int rpnMsb = 127, rpnLsb = 127;
        int lsb = -1;                // MPE+ pending extra bits
    };
    struct Note {
        int ch = 0, key = 0, zone = 0;
        float polyAT = 0.f;  bool polyATSent = false;
        float exprPressure = 0.f; bool exprPressureSent = false;
        float exprBrightness = 0.f; bool exprBrightnessSent = false;
        float exprTuning = 0.f;      // semitones
    };

    MpeInput() { reset(); }

    void reset() {
        for (auto& c : chans) c = Channel();
        notes.clear();
        bits[0] = bits[1] = 0;
        zonePressure[0] = zonePressure[1] = false;
        setLayout(15, 0);   // §2.1.3 power-on configuration
        bits[0] = bits[1] = 0;
    }

    // ---- zones
    int lowerMembers() const { return lower; }
    int upperMembers() const { return upper; }
    bool isMaster(int c) const { return (c == 0 && lower > 0) || (c == 15 && upper > 0); }
    bool isMember(int c) const { return (lower > 0 && c >= 1 && c <= lower) || (upper > 0 && c >= 15 - upper && c <= 14); }
    // -1: channel in no zone
    int zoneOf(int c) const {
        if (upper > 0 && (c == 15 || (c >= 15 - upper && c <= 14))) return 1;
        if (lower > 0 && c <= lower) return 0;
        return -1;
    }
    int masterOf(int zone) const { return zone == 1 ? 15 : 0; }

    // ---- per-note values, for the note sounding on (ch, key); false: the source does not exist
    const Note* find(int ch, int key) const {
        for (auto it = notes.rbegin(); it != notes.rend(); ++it) if (it->ch == ch && it->key == key) return &*it;
        return nullptr;
    }
    bool pressure(const Note& n, float& x) const {
        if (n.exprPressureSent) { x = n.exprPressure; return true; }
        if (isMember(n.ch) && zonePressure[n.zone]) { x = chans[n.ch].pressure; return true; }
        return false;
    }
    bool aftertouch(const Note& n, float& x) const {
        if (n.polyATSent) { x = n.polyAT; return true; }
        const int z = zoneOf(n.ch);
        const Channel& own = chans[n.ch];
        if (z < 0 || isMaster(n.ch)) { if (own.pressureSent) { x = own.pressure; return true; } return false; }
        const Channel& m = chans[masterOf(z)];
        if (m.pressureSent) { x = m.pressure; return true; }
        return false;
    }
    bool cc74(const Note& n, float& x) const {
        if (n.exprBrightnessSent) { x = n.exprBrightness; return true; }
        const Channel& own = chans[n.ch];
        if (own.cc74Sent) { x = own.cc74; return true; }
        const int z = zoneOf(n.ch);
        if (z >= 0 && isMember(n.ch) && chans[masterOf(z)].cc74Sent) { x = chans[masterOf(z)].cc74; return true; }
        return false;
    }
    float bendSemitones(const Note& n) const {
        const Channel& own = chans[n.ch];
        float s = own.bend * own.bendRange + n.exprTuning;
        const int z = zoneOf(n.ch);
        if (z >= 0 && isMember(n.ch)) { const Channel& m = chans[masterOf(z)]; s += m.bend * m.bendRange; }
        return s;
    }

    // ---- diagnostics
    int inputBits(int zone) const { return bits[zone & 1]; }
    uint16_t channelsWithNotes() const { uint16_t m = 0; for (const auto& n : notes) m |= (uint16_t)(1u << n.ch); return m; }
    int maxNotesOnOneChannel() const {
        int c[16] = {}; int best = 0;
        for (const auto& n : notes) best = std::max(best, ++c[n.ch]);
        return best;
    }
    const Channel& channel(int c) const { return chans[c & 15]; }
    int noteCount() const { return (int)notes.size(); }

    // ---- input
    void process(const uint8_t* d, int size, Listener& L) {
        if (size < 1) return;
        if (d[0] == kPrivateNoteExpression) { if (size >= 7) noteExpression(d, L); return; }
        if (d[0] >= 0xF0) return;   // system messages
        const int st = d[0] & 0xF0, c = d[0] & 0x0F;
        const int z = zoneOf(c), zi = z < 0 ? 0 : z;
        switch (st) {
        case 0x90:
            if (size < 3) return;
            if (d[2] == 0) { noteOff(c, d[1] & 127, 64, L); return; }   // MIDI 1.0: velocity 0 is Note Off
            noteOn(c, d[1] & 127, d[2], L);
            return;
        case 0x80: if (size >= 3) noteOff(c, d[1] & 127, d[2], L); return;
        case 0xA0:
            if (size < 3) return;
            for (auto& n : notes) if (n.ch == c && n.key == (d[1] & 127)) { n.polyAT = d[2] / 127.f; n.polyATSent = true; }
            bits[zi] |= kInPolyPressure;
            L.onLearn(kLearnAftertouch, 0);
            return;
        case 0xD0: {
            if (size < 2) return;
            Channel& ch = chans[c];
            ch.pressure = withLsb(ch, d[1]); ch.pressureSent = true;
            if (isMember(c)) { zonePressure[zi] = true; bits[zi] |= kInMemberPressure; L.onLearn(kLearnPressure, 0); }
            else { bits[zi] |= (z < 0 ? kInNonMpePressure : kInMasterPressure); L.onLearn(kLearnAftertouch, 0); }
            return;
        }
        case 0xE0: {
            if (size < 3) return;
            Channel& ch = chans[c];
            const int v14 = d[1] | (d[2] << 7);
            if (ch.lsb >= 0) { ch.bend = (float)(((v14 << 7) | ch.lsb) - (8192 << 7)) / (float)(8192 << 7); ch.lsb = -1; }
            else ch.bend = (v14 - 8192) / 8192.f;
            ch.bend = std::max(-1.f, std::min(1.f, ch.bend));
            if (v14 != 8192) bits[zi] |= kInBend;
            if (v14 > 8192 + 1024 || v14 < 8192 - 1024) L.onLearn(kLearnBend, 0);
            return;
        }
        case 0xB0: if (size >= 3) controller(c, d[1] & 127, d[2] & 127, L); return;
        default: return;
        }
    }

    // learn source codes (match Matrix ModSource values)
    static constexpr int kLearnPressure = 1, kLearnAftertouch = 2, kLearnCC74 = 3, kLearnBend = 4, kLearnCC = 6;

private:
    Channel chans[16];
    std::vector<Note> notes;
    int lower = 15, upper = 0;
    bool zonePressure[2] = { false, false };
    int bits[2] = { 0, 0 };

    static float withLsb(Channel& ch, int msb) {
        if (ch.lsb < 0) return msb / 127.f;
        const float v = std::min(1.f, (float)(msb * 128 + ch.lsb) / 16256.f);
        ch.lsb = -1;
        return v;
    }

    void noteOn(int c, int key, int vel, Listener& L) {
        const int z = zoneOf(c), zi = z < 0 ? 0 : z;
        if (vel != 64 && vel != 100 && vel != 127) bits[zi] |= kInVelocity;
        // A second Note On for a key already sounding on this channel retriggers it.
        for (size_t i = 0; i < notes.size(); ++i)
            if (notes[i].ch == c && notes[i].key == key) { L.onNoteOff(c, key, 64); notes.erase(notes.begin() + (long)i); break; }
        Note n; n.ch = c; n.key = key; n.zone = zi;
        notes.push_back(n);
        L.onNoteOn(c, key, vel, zi);
    }
    void noteOff(int c, int key, int vel, Listener& L) {
        const int z = zoneOf(c), zi = z < 0 ? 0 : z;
        if (vel != 64 && vel != 0) bits[zi] |= kInRelease;
        for (size_t i = 0; i < notes.size(); ++i)
            if (notes[i].ch == c && notes[i].key == key) {
                notes.erase(notes.begin() + (long)i);
                L.onNoteOff(c, key, vel);
                return;
            }
    }

    void noteExpression(const uint8_t* d, Listener& L) {
        const int type = d[1], c = d[2] & 15, key = d[3] & 127;
        const float v = (float)(d[4] | (d[5] << 7) | (d[6] << 14)) / 2097151.f;
        for (auto& n : notes) {
            if (n.ch != c || n.key != key) continue;
            if (type == kExprPressure) { n.exprPressure = v; n.exprPressureSent = true; }
            else if (type == kExprBrightness) { n.exprBrightness = v; n.exprBrightnessSent = true; }
            else if (type == kExprTuning) n.exprTuning = v * 240.f - 120.f;
            bits[n.zone] |= kInNoteExpression;
        }
        if (type == kExprPressure) L.onLearn(kLearnPressure, 0);
        else if (type == kExprBrightness) L.onLearn(kLearnCC74, 74);
        else L.onLearn(kLearnBend, 0);
    }

    void controller(int c, int cc, int val, Listener& L) {
        Channel& ch = chans[c];
        const int z = zoneOf(c), zi = z < 0 ? 0 : z;
        switch (cc) {
        case 74:
            ch.cc74 = withLsb(ch, val); ch.cc74Sent = true;
            bits[zi] |= kInCC74;
            L.onLearn(kLearnCC74, 74);
            return;
        case 87: ch.lsb = val; bits[zi] |= kInHiRes; return;   // MPE+ extra bits for the next value
        case 101: ch.rpnMsb = val; return;
        case 100: ch.rpnLsb = val; return;
        case 99: case 98: ch.rpnMsb = ch.rpnLsb = 127; return;  // NRPN selected: data entry is not ours
        case 6:
            if (ch.rpnMsb == 0 && ch.rpnLsb == 0) bendSensitivity(c, (float)val);
            else if (ch.rpnMsb == 0 && ch.rpnLsb == 6) mcm(c, val, L);
            return;
        case 38: return;   // data entry LSB (cents): ignored
        case 121:          // reset all controllers (MIDI 1.0)
            ch.bend = 0.f; ch.pressure = 0.f; ch.cc74 = 0.f; ch.rpnMsb = ch.rpnLsb = 127; ch.lsb = -1;
            return;
        case 120: case 123: {   // all sound off / all notes off: this channel; on a Master Channel, its zone
            int mask = 1 << c;
            if (isMaster(c)) for (int k = 0; k < 16; ++k) if (zoneOf(k) == z) mask |= 1 << k;
            for (size_t i = notes.size(); i-- > 0;) if (mask & (1 << notes[i].ch)) notes.erase(notes.begin() + (long)i);
            L.onAllNotesOff(mask);
            return;
        }
        default:
            L.onController(c, cc, val, zi);
            return;
        }
    }

    // §2.4: on a Member Channel it applies to all Member Channels of the zone; on a Master Channel
    // to the Master; elsewhere to that channel.
    void bendSensitivity(int c, float semis) {
        if (isMember(c)) { const int z = zoneOf(c); for (int k = 0; k < 16; ++k) if (isMember(k) && zoneOf(k) == z) chans[k].bendRange = semis; }
        else chans[c].bendRange = semis;
    }

    // §2.1: MCM on channel 1 (Lower Zone) or 16 (Upper Zone); value = number of Member Channels.
    void mcm(int c, int n, Listener& L) {
        if (c != 0 && c != 15) return;
        n = std::min(15, std::max(0, n));
        const int oldLower = lower, oldUpper = upper;
        int lo = lower, up = upper;
        if (c == 0) { lo = n; if (lo + up > 14) up = std::max(0, 14 - lo); }      // §2.1.1: newest takes precedence
        else { up = n; if (lo + up > 14) lo = std::max(0, 14 - up); }
        // §2.1.4: channels entering or leaving MPE control: stop their notes, reset their controls
        int changed = 0;
        for (int k = 0; k < 16; ++k) {
            const int before = zoneOfWith(k, oldLower, oldUpper), after = zoneOfWith(k, lo, up);
            const bool mb = memberWith(k, oldLower, oldUpper), ma = memberWith(k, lo, up);
            if (before != after || mb != ma || k == c) changed |= 1 << k;
        }
        for (size_t i = notes.size(); i-- > 0;) if (changed & (1 << notes[i].ch)) notes.erase(notes.begin() + (long)i);
        if (changed) L.onAllNotesOff(changed);
        for (int k = 0; k < 16; ++k) if (changed & (1 << k)) { const int r = chans[k].rpnMsb, l = chans[k].rpnLsb; chans[k] = Channel(); chans[k].rpnMsb = r; chans[k].rpnLsb = l; }
        setLayout(lo, up);
        zonePressure[0] = zonePressure[1] = false;
        bits[c == 15 ? 1 : 0] = kInMcm;
    }
    void setLayout(int lo, int up) {
        lower = lo; upper = up;
        // §2.4: an MCM sets Member Channels to ±48 and Master Channels to ±2; channels in no zone keep MIDI's ±2
        for (int k = 0; k < 16; ++k) chans[k].bendRange = isMember(k) ? 48.f : 2.f;
    }
    static int zoneOfWith(int c, int lo, int up) {
        if (up > 0 && (c == 15 || (c >= 15 - up && c <= 14))) return 1;
        if (lo > 0 && c <= lo) return 0;
        return -1;
    }
    static bool memberWith(int c, int lo, int up) { return (lo > 0 && c >= 1 && c <= lo) || (up > 0 && c >= 15 - up && c <= 14); }
};

} // namespace pipe
