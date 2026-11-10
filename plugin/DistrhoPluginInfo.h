// Pipe - physically modelled pipe organ
#pragma once

#define DISTRHO_PLUGIN_BRAND   "78Hz"
#define DISTRHO_PLUGIN_NAME    "Pipe"
#define DISTRHO_PLUGIN_URI     "https://github.com/78Hz/pipe"
#define DISTRHO_PLUGIN_CLAP_ID "com.78hz.pipe"

#define DISTRHO_PLUGIN_BRAND_ID  Hz78
#define DISTRHO_PLUGIN_UNIQUE_ID Pipe

#define DISTRHO_PLUGIN_HAS_UI          1
#define DISTRHO_PLUGIN_IS_RT_SAFE      1
#define DISTRHO_PLUGIN_IS_SYNTH        1
#define DISTRHO_PLUGIN_NUM_INPUTS      0
#define DISTRHO_PLUGIN_NUM_OUTPUTS     2
#define DISTRHO_PLUGIN_WANT_MIDI_INPUT 1
// Tell CLAP and AU hosts that Pipe takes MPE (one channel per note); without it hosts flatten MPE onto one channel
#define DISTRHO_PLUGIN_WANT_MIDI_AS_MPE 1
#define DISTRHO_PLUGIN_WANT_PROGRAMS   1
#define DISTRHO_PLUGIN_WANT_STATE      1
#define DISTRHO_PLUGIN_WANT_FULL_STATE 1
#define DISTRHO_PLUGIN_WANT_DIRECT_ACCESS 0

#define DISTRHO_UI_USE_NANOVG      1
#define DISTRHO_UI_USER_RESIZABLE  1
#define DISTRHO_UI_DEFAULT_WIDTH   1280
#define DISTRHO_UI_DEFAULT_HEIGHT  800

#define DISTRHO_PLUGIN_AU_TYPE          aumu
#define DISTRHO_PLUGIN_LV2_CATEGORY     "lv2:InstrumentPlugin"
#define DISTRHO_PLUGIN_VST3_CATEGORIES  "Instrument|Synth"
#define DISTRHO_PLUGIN_CLAP_FEATURES    "instrument", "synthesizer", "stereo"

#define DISTRHO_UI_FILE_BROWSER 1
