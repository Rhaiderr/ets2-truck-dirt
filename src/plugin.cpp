// ETS2 & ATS truck dirt - viability tests V1 and V2 (logging only).
//
// V1 (telemetry): which substance is under every wheel of the truck and the
// first trailer, wipers, fuel, parking brake and position, once a second in
// telemetry.csv; every configuration the game sends (substance names, truck,
// trailers, job) and every gameplay event in configs.log, plus a line that
// says whether the trailer looks like the player's own (job.market).
// V2 (gpu_probe): which pixel shaders the game draws with, and the constants
// of the glass-drop passes (rain on the glass, wipers).
// V3a (gpu_probe): whole-frame captures with the telemetry pose of the
// player's truck and trailer, to find the player's draws offline.
// V3 part 2 (tint, rig_tracker): the wheel layout and placements of the truck
// and trailer.0 go to the rig tracker, which paints the player's rig red.
// D5 (dirt_state): the dirt of the player's truck and own trailers is kept in
// dirt_state.txt between sessions; company units start clean.
// N10 (test_panel): with [test] panel = 1, panel_key (ctrl+f5) opens a panel
// that forces any level of any layer, drawn over the game (gpu_probe).
// D5 (wash, L7): the stations where the player refuels are remembered
// (wash_spots.txt); stopped there with the parking brake on, the wash key
// (F8) washes the rig behind a black screen, charged through the game's own
// payment only with [wash] charge = 1.
// D10 (N9): its texts in the game's language (the game's log: g_lang).
// Nothing else in the game changes. Files go to Documents\<game>\TruckDirt.

#include <windows.h>
#include <io.h>  // _commit
#include <shlobj.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include "scssdk_telemetry.h"
#include "amtrucks/scssdk_ats.h"
#include "eurotrucks2/scssdk_eut2.h"
#include "common/scssdk_telemetry_common_channels.h"
#include "common/scssdk_telemetry_common_configs.h"
#include "common/scssdk_telemetry_common_gameplay_events.h"
#include "common/scssdk_telemetry_truck_common_channels.h"
#include "common/scssdk_telemetry_trailer_common_channels.h"

#include "dirt_version.h"  // D7: generated at build time (tools/release/version.cmake)
#include "gpu_probe.h"
#include "dirt_model.h"
#include "cargo_dirt.h"
#include "dirt_state.h"
#include "file_writer.h"
#include "rig_tracker.h"
#include "trigger_probe.h"
#include "test_panel.h"
// D5: wash at the station
#include "lang.h"
#include "input_hook.h"
#include "menu.h"
#include "overlay.h"
#include "panel_draw.h"
#include "payment.h"
#include "wash.h"
#include "station.h"
#include "tint.h"

namespace {

constexpr int kMaxWheels = 14;  // the SDK rejects index 14 [verified: tire plugin game.log]
constexpr double kRowIntervalSec = 1.0;

struct Unit {
    int wheel_count = 0;
    scs_u32_t substance[kMaxWheels] = {};
    bool on_ground[kMaxWheels] = {};
    // V3a
    float rotation[kMaxWheels] = {};
    float steering[kMaxWheels] = {};
    float deflection[kMaxWheels] = {};
    float lift[kMaxWheels] = {};
};

struct State {
    scs_log_t log = nullptr;
    bool ats = false;
    FILE* csv = nullptr;
    FILE* configs = nullptr;
    bool paused = true;
    float speed_ms = 0.f;
    float odometer_km = 0.f;
    float fuel_l = 0.f;
    bool wipers = false;
    bool parking_brake = false;
    bool engine = false;       // V4: truck.engine.enabled
    bool trigger_log = false;  // V4: [wash] trigger_log, the map's trigger actions logged
    scs_u32_t game_time = 0;
    int32_t mp_offset = 0;
    scs_value_dplacement_t truck_placement{};
    scs_value_dplacement_t trailer_placement{};  // V3a
    scs_value_fplacement_t cabin_offset{};       // V3a
    Unit truck, trailer;  // trailer = trailer.0
    bool trailer_connected = false;
    // N21 (step 1): trailer.1 .. (a road train's dollies and trailers), recorded in the captures only.
    Unit more[gpu_probe::kPoseMoreTrailers];
    scs_value_dplacement_t more_placement[gpu_probe::kPoseMoreTrailers]{};
    bool more_connected[gpu_probe::kPoseMoreTrailers] = {};
    bool next_has_hook = false;  // N24: trailer.1's hook.position: the first trailer's box ends there
    rig::UnitConfig more_cfg[gpu_probe::kPoseMoreTrailers];  // N21: trailer.1-4's layout, tracked while coupled
    float next_hook[3] = {};
    std::vector<std::string> substances;  // index -> name, from the "substances" configuration
    std::string job_market;               // "" = no job
    rig::UnitConfig rig_truck, rig_trailer;  // V3 part 2
    std::string truck_id, truck_plate;
    std::string trailer_id, trailer_plate, trailer_cargo;
    // N57: the cargo now on the trailer: the job (cargo, source and destination companies, delivery time) and the
    // trailer's cargo accessory; its dirt since it was picked up, kept in cargo_state.txt.
    std::string job_cargo, trailer_cargo_acc;
    cargo::State cargo;
    std::filesystem::path cargo_file;
    bool cargo_dirty = false;
    bool gpu_started = false;
    // D3: dust
    dirt::Model dust;
    float last_odometer = -1.f;
    scs_u32_t last_game_time = 0;
    bool dust_forced = false;  // [dust] level >= 0: a test, nothing is saved
    // D5: saved dirt
    dirt_state::Store saved;
    dirt_state::Tracker tracker{&saved};
    std::filesystem::path state_file;
    // N47: the vehicle of the truck (0) and trailer.0 (1) whose rig map the tracker has (empty: none), and the
    // map last handed to the writer (rig::ExportMap's key and serial).
    std::string map_key[2];
    std::string map_written_key[2];
    long long map_written[2] = {-1, -1};
    std::atomic<bool> map_failed{false};
    // D13c: the game's saves and loads, from its game.log.txt (read on the writer's thread): how many of each were
    // seen, the size handed to the writer, what was taken here, and a load waiting for its game time.
    std::atomic<int> log_saves{0}, log_loads{0};
    unsigned long long log_posted = 0;
    std::chrono::steady_clock::time_point log_poll{};
    int seen_saves = 0, seen_loads = 0;
    bool load_pending = false;
    scs_u32_t load_time0 = 0;
    std::chrono::steady_clock::time_point load_at{};
    bool state_on = false;
    bool rig_changed = false;
    bool state_error_logged = false;
    std::chrono::steady_clock::time_point last_save{};
    // N10: test panel
    panel::Panel panel;
    std::chrono::steady_clock::time_point last_wipe = std::chrono::steady_clock::now();  // N64
    panel::Hotkey washer_key;  // N65: the washer jet ([glass] washer_key); the SDK has no washer channel
    bool washer_key_down = false;
    panel::Hotkey panel_key;
    bool panel_key_down = false;
    // N41: [dust] cab_inside, the box with no dust around the driver's eye (tint::SetCabInside).
    float cab_inside[5] = {0.95f, 1.05f, 0.35f, 0.85f, 0.45f};
    // D11f: [dust] cab_cavity, the cab's whole cavity (the sleeper too): no dust on its faces turned to its middle.
    float cab_cavity[5] = {1.15f, 1.0f, 1.5f, 0.85f, 2.6f};
    panel::Repeat panel_repeat[8];  // one per panel::kNavKeys
    // D5 (L7): wash at the service station
    bool wash_on = false;
    wash::Machine wash;
    wash::Spots spots;
    std::filesystem::path spots_file;
    bool spots_keep = false;  // the file could not be read: kept as wash_spots.txt.bad before the first save
    panel::Hotkey wash_key;
    bool wash_key_down = false;
    bool charge = false;  // [wash] charge = 1 and the game's payment found
    station::Located station;  // N36: the game's refuel offer ([wash] game_station)
    int activation = -2;       // N36: the game's offer at the last frame (log on change)
    payment::Located pay;
    lang::Catalog text;
    std::string currency;  // the base currency's symbol (what the game charges in)
    wash::Phase wash_phase = wash::Phase::kIdle;
    bool block_input = true;  // L12/V6: [menu] block_input, the keyboard hook while the panel or the menu is open
    // V6: the keys menu
    bool menu_on = false;
    menu::Menu menu;
    bool menu_key_down = false;
    uint64_t menu_last_key = 0;  // GetTickCount64 of the last key in the menu (it closes after 2 min without one)
    bool menu_down[256] = {};    // the keys held, for the press edges (polled, or from the hook's events)
    int menu_leaks = 0;          // D11p: keys the hook should have kept that the game got, since the menu opened
    std::wstring ini;
    overlay::Wash overlay_wash;  // the wash's part of the overlay (WashStep)
    float hint_y = 0.82f;        // D11c: [wash] hint_y (D11g: lower, the user 30/10; was 0.70)
    std::chrono::steady_clock::time_point t0{}, last_row{};
    // D11n: the files written on a thread of their own (file_writer.h); what failed there, for the game's thread.
    file_writer::Writer writer;
    std::atomic<bool> state_failed{false}, spots_failed{false};
    std::mutex failed_mutex;
    std::string state_error;  // guarded by failed_mutex
};

State g;

void Log(scs_log_type_t type, const std::string& msg) {
    if (g.log) g.log(type, ("[ets2_truck_dirt] " + msg).c_str());
}

std::wstring DocumentsDir() {
    PWSTR p = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &p))) out = p;
    CoTaskMemFree(p);
    return out;
}

// Each game keeps its own settings and logs, next to its own profiles.
std::wstring DirtDir() {
    // tools/probetest runs the DLL outside the game and points it elsewhere.
    wchar_t test_dir[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"TRUCK_DIRT_TEST_DIR", test_dir, MAX_PATH)) return test_dir;
    std::wstring dir = DocumentsDir() + (g.ats ? L"\\American Truck Simulator" : L"\\Euro Truck Simulator 2") +
                       L"\\TruckDirt";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

// The folder this DLL is in ("" when unknown).
std::wstring PluginDir() {
    wchar_t self[MAX_PATH] = {};
    HMODULE mod = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&PluginDir), &mod) ||
        !GetModuleFileNameW(mod, self, MAX_PATH))
        return L"";
    const std::wstring path = self;
    return path.substr(0, path.rfind(L'\\'));
}

// An ets2_truck_dirt.ini next to this DLL wins; otherwise the one in
// TruckDirt, created when missing (that folder is always writable).
std::wstring IniPath() {
    wchar_t self[MAX_PATH] = {};
    HMODULE mod = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&IniPath), &mod) &&
        GetModuleFileNameW(mod, self, MAX_PATH)) {
        std::wstring next = self;
        next = next.substr(0, next.rfind(L'\\')) + L"\\ets2_truck_dirt.ini";
        if (GetFileAttributesW(next.c_str()) != INVALID_FILE_ATTRIBUTES) return next;
    }
    const std::wstring path = DirtDir() + L"\\ets2_truck_dirt.ini";
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        if (FILE* f = _wfopen(path.c_str(), L"wb")) {
            fputs("; ETS2 & ATS Truck Dirt - viability tests V1/V2 (logging only)\r\n"
                  "[probe]\r\n"
                  "; 1 = log telemetry (substance under the wheels, wipers...) to telemetry.csv\r\n"
                  "telemetry = 1\r\n"
                  "; 1 = log the game's pixel shaders and the glass-drop constants to gpu_probe.log\r\n"
                  "gpu = 1\r\n"
                  "; census period in seconds; glass-drop constants at most every N ms\r\n"
                  "census_s = 60\r\n"
                  "sample_ms = 1000\r\n"
                  "; V3a: one whole-frame capture every N seconds while driving (0 = off), at most M per session\r\n"
                  "capture_s = 0\r\n"
                  "captures = 20\r\n"
                  "; 0 = draw nothing, 1 = paint the player's truck and trailer red (V3 test), 2 = dust (D3),\r\n"
                  "; 3 = position test (colours by position in the vehicle; for tools/probetest),\r\n"
                  "; 4 = diagnosis (green = dust, black = none, red/blue = the G-buffer read wrong)\r\n"
                  "tint = 2\r\n"
                  "; 1 = also log the V2/V2c diagnostics every second (rain, particles, glass-drop constants)\r\n"
                  "verbose = 0\r\n"
                  "[dust]\r\n"
                  "; multiplies how fast the rig gets dusty (test: 10 = ten times faster)\r\n"
                  "rate = 1.0\r\n"
                  "; test: forces the dust level of truck and trailer, 0.0 to 1.0 (-1 = the real level)\r\n"
                  "level = -1\r\n"
                  "[glass]\r\n"
                  "; N40 (being tested): 1 = the truck's glass takes the truck's dirt, 0 = clean glass,\r\n"
                  ";   2 = paint the glass the dirt reaches magenta, 3 = paint each pixel where the shader\r\n"
                  ";   places it, 4 = paint the arc the wipers sweep, 5 = paint the dirt it worked out\r\n"
                  ";   (red how much, green what the blades took) (tests only)\r\n"
                  "on = 0\r\n"
                  "; how strong the dirt on the glass is (0.5 = half, 2 = twice)\r\n"
                  "strength = 1.0\r\n"
                  "; N64k: how bright a surface of that dirt is, in the scene's own units. The game's\r\n"
                  ";   forward target holds radiance before the tone map, where a sunlit surface is worth\r\n"
                  ";   tens, so a reflectance of 0..1 written there comes out nearly black. Higher = the\r\n"
                  ";   pane obstructs more; it is also the 'glass light' row of the test panel.\r\n"
                  "brightness = 10\r\n"
                  "; N64: the arc the wipers clear on the windshield: how far the pivots sit from its middle and\r\n"
                  ";   how high, as shares of the pane the plugin measures; the blade's length as a share of its\r\n"
                  ";   height; half the sweep in degrees. 0 = the wipers clear nothing.\r\n"
                  "wiper = 0.45 0.02 0.48 42\r\n"
                  "; N65: the key for the washer jet, which takes everything inside that arc (the film and the\r\n"
                  ";   soot too, which the blades alone never do). Empty = off.\r\n"
                  "washer_key = f9\r\n"
                  "[test]\r\n"
                  "; 1 = test panel: panel_key opens it; it forces any level of any dirt layer (nothing is saved\r\n"
                  "; from it: the real dirt goes on underneath)\r\n"
                  "panel = 0\r\n"
                  "panel_key = ctrl+f5\r\n"
                  "[wash]\r\n"
                  "; D5: wash at the service station: at the pump (the game offers the refuel there, N36) or at a\r\n"
                  "; station where the truck was refuelled, stopped with the parking brake on, key washes the rig\r\n"
                  "enabled = 1\r\n"
                  "; N36: 1 = the game's own refuel offer marks the station, 0 = only the stations learnt at a refuel\r\n"
                  "game_station = 1\r\n"
                  "key = ctrl+f7\r\n"
                  "price_truck = 120\r\n"
                  "price_trailer = 180\r\n"
                  "; 1 = charge the price through the game's own payment, 0 = free\r\n"
                  "charge = 1\r\n"
                  "[menu]\r\n"
                  "; V6: key opens the keys menu (wash key, test panel key); block_input = 1 keeps\r\n"
                  "; the game from getting the keys while the menu or the test panel is open\r\n"
                  "key = shift+f7\r\n"
                  "block_input = 1\r\n"
                  "[lang]\r\n"
                  "; the plugin's texts: auto = the game's language (English when the font lacks its letters), or a\r\n"
                  "; code such as pt_br, de_de, en_gb\r\n"
                  "code = auto\r\n"
                  "[compat]\r\n"
                  "; self-check of the game's render (see docs/compatibilidade.md): when an update or a mod breaks\r\n"
                  "; something the drawing relies on, 1 = turn that part of the drawing off, 0 = only write it to the log\r\n"
                  "enforce = 1\r\n",
                  f);
            fclose(f);
        }
    }
    return path;
}

void Flush() {
    for (const auto& m : gpu_probe::TakeMessages()) Log(SCS_LOG_TYPE_message, m);
}

std::string Timestamp() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    char b[32];
    snprintf(b, sizeof(b), "%04d-%02d-%02d %02d:%02d:%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
             st.wSecond);
    return b;
}

// A line of configs.log, on the writer's thread (D11n).
void ConfigWrite(const std::string& line) {
    if (!g.configs) return;
    fputs(line.c_str(), g.configs);
    fflush(g.configs);
}

void ConfigLog(const std::string& text) {
    if (!g.configs) return;
    g.writer.Post([line = Timestamp() + " " + text + "\n"] { ConfigWrite(line); });  // the time it happened
}

std::string ValueText(const scs_value_t& v) {
    char b[160];
    switch (v.type) {
        case SCS_VALUE_TYPE_bool: return v.value_bool.value ? "true" : "false";
        case SCS_VALUE_TYPE_s32: return std::to_string(v.value_s32.value);
        case SCS_VALUE_TYPE_u32: return std::to_string(v.value_u32.value);
        case SCS_VALUE_TYPE_s64: return std::to_string(v.value_s64.value);
        case SCS_VALUE_TYPE_u64: return std::to_string(v.value_u64.value);
        case SCS_VALUE_TYPE_float: snprintf(b, sizeof(b), "%g", v.value_float.value); return b;
        case SCS_VALUE_TYPE_double: snprintf(b, sizeof(b), "%.6f", v.value_double.value); return b;
        case SCS_VALUE_TYPE_fvector:
            snprintf(b, sizeof(b), "(%g %g %g)", v.value_fvector.x, v.value_fvector.y, v.value_fvector.z);
            return b;
        case SCS_VALUE_TYPE_dvector:
            snprintf(b, sizeof(b), "(%.4f %.4f %.4f)", v.value_dvector.x, v.value_dvector.y, v.value_dvector.z);
            return b;
        case SCS_VALUE_TYPE_euler:
            snprintf(b, sizeof(b), "(h %g p %g r %g)", v.value_euler.heading, v.value_euler.pitch, v.value_euler.roll);
            return b;
        case SCS_VALUE_TYPE_string: return v.value_string.value ? v.value_string.value : "";
        default: return "?";
    }
}

// ---- V1: CSV ----

bool OpenFiles() {
    const std::wstring dir = DirtDir();
    g.configs = _wfopen((dir + L"\\configs.log").c_str(), L"ab");
    g.csv = _wfopen((dir + L"\\telemetry.csv").c_str(), L"ab");
    if (!g.csv) return false;
    fseek(g.csv, 0, SEEK_END);
    if (ftell(g.csv) == 0)
        fprintf(g.csv, "time,t_s,game_time_min,paused,speed_kmh,odometer_km,wipers,parking_brake,fuel_l,pos_x,pos_y,pos_z,"
                       "heading,truck_wheels,truck_substances,truck_on_ground,trailer_connected,trailer_wheels,"
                       "trailer_substances,trailer_on_ground,job_market\n");
    return true;
}

std::string SubstanceName(scs_u32_t i) {
    if (i < g.substances.size() && !g.substances[i].empty()) return g.substances[i];
    return "#" + std::to_string(i);
}

std::string Substances(const Unit& u) {
    std::string s;
    for (int w = 0; w < u.wheel_count; ++w) s += (w ? "/" : "") + SubstanceName(u.substance[w]);
    return s;
}

std::string OnGround(const Unit& u) {
    std::string s;
    for (int w = 0; w < u.wheel_count; ++w) s += u.on_ground[w] ? '1' : '0';
    return s;
}

void WriteRow() {
    if (!g.csv) return;
    const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - g.t0).count();
    const auto& p = g.truck_placement;
    char b[1024];
    snprintf(b, sizeof(b), "%s,%.1f,%u,%d,%.2f,%.3f,%d,%d,%.2f,%.2f,%.2f,%.2f,%.4f,%d,%s,%s,%d,%d,%s,%s,%s\n",
            Timestamp().c_str(), t, g.game_time, g.paused ? 1 : 0, g.speed_ms * 3.6f, g.odometer_km, g.wipers ? 1 : 0,
            g.parking_brake ? 1 : 0, g.fuel_l, p.position.x, p.position.y, p.position.z, p.orientation.heading,
            g.truck.wheel_count, Substances(g.truck).c_str(), OnGround(g.truck).c_str(), g.trailer_connected ? 1 : 0,
            g.trailer.wheel_count, Substances(g.trailer).c_str(), OnGround(g.trailer).c_str(),
            g.job_market.empty() ? "-" : g.job_market.c_str());
    g.writer.Post([row = std::string(b)] {  // D11n: written on the writer's thread
        fputs(row.c_str(), g.csv);
        fflush(g.csv);
    });
}

// ---- callbacks ----

SCSAPI_VOID OnFloat(const scs_string_t, const scs_u32_t, const scs_value_t* value, const scs_context_t ctx) {
    if (value && value->type == SCS_VALUE_TYPE_float) *static_cast<float*>(ctx) = value->value_float.value;
}

SCSAPI_VOID OnBool(const scs_string_t, const scs_u32_t, const scs_value_t* value, const scs_context_t ctx) {
    if (value && value->type == SCS_VALUE_TYPE_bool) *static_cast<bool*>(ctx) = value->value_bool.value != 0;
}

SCSAPI_VOID OnU32(const scs_string_t, const scs_u32_t, const scs_value_t* value, const scs_context_t ctx) {
    if (value && value->type == SCS_VALUE_TYPE_u32) *static_cast<scs_u32_t*>(ctx) = value->value_u32.value;
}

SCSAPI_VOID OnS32(const scs_string_t, const scs_u32_t, const scs_value_t* value, const scs_context_t ctx) {
    if (value && value->type == SCS_VALUE_TYPE_s32) *static_cast<int32_t*>(ctx) = value->value_s32.value;
}

SCSAPI_VOID OnDPlacement(const scs_string_t, const scs_u32_t, const scs_value_t* value, const scs_context_t ctx) {
    if (value && value->type == SCS_VALUE_TYPE_dplacement)
        *static_cast<scs_value_dplacement_t*>(ctx) = value->value_dplacement;
}

SCSAPI_VOID OnFPlacement(const scs_string_t, const scs_u32_t, const scs_value_t* value, const scs_context_t ctx) {
    if (value && value->type == SCS_VALUE_TYPE_fplacement)
        *static_cast<scs_value_fplacement_t*>(ctx) = value->value_fplacement;
}

// Indexed channels: the context is the unit, the index is the wheel.
SCSAPI_VOID OnWheelSubstance(const scs_string_t, const scs_u32_t index, const scs_value_t* value, const scs_context_t ctx) {
    if (value && value->type == SCS_VALUE_TYPE_u32 && index < kMaxWheels)
        static_cast<Unit*>(ctx)->substance[index] = value->value_u32.value;
}

SCSAPI_VOID OnWheelOnGround(const scs_string_t, const scs_u32_t index, const scs_value_t* value, const scs_context_t ctx) {
    if (value && value->type == SCS_VALUE_TYPE_bool && index < kMaxWheels)
        static_cast<Unit*>(ctx)->on_ground[index] = value->value_bool.value != 0;
}

// V3a: the context is one of the unit's per-wheel float arrays.
SCSAPI_VOID OnWheelFloat(const scs_string_t, const scs_u32_t index, const scs_value_t* value, const scs_context_t ctx) {
    if (value && value->type == SCS_VALUE_TYPE_float && index < kMaxWheels)
        static_cast<float*>(ctx)[index] = value->value_float.value;
}

// V1: whose trailer is it? The player's own trailer pulls cargo_market jobs or
// runs without a job; every other market brings the company's trailer.
void LogOwnership(const char* why) {
    std::string verdict;
    if (!g.trailer_connected && g.trailer_id.empty()) verdict = "no trailer";
    else if (g.job_market.empty()) verdict = "player's own (no job)";
    else if (g.job_market == "cargo_market") verdict = "player's own (cargo_market)";
    else verdict = "company (" + g.job_market + ")";
    ConfigLog(std::string("ownership (") + why + "): trailer.0 id=" + g.trailer_id + " plate=" + g.trailer_plate +
              " cargo=" + g.trailer_cargo + " connected=" + (g.trailer_connected ? "1" : "0") + " job.market=" +
              (g.job_market.empty() ? "-" : g.job_market) + " -> " + verdict);
}

// ---- N47: rig maps ----

// TruckDirt\rigs, created when missing.
std::filesystem::path MapDir() {
    const std::filesystem::path dir = std::filesystem::path(DirtDir()) / L"rigs";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

// The vehicle a unit's map is for: its model (the config's id: "vehicle.volvo.fh_2024", "vehicle.scs.grainhopper")
// and a hash of its wheels (and radii), hook and cab pivot (another chassis or cab of one model draws other parts:
// and a new layout makes the tracker forget, so the key changes with it). The
// trailer's id is its model, not the one trailer: two of a model share a map.
std::string MapKey(const char* kind, const std::string& id, const rig::UnitConfig& rc, const float* cab) {
    if (id.empty() || !rc.wheels) return "";
    char b[64];
    std::string layout;
    for (int w = 0; w < rc.wheels; ++w) {
        snprintf(b, sizeof(b), "%.3f %.3f %.3f %.3f;", rc.wheel[w][0], rc.wheel[w][1], rc.wheel[w][2], rc.radius[w]);
        layout += b;
    }
    if (rc.has_hook) snprintf(b, sizeof(b), "h %.3f %.3f %.3f;", rc.hook[0], rc.hook[1], rc.hook[2]), layout += b;
    if (cab) snprintf(b, sizeof(b), "c %.3f %.3f %.3f;", cab[0], cab[1], cab[2]), layout += b;
    uint32_t h = 2166136261u;
    for (unsigned char c : layout) h = (h ^ c) * 16777619u;
    snprintf(b, sizeof(b), " %08x", h);
    return std::string(kind) + " " + id + b;
}

// The key as a file name: letters, digits, '.', '-' and '_' (the rest '_').
std::filesystem::path MapFile(const std::string& key) {
    std::string name = key;
    for (char& c : name)
        if (!isalnum(static_cast<unsigned char>(c)) && c != '.' && c != '-' && c != '_') c = '_';
    return MapDir() / (name + ".txt");
}

// The maps the tracker wrote since the last time, to their files (on the writer's thread).
void MapSave() {
    for (int u = 0; u < 2; ++u) {
        std::string key;
        std::vector<rig::MapPart> parts;
        long long serial = 0;
        if (!rig::ExportMap(u, &key, &parts, &serial)) continue;
        if (key == g.map_written_key[u] && serial == g.map_written[u]) continue;
        g.map_written_key[u] = key;
        g.map_written[u] = serial;
        g.writer.Post([key, parts = std::move(parts)] {  // the text made here, off the game's thread
            std::string error;
            if (!dirt_state::Store::WriteText(MapFile(key), rig::MapText(key, parts), &error)) g.map_failed.store(true);
        });
    }
}

// A unit's vehicle (truck 0, trailer.0 1): its map read on the writer's thread (after a write of it still
// queued) and handed to the tracker. The same vehicle again (the SDK sends the config again) changes nothing.
void MapLoad(int unit, const std::string& key) {
    if (key == g.map_key[unit]) return;
    MapSave();  // the vehicle leaving: what it learnt since the last save (the tracker forgets it at the new layout)
    g.map_key[unit] = key;
    if (key.empty()) {
        rig::ImportMap(unit, "", "");
        return;
    }
    g.writer.Post([unit, key] {
        std::string text;
        if (FILE* f = _wfopen(MapFile(key).c_str(), L"rb")) {
            char buf[65536];
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
            fclose(f);
        }
        const int parts = rig::ImportMap(unit, key, text);
        ConfigWrite(Timestamp() + " rig map (N47): " + key + ": " + std::to_string(parts) + " parts read" +
                    (text.empty() ? " (none yet)" : "") + "\n");
    });
}

SCSAPI_VOID OnConfig(const scs_event_t, const void* info, const scs_context_t) {
    const auto* cfg = static_cast<const scs_telemetry_configuration_t*>(info);
    if (!cfg || !cfg->id) return;
    const std::string id = cfg->id;
    std::string text = "config " + id + ":";
    int wheel_count = 0;
    std::string str_id, plate, cargo, market;
    std::string cargo_acc, job_parts[3];  // N57: cargo.accessory.id; the job's cargo.id, source and destination
    scs_u32_t delivery = 0;
    rig::UnitConfig rc;  // V3 part 2: wheel layout of this unit
    bool has_cabin = false;  // D3c: the cab's pivot (truck)
    float cabin[3] = {};
    bool has_head = false;  // N41: the driver's head in the cabin
    float head[3] = {};
    if (id == SCS_TELEMETRY_CONFIG_substances) g.substances.clear();
    for (const scs_named_value_t* a = cfg->attributes; a && a->name; ++a) {
        text += std::string("\n    ") + a->name;
        if (a->index != SCS_U32_NIL) text += "[" + std::to_string(a->index) + "]";
        text += " = " + ValueText(a->value);
        const bool is_str = a->value.type == SCS_VALUE_TYPE_string && a->value.value_string.value;
        if (id == SCS_TELEMETRY_CONFIG_substances && is_str && !strcmp(a->name, SCS_TELEMETRY_CONFIG_ATTRIBUTE_id) &&
            a->index != SCS_U32_NIL && a->index < 256) {
            if (g.substances.size() <= a->index) g.substances.resize(a->index + 1);
            g.substances[a->index] = a->value.value_string.value;
        }
        if (!strcmp(a->name, SCS_TELEMETRY_CONFIG_ATTRIBUTE_wheel_count) && a->value.type == SCS_VALUE_TYPE_u32)
            wheel_count = static_cast<int>(a->value.value_u32.value);
        if (!strcmp(a->name, SCS_TELEMETRY_CONFIG_ATTRIBUTE_wheel_position) && a->value.type == SCS_VALUE_TYPE_fvector &&
            a->index != SCS_U32_NIL && a->index < static_cast<scs_u32_t>(rig::kMaxWheels)) {
            const auto& v = a->value.value_fvector;
            rc.wheel[a->index][0] = v.x, rc.wheel[a->index][1] = v.y, rc.wheel[a->index][2] = v.z;
            rc.wheels = (std::max)(rc.wheels, static_cast<int>(a->index) + 1);
        }
        if (!strcmp(a->name, SCS_TELEMETRY_CONFIG_ATTRIBUTE_wheel_radius) && a->value.type == SCS_VALUE_TYPE_float &&
            a->index != SCS_U32_NIL && a->index < static_cast<scs_u32_t>(rig::kMaxWheels)) {
            const float v = a->value.value_float.value;
            rc.radius[a->index] = std::isfinite(v) && v > 0.1f && v < 2.f ? v : 0.f;  // N22
        }
        if (!strcmp(a->name, SCS_TELEMETRY_CONFIG_ATTRIBUTE_cabin_position) && a->value.type == SCS_VALUE_TYPE_fvector) {
            const auto& v = a->value.value_fvector;
            cabin[0] = v.x, cabin[1] = v.y, cabin[2] = v.z;
            has_cabin = true;
        }
        if (!strcmp(a->name, SCS_TELEMETRY_CONFIG_ATTRIBUTE_head_position) && a->value.type == SCS_VALUE_TYPE_fvector) {
            const auto& v = a->value.value_fvector;
            head[0] = v.x, head[1] = v.y, head[2] = v.z;
            has_head = true;
        }
        if (!strcmp(a->name, SCS_TELEMETRY_CONFIG_ATTRIBUTE_hook_position) && a->value.type == SCS_VALUE_TYPE_fvector) {
            const auto& v = a->value.value_fvector;
            rc.hook[0] = v.x, rc.hook[1] = v.y, rc.hook[2] = v.z;
            rc.has_hook = true;
        }
        if (is_str && !strcmp(a->name, SCS_TELEMETRY_CONFIG_ATTRIBUTE_id)) str_id = a->value.value_string.value;
        if (is_str && !strcmp(a->name, SCS_TELEMETRY_CONFIG_ATTRIBUTE_license_plate)) plate = a->value.value_string.value;
        if (is_str && !strcmp(a->name, SCS_TELEMETRY_CONFIG_ATTRIBUTE_cargo)) cargo = a->value.value_string.value;
        if (is_str && !strcmp(a->name, SCS_TELEMETRY_CONFIG_ATTRIBUTE_job_market)) market = a->value.value_string.value;
        if (is_str && !strcmp(a->name, "cargo.accessory.id")) cargo_acc = a->value.value_string.value;  // N57
        if (is_str && !strcmp(a->name, "cargo.id")) job_parts[0] = a->value.value_string.value;
        if (is_str && !strcmp(a->name, "source.company.id")) job_parts[1] = a->value.value_string.value;
        if (is_str && !strcmp(a->name, "destination.company.id")) job_parts[2] = a->value.value_string.value;
        if (!strcmp(a->name, "delivery.time") && a->value.type == SCS_VALUE_TYPE_u32) delivery = a->value.value_u32.value;
    }
    ConfigLog(text);
    if (id == SCS_TELEMETRY_CONFIG_substances) {
        Log(SCS_LOG_TYPE_message, std::to_string(g.substances.size()) + " substances");
    } else if (id == SCS_TELEMETRY_CONFIG_truck) {
        g.truck.wheel_count = (std::min)(wheel_count, kMaxWheels);
        rc.wheels = (std::min)(rc.wheels, g.truck.wheel_count);
        rc.has_hook = false;  // the truck's hook is the fifth wheel, inside its box
        rig::SetUnit(0, rc);
        rig::SetCabin(has_cabin, cabin);
        MapLoad(0, MapKey("truck", str_id, rc, has_cabin ? cabin : nullptr));  // N47
        // N41: the driver's eye in the truck (the cab's frame is the truck's at rest), for the box with no dust.
        const float eye[3] = {cabin[0] + head[0], cabin[1] + head[1], cabin[2] + head[2]};
        tint::SetCabInside(has_cabin && has_head, eye, g.cab_inside, g.cab_cavity);
        rig::SetDriver(has_cabin && has_head, eye);  // D12c: the driver is no part of the truck
        char inside_log[300];
        snprintf(inside_log, sizeof(inside_log),
                 "cab inside (N41): %s, eye %.2f %.2f %.2f, box half width %.2f, below %.2f above %.2f ahead %.2f behind %.2f; "
                 "cavity (D11f): %s, %.2f %.2f %.2f %.2f %.2f",
                 has_cabin && has_head && g.cab_inside[0] > 0.f ? "on" : "off", eye[0], eye[1], eye[2], g.cab_inside[0],
                 g.cab_inside[1], g.cab_inside[2], g.cab_inside[3], g.cab_inside[4],
                 has_cabin && has_head && g.cab_cavity[0] > 0.f ? "on" : "off", g.cab_cavity[0], g.cab_cavity[1],
                 g.cab_cavity[2], g.cab_cavity[3], g.cab_cavity[4]);
        ConfigLog(inside_log);
        g.truck_id = str_id;
        g.truck_plate = plate;
        tint::SetTruckId(g.truck_id.c_str());  // N68: its wiper blades come from the pre-map
        g.rig_changed = true;
    } else if (id == "trailer.0") {
        g.trailer.wheel_count = (std::min)(wheel_count, kMaxWheels);
        rc.wheels = (std::min)(rc.wheels, g.trailer.wheel_count);  // an empty config = no trailer
        rig::SetUnit(1, rc);
        MapLoad(1, MapKey("trailer", str_id, rc, nullptr));  // N47
        g.trailer_id = str_id;
        g.trailer_plate = plate;
        g.trailer_cargo = cargo;
        g.trailer_cargo_acc = cargo_acc;  // N57
        g.rig_changed = true;
        LogOwnership("trailer config");
    } else if (id.size() == 9 && id.compare(0, 8, "trailer.") == 0 && id[8] >= '1' &&
               id[8] < '1' + gpu_probe::kPoseMoreTrailers) {
        g.more[id[8] - '1'].wheel_count = (std::min)(wheel_count, kMaxWheels);  // N21 (step 1): captures only
        rc.wheels = (std::min)(rc.wheels, g.more[id[8] - '1'].wheel_count);  // N21: tracked too (OnFrameEnd)
        g.more_cfg[id[8] - '1'] = rc;
        if (id[8] == '1') {  // N24
            g.next_has_hook = rc.has_hook;
            for (int i = 0; i < 3; ++i) g.next_hook[i] = rc.hook[i];
        }
    } else if (id == SCS_TELEMETRY_CONFIG_job) {
        g.job_market = market;  // an empty job config = no job
        // N57: one job's cargo (a new job, even of the same cargo, brings a new load).
        g.job_cargo = job_parts[0].empty() ? "" : job_parts[0] + "|" + job_parts[1] + "|" + job_parts[2] + "|" +
                                                     std::to_string(delivery);
        g.rig_changed = true;
        LogOwnership("job config");
    }
}

SCSAPI_VOID OnGameplay(const scs_event_t, const void* info, const scs_context_t) {
    const auto* ev = static_cast<const scs_telemetry_gameplay_event_t*>(info);
    if (!ev || !ev->id) return;
    std::string text = std::string("gameplay ") + ev->id;
    for (const scs_named_value_t* a = ev->attributes; a && a->name; ++a)
        text += std::string(", ") + a->name + " = " + ValueText(a->value);
    ConfigLog(text);
}

// D5: the units' real layers (without the INI or panel overrides) as dirt_state keeps them: "<layer>" (body),
// "<layer>_front" (the trailer's front face) and "<layer>_reach" (how high the body's layer reaches).
void DustLayers(dirt_state::Layers out[dirt::kUnits]) {
    for (int u = 0; u < dirt::kUnits; ++u) {
        const dirt::Coat c = g.dust.Real(u);
        for (int k = 0; k < dirt::kLayers; ++k) {
            const std::string name = dirt::LayerName(static_cast<dirt::Layer>(k));
            out[u][name] = c.body[k];
            if (k == dirt::kDust) continue;
            out[u][name + "_front"] = c.front[k];
            out[u][name + "_reach"] = c.reach[k];
        }
    }
    out[0]["dust_wipe"] = g.dust.ArcWipe();  // N64: the share of the dust the blades have taken inside their arc
    out[0]["dust_arc_other"] = g.dust.ArcOther();  // N65: the share of the other layers the jet has left there
}

// D5: a unit that came in: clean, then its saved layers (none = stays clean).
void DustRestore(int unit, const dirt_state::Layers& saved) {
    g.dust.Reset(unit);
    for (int k = 0; k < dirt::kLayers; ++k) {
        const auto l = static_cast<dirt::Layer>(k);
        const std::string name = dirt::LayerName(l);
        const auto body = saved.find(name), front = saved.find(name + "_front"), reach = saved.find(name + "_reach");
        if (body != saved.end()) g.dust.SetLayer(unit, l, body->second);
        if (front != saved.end()) g.dust.SetLayer(unit, l, front->second, true);
        if (reach != saved.end()) g.dust.SetReach(unit, l, reach->second);
    }
    if (unit == 0) {  // N64: a save from before this has none of these, and the pane starts dirty all over
        const auto wipe = saved.find("dust_wipe");
        g.dust.SetArcWipe(wipe != saved.end() ? wipe->second : 0.f);
        const auto other = saved.find("dust_arc_other");  // N65: and with all of its other layers
        g.dust.SetArcOther(other != saved.end() ? other->second : 1.f);
    }
}

// D5: the player's units into dirt_state.txt (only when something changed).
void SaveState(const char* why) {
    MapSave();  // N47: whatever the dirt state (the maps are the tracker's)
    if (!g.state_on || g.dust_forced) return;
    if (!g.cargo.key.empty() && !g.cargo_file.empty()) {  // N57: the cargo's base, each save (it follows the washes)
        g.writer.Post([text = cargo::Serialize(g.cargo), file = g.cargo_file] {
            std::string error;
            dirt_state::Store::WriteText(file, text, &error);
        });
        g.cargo_dirty = false;
    }
    dirt_state::Layers cur[dirt::kUnits];
    DustLayers(cur);
    g.tracker.Commit(cur);
    g.last_save = std::chrono::steady_clock::now();
    if (!g.saved.Dirty()) return;
    if (g.saved.ReadOnly()) {
        if (!g.state_error_logged) {
            g.state_error_logged = true;
            Log(SCS_LOG_TYPE_warning, "dirt_state.txt not saved: the file is of a newer format or could not be read: left alone");
        }
        return;
    }
    // D11n: the text now, the file on the writer's thread (a failure marks the store dirty again: SaveFailures).
    g.saved.MarkClean();
    g.writer.Post([text = g.saved.Serialize(),
                   line = Timestamp() + " dirt_state saved (" + why + "): " + std::to_string(g.saved.Size()) + " units\n"] {
        std::string error;
        if (dirt_state::Store::WriteText(g.state_file, text, &error)) {
            ConfigWrite(line);
            return;
        }
        std::lock_guard<std::mutex> lock(g.failed_mutex);
        g.state_error = error;
        g.state_failed.store(true);
    });
}

// ---- D13c: the dirt with the game's saves ----
// The user, 30/10 (the PC switched off by accident): the game reopens at its last save, the dirt should be that
// save's too. Every save the game makes ("Game save completed" in its game.log.txt) also writes the dirt to
// TruckDirt\saves\g<game time>.txt (the game time, in minutes, is saved with the game); a load ("Loading save.")
// takes back the snapshot of the game time it brought, when there is one (else the dirt stays as it was).
constexpr int kSaveSnapshots = 40;  // the newest ones kept

std::filesystem::path SnapshotFile(scs_u32_t game_time) {
    const std::filesystem::path dir = std::filesystem::path(DirtDir()) / L"saves";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir / ("g" + std::to_string(game_time) + ".txt");
}

// The game's log, new bytes since the last read, on the writer's thread: counts its saves and loads.
void GameLogRead(unsigned long long to) {
    static unsigned long long from = 0;
    static std::string carry;  // a line cut by the end of the last read
    const std::filesystem::path log = std::filesystem::path(DirtDir()).parent_path() / L"game.log.txt";
    FILE* f = _wfopen(log.c_str(), L"rb");
    if (!f) return;
    if (to < from) from = 0, carry.clear();  // a new log (another session)
    std::string text;
    if (_fseeki64(f, static_cast<long long>(from), SEEK_SET) == 0) {
        text.resize(static_cast<size_t>(to - from));
        text.resize(fread(&text[0], 1, text.size(), f));
    }
    fclose(f);
    from += text.size();
    text = carry + text;
    const size_t last = text.rfind('\n');
    carry = last == std::string::npos ? text : text.substr(last + 1);
    if (last == std::string::npos) return;
    for (size_t pos = 0; (pos = text.find("Game save completed", pos)) != std::string::npos && pos < last; ++pos)
        g.log_saves.fetch_add(1);
    for (size_t pos = 0; (pos = text.find("Loading save.", pos)) != std::string::npos && pos < last; ++pos)
        g.log_loads.fetch_add(1);
}

// Writes the snapshot of the dirt as saved now (the store's text) and keeps the newest kSaveSnapshots.
void SnapshotSave() {
    if (!g.state_on || g.dust_forced || !g.game_time) return;
    SaveState("game save");  // the store gets the units' levels now (and dirt_state.txt)
    if (g.saved.ReadOnly()) return;
    g.writer.Post([file = SnapshotFile(g.game_time), text = g.saved.Serialize(),
                   line = Timestamp() + " dirt with the game's save (D13c): game time " + std::to_string(g.game_time) +
                          "\n"] {
        std::string error;
        if (!dirt_state::Store::WriteText(file, text, &error)) return;
        ConfigWrite(line);
        std::vector<std::pair<std::filesystem::file_time_type, std::filesystem::path>> all;
        std::error_code ec;
        for (const auto& e : std::filesystem::directory_iterator(file.parent_path(), ec))
            if (e.path().extension() == L".txt" && e.path().filename().wstring().rfind(L"g", 0) == 0)
                all.push_back({e.last_write_time(ec), e.path()});
        if (all.size() <= static_cast<size_t>(kSaveSnapshots)) return;
        std::sort(all.begin(), all.end());
        for (size_t i = 0; i + kSaveSnapshots < all.size(); ++i) std::filesystem::remove(all[i].second, ec);
    });
}

// After a load: the snapshot of the game time it brought (a minute either way) replaces the saved dirt, and the
// player's units now take theirs.
void SnapshotLoad() {
    std::string text;
    scs_u32_t at = 0;
    for (const int d : {0, -1, 1}) {
        if (FILE* f = _wfopen(SnapshotFile(g.game_time + d).c_str(), L"rb")) {
            char buf[4096];
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
            fclose(f);
            at = g.game_time + d;
            break;
        }
    }
    if (text.empty()) {
        ConfigLog("dirt with the game's save (D13c): no snapshot for game time " + std::to_string(g.game_time) +
                  ": the dirt as it was");
        return;
    }
    dirt_state::Store snap;
    if (snap.Deserialize(text).result != dirt_state::Parse::kOk) {
        ConfigLog("dirt with the game's save (D13c): the snapshot of game time " + std::to_string(at) + " is damaged");
        return;
    }
    g.saved.Deserialize(text);
    g.saved.MarkDirty();
    for (int u = 0; u < dirt::kUnits; ++u) {
        if (g.tracker.KindOf(u) != dirt_state::Kind::kPlayer) continue;
        const dirt_state::Layers* l = g.saved.Find(g.tracker.Key(u));
        DustRestore(u, l ? *l : dirt_state::Layers());
    }
    ConfigLog("dirt with the game's save (D13c): game time " + std::to_string(at) + " taken back, dust truck " +
              std::to_string(g.dust.Level(0)) + " trailer " + std::to_string(g.dust.Level(1)));
    SaveState("game load");
}

// Each frame (game thread): the log's size once a second; a save or a load seen.
void GameSaveStep() {
    const auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration<double>(now - g.log_poll).count() >= 1.0) {
        g.log_poll = now;
        WIN32_FILE_ATTRIBUTE_DATA a;
        const std::filesystem::path log = std::filesystem::path(DirtDir()).parent_path() / L"game.log.txt";
        if (GetFileAttributesExW(log.c_str(), GetFileExInfoStandard, &a)) {
            const unsigned long long size = (static_cast<unsigned long long>(a.nFileSizeHigh) << 32) | a.nFileSizeLow;
            if (size != g.log_posted) {
                g.log_posted = size;
                g.writer.Post([size] { GameLogRead(size); });
            }
        }
    }
    const int loads = g.log_loads.load();
    if (loads != g.seen_loads) {
        g.seen_loads = loads;
        ConfigLog("game's load seen (D13c), game time " + std::to_string(g.game_time));
        g.load_pending = true;
        g.load_time0 = g.game_time;
        g.load_at = now;
    }
    // The load done: its game time in the telemetry (another than before it, or 15 s later) and the rig known.
    if (g.load_pending && g.game_time && !g.rig_changed &&
        (g.game_time != g.load_time0 || std::chrono::duration<double>(now - g.load_at).count() > 15.0)) {
        g.load_pending = false;
        SnapshotLoad();
    }
    const int saves = g.log_saves.load();
    if (saves != g.seen_saves) {
        g.seen_saves = saves;
        ConfigLog("game's save seen (D13c), game time " + std::to_string(g.game_time));
        if (!g.load_pending) SnapshotSave();  // (the game also saves its profile while loading)
    }
}

// D11n: what the writer's thread could not save, said on the game's thread (the SDK's log).
void SaveFailures() {
    if (g.state_failed.exchange(false)) {
        g.saved.MarkDirty();  // tried again at the next save
        std::string error;
        {
            std::lock_guard<std::mutex> lock(g.failed_mutex);
            error = g.state_error;
        }
        if (!g.state_error_logged) {
            g.state_error_logged = true;
            Log(SCS_LOG_TYPE_warning, "dirt_state.txt not saved: " + error);
        }
    }
    if (g.spots_failed.exchange(false)) Log(SCS_LOG_TYPE_warning, "wash_spots.txt not saved");
    if (g.map_failed.exchange(false)) {  // N47: written again when the tracker has a new one
        Log(SCS_LOG_TYPE_warning, "a rig map (TruckDirt\\rigs) not saved");
        g.map_written[0] = g.map_written[1] = -1;
    }
}

SCSAPI_VOID OnPause(const scs_event_t ev, const void*, const scs_context_t) {
    g.paused = (ev == SCS_TELEMETRY_EVENT_paused);
    if (g.paused) SaveState("pause");  // the menu: the player may quit from here
}

bool g_last_connected = false;

// D3/D4: the unit's ground classes from its wheels' substances, and the speed.
void DustDrive(int unit, const Unit& u, double km) {
    dirt::Ground ground[kMaxWheels];
    for (int w = 0; w < u.wheel_count; ++w) ground[w] = dirt::GroundOf(SubstanceName(u.substance[w]));
    g.dust.Drive(unit, km, ground, u.on_ground, u.wheel_count, std::fabs(g.speed_ms) * 3.6f);
}

// D5: after a truck, trailer or job change, every unit that came in gets its
// saved dirt (the player's), every layer, or starts clean (the company's, none).
void DustSync() {
    g.rig_changed = false;
    const dirt_state::Rig rig{g.truck_id, g.truck_plate, g.trailer_id, g.trailer_plate, g.job_market};
    dirt_state::Layers cur[dirt::kUnits], start[dirt::kUnits];
    DustLayers(cur);
    const unsigned changed = g.tracker.Sync(rig, cur, start);
    for (int u = 0; u < dirt::kUnits; ++u) {
        if (!(changed & (1u << u))) continue;
        DustRestore(u, start[u]);
        ConfigLog(std::string("dirt_state ") + (u ? "trailer " : "truck ") + g.tracker.Key(u) + " -> " +
                  (g.tracker.KindOf(u) == dirt_state::Kind::kPlayer    ? "player's"
                   : g.tracker.KindOf(u) == dirt_state::Kind::kCompany ? "company's (clean)"
                                                                       : "none") +
                  ", dust " + std::to_string(g.dust.Level(u)));
    }
    if (changed) SaveState("rig change");
}

// N5: a unit's D4 layers as the dust pass takes them (dust_shader.h slots), with the road wetness.
dust::Layers ShownLayers(int unit) {
    static_assert(dirt::kSalt - dirt::kWetMud == dust::kSalt - dust::kWetMud, "layer slots");
    dust::Layers out = {};
    const dirt::Coat c = g.dust.Layers(unit);
    for (int k = dirt::kWetMud; k <= dirt::kSalt; ++k) {
        out.body[k - dirt::kWetMud] = c.body[k];
        out.front[k - dirt::kWetMud] = c.front[k];
        out.reach[k - dirt::kWetMud] = c.reach[k];
    }
    out.body[dust::kWet] = g.dust.Wetness();
    out.body[dust::kTread] = g.dust.TreadWear(unit);  // N31
    out.body[dust::kSoot] = c.body[dirt::kSoot];       // N39
    out.front[dust::kSoot] = c.front[dirt::kSoot];
    out.reach[dust::kSoot] = c.reach[dirt::kSoot];
    return out;
}

// D3: distance and game time since the last frame, the ground under the
bool HotkeyPressed(const panel::Hotkey& h, bool* was_down);  // N65: the washer's key, defined with the others

// wheels and the rain move the dust levels; D5: saved every few minutes.
void DustStep() {
    constexpr double kSaveIntervalSec = 120.0;
    if (g.rig_changed) DustSync();
    if (!g.paused) {
        const float odo = g.odometer_km;
        if (g.last_odometer >= 0.f) {
            const double km = double(odo) - double(g.last_odometer);
            DustDrive(0, g.truck, km);
            if (g.trailer_connected && g.trailer.wheel_count) DustDrive(1, g.trailer, km);
        }
        g.last_odometer = odo;
        if (g.last_game_time && g.game_time > g.last_game_time)
            g.dust.Weather(double(g.game_time - g.last_game_time), gpu_probe::RainActive() || g.wipers);
        g.last_game_time = g.game_time;
        // N64: the blades sweep in real time (a sweep is about a second), not in the game's clock, which runs many
        // times faster -- in game minutes a wipe would always be instant.
        const auto now = std::chrono::steady_clock::now();
        const double wipe_dt = std::chrono::duration<double>(now - g.last_wipe).count();
        g.last_wipe = now;
        g.dust.Wipers(wipe_dt, g.wipers);
        // N65: the washer jet. The SDK has no washer channel (only truck.wipers, on or off), so it is a key of
        // ours. Inside the arc it takes everything; the rest of the pane stays dirty until the truck is washed.
        if (g.washer_key.vk && HotkeyPressed(g.washer_key, &g.washer_key_down) && !g.menu.Open()) {
            g.dust.Washer();
            ConfigLog("[glass] washer: the jet cleaned the arc the wipers sweep (N65)");
            SaveState("washer");
        }
        if (std::chrono::duration<double>(std::chrono::steady_clock::now() - g.last_save).count() >= kSaveIntervalSec)
            SaveState("timer");
    }
    // A trailer keeps its dirt while detached (the player's own stays dirty; a company's or a new one
    // starts clean through DustSync); no trailer config = nothing to draw.
    const bool trailer = g.trailer.wheel_count > 0;
    // N40e: how much light there is for the dust on the glass to scatter, from the game's clock (minutes): full
    // between 8h and 17h, nothing between 21h and 5h, the hour in between a ramp. Dust on glass is only seen
    // because it scatters light; at night a veil of its own would glow on its own.
    {
        const float h = float(g.game_time % 1440u) / 60.f;
        const float day = h <= 5.f || h >= 21.f ? 0.1f
                          : h < 8.f             ? 0.1f + 0.9f * (h - 5.f) / 3.f
                          : h <= 17.f           ? 1.f
                                                : 0.1f + 0.9f * (21.f - h) / 4.f;
        tint::SetDaylight(day);
    }
    // N64/N64c/N65: what the blades and the jet have taken inside the arc, and where the blade is right now.
    tint::SetWipe(g.dust.Wipe(), g.dust.WipeBefore(), g.dust.Phase(), g.dust.Dir(), g.dust.Jet());
    if (g.panel.Enabled()) {  // N64h: the arc as the panel has it, so it can be lined up without a restart
        const float* a = g.panel.Arc();
        tint::SetWiperArc(a[0], a[1], a[2], a[3] * 90.f * 3.14159265f / 180.f);
        tint::SetGlassBright(a[4] * panel::kGlassBrightMax);  // N64k
    }
    gpu_probe::SetDust(g.dust.Level(0), trailer ? g.dust.Level(1) : 0.f);
    gpu_probe::SetLayers(ShownLayers(0), trailer ? ShownLayers(1) : dust::Layers{});
    // N57: the load on the trailer, clean when picked up (a job's cargo on the trailer's cargo accessory).
    float cargo_level = 0.f;
    dust::Layers cargo_layers = {};
    if (trailer) {
        const std::string key = g.job_cargo.empty() || g.trailer_cargo_acc.empty() ? std::string()
                                                                                   : g.job_cargo + "|" + g.trailer_cargo_acc;
        if (cargo::Step(&g.cargo, key, g.dust.Level(1), ShownLayers(1), &cargo_level, &cargo_layers)) {
            g.cargo_dirty = true;
            ConfigLog("cargo (N57): " + key + " picked up: clean (the trailer's dust " + std::to_string(g.dust.Level(1)) +
                      " its base)");
        }
    }
    gpu_probe::SetCargo(cargo_level, cargo_layers);
}

// N10: the panel's keys, only while the game's window has the focus (polled each frame, so no
// window hook; the game sees the keys too: see the development notes), and what it shows.
bool GameHasFocus() {
    DWORD pid = 0;
    const HWND w = GetForegroundWindow();
    return w && GetWindowThreadProcessId(w, &pid) && pid == GetCurrentProcessId();
}

// L12 / V6: while the menu is open every key is kept from the game, while the panel is open only its keys, by
// the keyboard hook ([menu] block_input = 1).
void InputStep() {
    if (!g.block_input) return;
    input_hook::Mode m = input_hook::Mode::kOff;
    std::vector<panel::Hotkey> combos;
    if (g.menu.Open()) {
        m = input_hook::Mode::kAll;
    } else if (g.panel.Enabled() && g.panel.Open()) {
        m = input_hook::Mode::kCombos;
        combos.push_back(g.panel_key);
        for (const int vk : panel::kNavKeys) combos.push_back({vk, true, false, false});
        combos.push_back({panel::kVkLeft, true, true, false});
        combos.push_back({panel::kVkRight, true, true, false});
    }
    if (!input_hook::SetMode(m, combos)) {
        g.block_input = false;
        Log(SCS_LOG_TYPE_warning, "keyboard hook not installed: the panel's and the menu's keys reach the game too");
    }
    if (m == input_hook::Mode::kCombos) input_hook::Drain();  // the panel reads the held state (input_hook::Held)
}

void PanelStep() {
    if (!g.panel.Enabled()) return;
    const bool focus = GameHasFocus() && !g.menu.Open();  // the menu takes the keys while open
    // A key the hook swallowed may be missing from the async state: its held state comes from the hook.
    auto down = [focus](int vk) { return focus && ((GetAsyncKeyState(vk) & 0x8000) != 0 || input_hook::Held(vk)); };
    const bool ctrl = down(VK_CONTROL), shift = down(VK_SHIFT), alt = down(VK_MENU);
    const uint64_t now = GetTickCount64();
    dirt::Coat shown[dirt::kUnits] = {g.dust.Layers(0), g.dust.Layers(1)};
    bool changed = false;
    const bool key = down(g.panel_key.vk);
    if (key && !g.panel_key_down && panel::IsHotkey(g.panel_key, g.panel_key.vk, ctrl, shift, alt)) {
        g.panel.Press(panel::kToggle, shown);
        ConfigLog(std::string("test panel ") + (g.panel.Open() ? "opened" : "closed") +
                  (g.panel.Forced().Any() ? " (forced levels on)" : ""));
    }
    g.panel_key_down = key;
    for (int i = 0; i < 8; ++i) {
        const int vk = panel::kNavKeys[i];
        const int presses = g.panel_repeat[i].Step(g.panel.Open() && down(vk), now);
        const panel::Key k = panel::KeyOf(vk, ctrl, shift, alt);
        for (int n = 0; n < presses && k != panel::kNone; ++n) {
            if (g.panel.Press(k, shown)) {
                g.dust.Force(g.panel.Forced());
                shown[0] = g.dust.Layers(0), shown[1] = g.dust.Layers(1);
                changed = true;
            }
        }
    }
    if (changed && !g.panel.Forced().Any()) ConfigLog("test panel: every level real again");
    panel::View view = g.panel.MakeView(shown);
    gpu_probe::SetPanel(view);
}

// ---- D5 (L7): wash at the service station ----

std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string out(n > 0 ? n : 0, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), &out[0], n, nullptr, nullptr);
    return out;
}

std::string IniString(const std::wstring& ini, const wchar_t* section, const wchar_t* key, const wchar_t* def) {
    wchar_t b[256] = {};
    GetPrivateProfileStringW(section, key, def, b, 256, ini.c_str());
    return ToUtf8(b);
}

std::string ReadFileBytes(const std::filesystem::path& p, bool* found) {
    *found = false;
    FILE* f = _wfopen(p.c_str(), L"rb");
    if (!f) return std::string();
    *found = true;
    std::string out;
    char b[4096];
    for (size_t n; (n = fread(b, 1, sizeof(b), f)) > 0;) out.append(b, n);
    fclose(f);
    return out;
}

// Atomic: a temporary file, then MoveFileEx over the old one.
bool WriteFileAtomic(const std::filesystem::path& p, const std::string& bytes) {
    const std::wstring tmp = p.wstring() + L".tmp";
    FILE* f = _wfopen(tmp.c_str(), L"wb");
    if (!f) return false;
    // D11o: on the disk itself before the rename (dirt_state::Store::WriteText says why).
    const bool ok = fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size() && fflush(f) == 0 && _commit(_fileno(f)) == 0;
    if (fclose(f) != 0 || !ok || !MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

void SpotsLoad() {
    bool found = false;
    const std::string bytes = ReadFileBytes(g.spots_file, &found);
    std::vector<std::string> errors;
    if (found && !g.spots.Parse(bytes, &errors) && !bytes.empty()) g.spots_keep = true;
    std::string text = "wash: " + std::to_string(g.spots.All().size()) + " service station(s) known" +
                       (found ? "" : " (no wash_spots.txt yet)");
    if (g.spots_keep) text += " - wash_spots.txt unreadable, kept as wash_spots.txt.bad";
    Log(errors.empty() ? SCS_LOG_TYPE_message : SCS_LOG_TYPE_warning, text);
    for (const std::string& e : errors) ConfigLog("wash_spots.txt: " + e);
}

void SpotsSave() {
    // D11n: on the writer's thread.
    g.writer.Post([keep = g.spots_keep, text = g.spots.Serialize()] {
        if (keep) CopyFileW(g.spots_file.c_str(), (g.spots_file.wstring() + L".bad").c_str(), FALSE);
        if (!WriteFileAtomic(g.spots_file, text)) g.spots_failed.store(true);
    });
    g.spots_keep = false;
}

// A hotkey's press this frame: its key went down with exactly its modifiers, the game's window focused.
bool HotkeyPressed(const panel::Hotkey& h, bool* was_down) {
    const bool focus = GameHasFocus();
    auto down = [focus](int vk) { return focus && (GetAsyncKeyState(vk) & 0x8000) != 0; };
    const bool key = h.vk && down(h.vk);
    const bool pressed = key && !*was_down && panel::IsHotkey(h, h.vk, down(VK_CONTROL), down(VK_SHIFT), down(VK_MENU));
    *was_down = key;
    return pressed;
}

// The dirtiest layer the player sees on the units the wash takes (the panel's forced levels too).
float WashDirt(bool trailer) {
    float m = 0.f;
    for (int u = 0; u < (trailer ? 2 : 1); ++u) {
        const dirt::Coat c = g.dust.Layers(u);
        for (int k = 0; k < dirt::kLayers; ++k) m = (std::max)({m, c.body[k], c.front[k]});
    }
    return m;
}

// Under the black screen: the units go clean (the trailer's dirt is every trailer's of a road train) and are
// saved; the panel's forced levels go too (it shows the washed rig).
void WashClean(unsigned units) {
    for (int u = 0; u < dirt::kUnits; ++u)
        if (units & (1u << u)) g.dust.Reset(u);
    if (g.panel.Forced().Any()) {
        g.panel.ClearForced();
        g.dust.Force(g.panel.Forced());
    }
    ConfigLog(std::string("wash: ") + ((units & 2u) ? "truck and trailers" : "truck") + " cleaned");
    SaveState("wash");
}

// D11c: the text inside the last brackets of `s` ("(free)", "（免费）"), without them; "" = none.
std::string Bracketed(const std::string& s) {
    const char* const kOpen[] = {"(", "\xEF\xBC\x88"}, *const kClose[] = {")", "\xEF\xBC\x89"};
    for (int k = 0; k < 2; ++k) {
        const size_t a = s.rfind(kOpen[k]);
        const size_t b = a == std::string::npos ? a : s.find(kClose[k], a);
        if (b != std::string::npos) return s.substr(a + strlen(kOpen[k]), b - a - strlen(kOpen[k]));
    }
    return std::string();
}

std::string WashText(const wash::View& v) {
    const std::string key = panel::HotkeyLabel(g.wash_key);
    if (v.message == wash::Message::kHint && v.price == 0) return g.text.Format("wash.hint_free", {{"key", key}});
    return g.text.Format(std::string("wash.") + wash::MessageName(v.message),
                         {{"key", key}, {"price", g.text.Price(v.display_price, g.currency)}});
}

void WashStep() {
    if (!g.wash_on) return;
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - g.t0).count();
    const auto& p = g.truck_placement.position;
    const wash::Spot at{p.x, p.y, p.z};
    const float kmh = g.speed_ms * 3.6f;
    wash::RefuelInput r;
    r.now_s = now, r.paused = g.paused, r.at = at, r.speed_kmh = kmh, r.parking_brake = g.parking_brake;
    r.fuel_l = g.fuel_l;
    if (g.spots.Update(r)) {
        char b[160];
        snprintf(b, sizeof(b), "wash: service station learnt at a refuel (%.1f %.1f %.1f), %d known", p.x, p.y, p.z,
                 static_cast<int>(g.spots.All().size()));
        ConfigLog(b);
        Log(SCS_LOG_TYPE_message, b);
        SpotsSave();
    }
    // N36: the game offers a refuel here (its green spot at the pump): a station, known or not. The spot is
    // learnt too once stopped there, so the wash stays offered if the signal is lost after a game update.
    const int activation = station::Activation(g.station);
    if (activation != g.activation) {
        if (g.station.ok)
            ConfigLog("game activation: " + std::to_string(activation) +
                      (activation == g.station.refuel_type ? " (refuel offered)" : activation == 0 ? " (none)" : ""));
        g.activation = activation;
    }
    const bool at_pump = g.station.ok && activation == g.station.refuel_type;
    if (at_pump && std::fabs(kmh) < g.spots.Config().stop_kmh && !g.paused && g.spots.Add(at)) {
        char b[160];
        snprintf(b, sizeof(b), "wash: service station learnt from the game's refuel offer (%.1f %.1f %.1f), %d known",
                 p.x, p.y, p.z, static_cast<int>(g.spots.All().size()));
        ConfigLog(b);
        Log(SCS_LOG_TYPE_message, b);
        SpotsSave();
    }
    wash::Input in;
    // D11f (the user, 29/10: the prompt came "well before the pump", it must show only on the green refuel spot):
    // with the game's offer found, only there; the learnt stations (25 m around a refuel) when it is not.
    in.now_s = now, in.paused = g.paused, in.in_area = g.station.ok ? at_pump : g.spots.Near(at), in.speed_kmh = kmh;
    in.parking_brake = g.parking_brake;
    in.trailer = g.trailer_connected && g.trailer.wheel_count > 0;
    in.dirt = WashDirt(in.trailer);
    in.key = HotkeyPressed(g.wash_key, &g.wash_key_down) && !g.paused && !g.menu.Open();
    // D11f: away from the green spot the key says nothing when the game's offer is known (it is no unknown station;
    // and other mods' F8 on foot).
    if (g.station.ok && !in.in_area && !g.wash.Busy()) in.key = false;
    if (g.charge && in.in_area) {
        const int64_t bank = payment::Balance(g.pay);
        in.balance_known = bank >= 0;
        in.balance = bank;
    }
    const wash::Step step = g.wash.Update(in);
    if (step.charge) {
        const int64_t before = payment::Balance(g.pay);
        const payment::Result res = g.charge ? payment::Take(g.pay, step.amount) : payment::Result::kUnavailable;
        const int64_t after = payment::Balance(g.pay);
        g.wash.Charged(res == payment::Result::kTaken);
        ConfigLog("wash: charge " + std::to_string(step.amount) + " " +
                  (res == payment::Result::kTaken     ? "taken"
                   : res == payment::Result::kRefused ? "refused by the game"
                                                      : "not called") +
                  ", bank " + std::to_string(before) + " -> " + std::to_string(after));
    }
    if (step.clean) WashClean(step.clean);
    const wash::View v = g.wash.Current();
    if (v.phase != g.wash_phase) {
        if (g.wash_phase == wash::Phase::kIdle) ConfigLog(std::string("wash: started, ") + (in.trailer ? "with" : "no") + " trailer");
        if (v.phase == wash::Phase::kIdle) ConfigLog(std::string("wash: ended (") + wash::MessageName(v.message) + ")");
        g.wash_phase = v.phase;
    }
    overlay::Wash& o = g.overlay_wash;
    o = overlay::Wash();
    o.darkness = v.darkness;
    o.washing = v.show > 0.f;
    o.show = v.show;
    o.washing_text = g.text.Get("wash.washing");
    if (o.washing)
        o.washing_detail = v.paid == 0 ? Bracketed(g.text.Get("wash.hint_free")) : g.text.Price(v.paid_display, g.currency);
    o.y = g.hint_y;
    if (v.message == wash::Message::kHint) {
        // D11c: the prompt: the key on its keycap, the action, the price (or the word for free).
        o.key = panel::HotkeyLabel(g.wash_key);
        o.message = g.text.Get("wash.prompt");
        o.detail = v.price == 0 ? Bracketed(g.text.Get("wash.hint_free")) : g.text.Price(v.display_price, g.currency);
    } else if (v.message != wash::Message::kNone) {
        o.message = WashText(v);
    }
}

// ---- V6: the keys menu ----

void MenuApply(const menu::Write& w) {
    WritePrivateProfileStringW(std::wstring(w.section.begin(), w.section.end()).c_str(),
                               std::wstring(w.key.begin(), w.key.end()).c_str(),
                               std::wstring(w.value.begin(), w.value.end()).c_str(), g.ini.c_str());
    ConfigLog("keys menu: [" + w.section + "] " + w.key + " = " + (w.value.empty() ? "(none)" : w.value));
    panel::Hotkey h;
    const bool ok = !w.value.empty() && panel::ParseHotkey(w.value, &h);
    if (w.section == "wash") {
        g.wash_key = ok ? h : panel::Hotkey();
    } else if (w.section == "test") {
        g.panel_key = ok ? h : panel::Hotkey();
        g.panel.Enable(g.panel.Enabled(), panel::HotkeyName(g.panel_key));
    }
}

void MenuStep() {
    if (!g.menu_on) return;
    const uint64_t now = GetTickCount64();
    const bool focus = GameHasFocus();
    auto async = [focus](int vk) { return focus && (GetAsyncKeyState(vk) & 0x8000) != 0; };
    if (!g.menu.Open()) {
        if (HotkeyPressed(g.menu.MenuKey(), &g.menu_key_down) && !g.wash.Busy()) {
            g.menu.Toggle();
            g.menu_last_key = now;
            for (int vk = 0; vk < 256; ++vk) g.menu_down[vk] = async(vk);  // the opening keys are not presses
            g.menu_leaks = 0;
            // The hook goes in after this step (InputStep): block_input says which way the keys will come.
            ConfigLog(std::string("keys menu opened (") + (g.block_input ? "keyboard hook" : "keys polled") + ")");
        }
        return;
    }
    const bool hooked = input_hook::Active();
    std::vector<menu::Write> writes;
    auto press = [&](int vk, bool ctrl, bool shift, bool alt) {
        if (g.menu.Key(vk, ctrl, shift, alt, hooked, &writes)) g.menu_last_key = now;
    };
    if (hooked) {
        // The hook's events; a held key's repeats act only for Up/Down.
        for (const input_hook::Event& e : input_hook::Drain()) {
            const bool repeat = e.down && g.menu_down[e.vk];
            g.menu_down[e.vk] = e.down;
            if (e.down && (!repeat || e.vk == VK_UP || e.vk == VK_DOWN)) press(e.vk, e.ctrl, e.shift, e.alt);
        }
        // A release the game got (its press came before the hook): from the async state.
        for (int vk = 0; vk < 256; ++vk)
            if (g.menu_down[vk] && !async(vk) && !input_hook::Held(vk)) g.menu_down[vk] = false;
        // D11p (the user, 30/10, ETS2: the menu open, "stuck", no key changed nor the menu closed, Esc pausing the game):
        // a key the hook should have kept reached the game instead (another program's keyboard hook ahead of ours, or
        // Windows dropped ours), so its events never come. Twice: the hook goes and the keys are polled from then on
        // (with Ctrl, as without the hook), this press included.
        const bool ctrl = async(VK_CONTROL), shift = async(VK_SHIFT), alt = async(VK_MENU);
        for (int vk = 0x08; vk < 0xFF; ++vk) {
            if (!async(vk) || input_hook::Held(vk) || g.menu_down[vk] || !menu::HookKeeps(vk, ctrl, alt)) continue;
            g.menu_down[vk] = true;
            if (++g.menu_leaks < 2) continue;
            g.block_input = false;
            input_hook::SetMode(input_hook::Mode::kOff);
            Log(SCS_LOG_TYPE_warning, "keys menu: the keyboard hook let the keys through to the game (another program's "
                                      "hook ahead of it?): the menu reads them itself now (Ctrl+arrows, Ctrl+Enter, "
                                      "Ctrl+F8 closes); the game gets them too");
            ConfigLog("keys menu: keyboard hook not getting the keys (" + std::to_string(g.menu_leaks) +
                      " reached the game): keys polled from now on");
            press(vk, ctrl, shift, alt);
            break;
        }
    } else {
        const bool ctrl = async(VK_CONTROL), shift = async(VK_SHIFT), alt = async(VK_MENU);
        for (int vk = 0x08; vk < 0xFF && g.menu.Open(); ++vk) {
            const bool down = async(vk);
            if (down && !g.menu_down[vk]) press(vk, ctrl, shift, alt);
            g.menu_down[vk] = down;
        }
    }
    for (const menu::Write& w : writes) MenuApply(w);
    if (g.menu.Open() && now - g.menu_last_key > 120000) g.menu.Close();  // forgotten open [estimate: 2 min]
    if (!g.menu.Open()) {
        g.menu_key_down = true;  // the closing press is not an opening one
        ConfigLog("keys menu closed");
    }
}

void MenuConfigure(const std::wstring& ini) {
    g.ini = ini;
    panel::Hotkey key;
    const std::string text = IniString(ini, L"menu", L"key", L"shift+f7");
    if (!panel::ParseHotkey(text, &key)) {
        Log(SCS_LOG_TYPE_warning, "[menu] key = " + text + " not understood: shift+f7");
        panel::ParseHotkey("shift+f7", &key);
    }
    g.menu.SetMenuKey(key);
    for (int i = 0; i < menu::kItems; ++i) {
        const menu::Item& it = menu::kItemList[i];
        g.menu.Set(i, IniString(ini, std::wstring(it.section, it.section + strlen(it.section)).c_str(),
                                std::wstring(it.key, it.key + strlen(it.key)).c_str(), L""));
    }
    g.menu_on = true;
    Log(SCS_LOG_TYPE_message, "keys menu: " + panel::HotkeyName(key) + " opens it" +
                                  (g.block_input ? " (the game's keys held back while open)" : ""));
}

// What the player sees over the game this frame.
void OverlayStep() {
    overlay::View o;
    o.wash = g.overlay_wash;
    o.menu = g.menu.MakeView(g.text, input_hook::Active());
    gpu_probe::SetOverlay(o);
}

// ---- D10 (N9): the texts in the game's language ----

// A language's file: TruckDirt\lang\<code>.txt when the player put one there (a corrected translation), else the
// copy built into the DLL.
lang::Table LoadLanguage(const std::string& code, std::string* from) {
    lang::Table t;
    std::vector<std::string> errors;
    bool found = false;
    const std::string bytes = ReadFileBytes(std::filesystem::path(DirtDir()) / L"lang" / (code + ".txt"), &found);
    if (found) {
        lang::Parse(bytes, &t, &errors);
        *from = "TruckDirt\\lang\\" + code + ".txt";
    } else {
        lang::Parse(lang::Embedded(code), &t, &errors);
        *from = "built in";
    }
    for (const std::string& e : errors) ConfigLog("lang " + code + ": " + e);
    return t;
}

// [lang] code = auto (the default): the game's own choice, from its game.log.txt (the profile's g_lang), else
// its config.cfg; any code of lang/ forces one. A language the overlay's font cannot show falls back to English.
void LoadTexts(const std::wstring& ini) {
    std::string code = IniString(ini, L"lang", L"code", L"auto"), source = "[lang] code";
    if (code.empty() || code == "auto") {
        const std::filesystem::path game_dir = std::filesystem::path(DirtDir()).parent_path();  // Documents\<game>
        bool found = false;
        code = lang::FromGameLog(ReadFileBytes(game_dir / L"game.log.txt", &found));
        source = "the game's log";
        if (code.empty()) {
            code = lang::FromConfig(ReadFileBytes(game_dir / L"config.cfg", &found));
            source = "the game's config.cfg";
        }
        if (code.empty()) source = "not found";
    }
    std::vector<std::string> available = lang::EmbeddedCodes();
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(std::filesystem::path(DirtDir()) / L"lang", ec))
        if (e.path().extension() == L".txt") available.push_back(ToUtf8(e.path().stem().wstring()));
    std::string pick = lang::Pick(code, available), from, why;
    lang::Table table = LoadLanguage(pick, &from);
    if (!lang::FontCanShow(table)) {
        why = " (its " + table.Get("_script") + " script is not in the overlay's font)";
    } else {
        const std::vector<char32_t> missing = panel_draw::MissingGlyphs(lang::Glyphs(table));
        if (!missing.empty()) {
            char b[16];
            why = " (" + std::to_string(missing.size()) + " letter(s) not in the system font, the first U+";
            snprintf(b, sizeof(b), "%04X", static_cast<unsigned>(missing[0]));
            why += std::string(b) + ")";
        }
    }
    std::string en_from;
    lang::Table en = LoadLanguage(lang::kFallback, &en_from);
    std::string line = "texts: " + (code.empty() ? std::string("-") : code) + " from " + source;
    if (!why.empty()) {
        line += ", " + pick + why + ": English";
        table = en;
        pick = lang::kFallback;
        from = en_from;
    } else if (pick != code) {
        line += ", shown in " + pick;
    }
    Log(why.empty() ? SCS_LOG_TYPE_message : SCS_LOG_TYPE_warning, line + " (" + from + ")");
    g.text = lang::Catalog(table, en);
}

// [wash] of the INI; true when the wash is on.
bool WashConfigure(const std::wstring& ini) {
    if (GetPrivateProfileIntW(L"wash", L"enabled", 1, ini.c_str()) != 1) {
        Log(SCS_LOG_TYPE_message, "wash: off ([wash] enabled = 0)");
        return false;
    }
    wash::Settings ws;
    ws.charge = false;
    for (const wchar_t* key : {L"price_truck", L"price_trailer", L"charge", L"darken_s", L"show_s", L"lighten_s",
                               L"message_s", L"min_dirt", L"display_rate"}) {
        const std::string value = IniString(ini, L"wash", key, L"");
        std::string error;
        if (!value.empty() && !wash::SetOption(&ws, ToUtf8(key), value, &error))
            Log(SCS_LOG_TYPE_warning, "[wash] " + error + ": the default stays");
    }
    // D11c: where the prompt sits (its bottom edge, 0..1 of the screen's height from the top).
    const std::string hint_y = IniString(ini, L"wash", L"hint_y", L"");
    if (!hint_y.empty()) {
        const double v = atof(hint_y.c_str());
        if (v >= 0.2 && v <= 0.98) g.hint_y = static_cast<float>(v);
        else Log(SCS_LOG_TYPE_warning, "[wash] hint_y = " + hint_y + ": expected 0.2 to 0.98, 0.82 stays");
    }
    wash::SpotSettings ss;
    const std::string radius = IniString(ini, L"wash", L"radius_m", L"");
    if (!radius.empty()) {
        const double v = atof(radius.c_str());
        if (v >= 5 && v <= 200) ss.radius_m = v;
        else Log(SCS_LOG_TYPE_warning, "[wash] radius_m = " + radius + ": expected 5 to 200, 25 stays");
    }
    g.spots.Configure(ss);
    // N36: the game's own refuel offer marks a station (no refuel needed first); 0 = only the learnt ones.
    if (GetPrivateProfileIntW(L"wash", L"game_station", 1, ini.c_str()) != 0) {
        g.station = station::Locate();
        Log(g.station.ok ? SCS_LOG_TYPE_message : SCS_LOG_TYPE_warning,
            std::string("wash: the game's refuel offer ") + (g.station.ok ? "found: " : "not found, only learnt stations: ") +
                g.station.detail);
    } else {
        Log(SCS_LOG_TYPE_message, "wash: the game's refuel offer not used ([wash] game_station = 0)");
    }
    const std::string key = IniString(ini, L"wash", L"key", L"ctrl+f7");
    if (!panel::ParseHotkey(key, &g.wash_key)) {
        Log(SCS_LOG_TYPE_warning, "[wash] key = " + key + " not understood: ctrl+f7");
        panel::ParseHotkey("ctrl+f7", &g.wash_key);
    }
    // The game's payment: the tire plugin's approach (payment.h). Off by default: only one test call ever
    // (EUR 1 in ETS2), none in ATS, the charge after a save and reload not checked.
    const bool asked = ws.charge;
    std::string charge_line = "charging off ([wash] charge = 0; the game's payment is not proven in ATS nor across a "
                              "save reload): the wash is free";
    if (ws.charge) {
        g.pay = payment::Locate();
        g.charge = g.pay.ok;
        charge_line = g.charge ? "charging on (game's payment: " + g.pay.detail + ")"
                               : "charging on but the game's payment was not found (" + g.pay.detail + "): the wash is free";
        ws.charge = g.charge;
    }
    g.wash.Configure(ws);
    g.currency = g.ats ? "$" : "\xE2\x82\xAC";
    g.spots_file = std::filesystem::path(DirtDir()) / L"wash_spots.txt";
    SpotsLoad();
    char b[160];
    snprintf(b, sizeof(b), "wash: on, %s at a known station, price %d / %d with a trailer, ",
             panel::HotkeyName(g.wash_key).c_str(), ws.price_truck, ws.price_trailer);
    Log(asked && !g.charge ? SCS_LOG_TYPE_warning : SCS_LOG_TYPE_message, b + charge_line);
    return true;
}

void CopyWheels(const Unit& u, gpu_probe::PoseWheels* w) {
    w->count = (std::min)(u.wheel_count, gpu_probe::kPoseWheels);
    for (int i = 0; i < w->count; ++i) {
        w->rotation[i] = u.rotation[i];
        w->steering[i] = u.steering[i];
        w->deflection[i] = u.deflection[i];
        w->lift[i] = u.lift[i];
    }
}

void CopyPlacement(const scs_value_dplacement_t& p, double* out) {
    out[0] = p.position.x, out[1] = p.position.y, out[2] = p.position.z;
    out[3] = p.orientation.heading, out[4] = p.orientation.pitch, out[5] = p.orientation.roll;
}

SCSAPI_VOID OnFrameEnd(const scs_event_t, const void*, const scs_context_t) {
    gpu_probe::SetTelemetry(g.wipers, g.speed_ms * 3.6f, g.paused);
    if (g.gpu_started) {
        gpu_probe::Pose pose;
        CopyPlacement(g.truck_placement, pose.truck);
        CopyPlacement(g.trailer_placement, pose.trailer);
        pose.trailer_connected = g.trailer_connected;
        const auto& c = g.cabin_offset;
        pose.cabin[0] = c.position.x, pose.cabin[1] = c.position.y, pose.cabin[2] = c.position.z;
        pose.cabin[3] = c.orientation.heading, pose.cabin[4] = c.orientation.pitch, pose.cabin[5] = c.orientation.roll;
        pose.speed_kmh = g.speed_ms * 3.6f;
        CopyWheels(g.truck, &pose.truck_wheels);
        CopyWheels(g.trailer, &pose.trailer_wheels);
        for (int i = 0; i < gpu_probe::kPoseMoreTrailers; ++i) {
            CopyPlacement(g.more_placement[i], pose.more[i].place);
            pose.more[i].connected = g.more_connected[i];
            CopyWheels(g.more[i], &pose.more[i].wheels);
        }
        gpu_probe::SetPose(pose);
        rig::SetPlacement(0, pose.truck, true);
        rig::SetPlacement(1, pose.trailer, g.trailer.wheel_count > 0);
        rig::SetDeflection(0, g.truck.deflection, g.truck.wheel_count);
        rig::SetDeflection(1, g.trailer.deflection, g.trailer.wheel_count);
        rig::SetNext(1, g.next_hook, pose.more[0].place,
                     g.next_has_hook && g.more_connected[0] && g.more[0].wheel_count > 0);  // N24
        // N21: trailer.1-4 tracked as units 2-5 while coupled (a layout dropped when uncoupled: no dirt on a
        // trailer left behind).
        for (int i = 0; i < gpu_probe::kPoseMoreTrailers && 2 + i < rig::kUnits; ++i) {
            const bool on = g.more_connected[i] && g.more[i].wheel_count > 0 && g.trailer_connected;
            rig::SetUnit(2 + i, on ? g.more_cfg[i] : rig::UnitConfig());
            rig::SetPlacement(2 + i, pose.more[i].place, on);
            rig::SetDeflection(2 + i, g.more[i].deflection, on ? g.more[i].wheel_count : 0);
        }
        const float cab_off[6] = {c.position.x, c.position.y, c.position.z,
                                  c.orientation.heading, c.orientation.pitch, c.orientation.roll};
        rig::SetCabinOffset(cab_off);
        DustStep();
        InputStep();  // L12, V6
        MenuStep();   // V6
        PanelStep();
        WashStep();   // D5 (L7)
        OverlayStep();
    }
    // V4: each map trigger action with the truck's state then, to learn the service station's.
    if (g.trigger_log) {
        for (const trigger_probe::Action& a : trigger_probe::Drain()) {
            const auto& p = g.truck_placement.position;
            char hex[trigger_probe::kBytes * 2 + 1] = {};
            for (int i = 0; i < trigger_probe::kBytes; ++i) snprintf(hex + i * 2, 3, "%02X", a.bytes[i]);
            char b[512];
            snprintf(b, sizeof(b), "trigger: action type %d, truck at (%.1f %.1f %.1f), %.1f km/h, parking brake %d, engine %d, bytes %s",
                     a.type, p.x, p.y, p.z, g.speed_ms * 3.6f, g.parking_brake ? 1 : 0, g.engine ? 1 : 0, hex);
            ConfigLog(b);
        }
    }
    if (g.trailer_connected != g_last_connected) {
        g_last_connected = g.trailer_connected;
        LogOwnership(g.trailer_connected ? "trailer attached" : "trailer detached");
    }
    const auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration<double>(now - g.last_row).count() >= kRowIntervalSec) {
        g.last_row = now;
        WriteRow();
    }
    GameSaveStep();  // D13c
    SaveFailures();
    Flush();
}

template <typename T>
void RegisterChannel(scs_telemetry_register_for_channel_t reg, const char* name, scs_value_type_t type,
                     scs_telemetry_channel_callback_t cb, T* target, scs_u32_t index = SCS_U32_NIL) {
    if (reg(name, index, type, SCS_TELEMETRY_CHANNEL_FLAG_none, cb, target) != SCS_RESULT_ok)
        Log(SCS_LOG_TYPE_warning, std::string("channel not registered: ") + name);
}

void RegisterWheels(scs_telemetry_register_for_channel_t reg, const char* substance, const char* on_ground, Unit* u) {
    for (scs_u32_t w = 0; w < kMaxWheels; ++w) {
        reg(substance, w, SCS_VALUE_TYPE_u32, SCS_TELEMETRY_CHANNEL_FLAG_none, OnWheelSubstance, u);
        reg(on_ground, w, SCS_VALUE_TYPE_bool, SCS_TELEMETRY_CHANNEL_FLAG_none, OnWheelOnGround, u);
    }
}

// V3a: "<prefix>.wheel.rotation" etc. for every wheel of the unit.
void RegisterWheelPose(scs_telemetry_register_for_channel_t reg, const std::string& prefix, Unit* u) {
    const struct {
        const char* name;
        float* target;
    } channels[] = {{".wheel.rotation", u->rotation},
                    {".wheel.steering", u->steering},
                    {".wheel.suspension.deflection", u->deflection},
                    {".wheel.lift", u->lift}};
    for (const auto& ch : channels)
        for (scs_u32_t w = 0; w < kMaxWheels; ++w)
            reg((prefix + ch.name).c_str(), w, SCS_VALUE_TYPE_float, SCS_TELEMETRY_CHANNEL_FLAG_none, OnWheelFloat,
                ch.target);
}

// TruckersMP and Convoy: the probe stays off (single player only).
bool Multiplayer() {
    return GetModuleHandleW(L"core_ets2mp.dll") || GetModuleHandleW(L"core_atsmp.dll") || g.mp_offset != 0;
}

}  // namespace

SCSAPI_RESULT scs_telemetry_init(const scs_u32_t version, const scs_telemetry_init_params_t* const params) {
    // Accept 1.00 and 1.01 (the game offers the newest it supports first).
    if (version != SCS_TELEMETRY_VERSION_1_01 && version != SCS_TELEMETRY_VERSION_1_00) return SCS_RESULT_unsupported;
    const auto* p = static_cast<const scs_telemetry_init_params_v101_t*>(params);
    g.log = p->common.log;
    g.t0 = g.last_row = std::chrono::steady_clock::now();
    g.ats = strcmp(p->common.game_id, SCS_GAME_ID_ATS) == 0;
    Log(SCS_LOG_TYPE_message, "version " DIRT_VERSION);  // D7
    Log(SCS_LOG_TYPE_message, std::string("init (tests V1/V2/V3a + V3 tint + D3 dust): game=") + p->common.game_id +
                                  " game_version=" + std::to_string(SCS_GET_MAJOR_VERSION(p->common.game_version)) +
                                  "." + std::to_string(SCS_GET_MINOR_VERSION(p->common.game_version)));

    const std::wstring ini = IniPath();
    const bool telemetry = GetPrivateProfileIntW(L"probe", L"telemetry", 1, ini.c_str()) == 1;
    const bool gpu = GetPrivateProfileIntW(L"probe", L"gpu", 1, ini.c_str()) == 1;
    gpu_probe::Config gc;
    gc.census_s = static_cast<int>(GetPrivateProfileIntW(L"probe", L"census_s", 60, ini.c_str()));
    gc.sample_ms = static_cast<int>(GetPrivateProfileIntW(L"probe", L"sample_ms", 1000, ini.c_str()));
    gc.capture_s = static_cast<int>(GetPrivateProfileIntW(L"probe", L"capture_s", 0, ini.c_str()));
    gc.captures = static_cast<int>(GetPrivateProfileIntW(L"probe", L"captures", 20, ini.c_str()));
    gc.tint = static_cast<int>(GetPrivateProfileIntW(L"probe", L"tint", 2, ini.c_str()));
    gc.verbose = static_cast<int>(GetPrivateProfileIntW(L"probe", L"verbose", 0, ini.c_str()));
    gc.compat_enforce = GetPrivateProfileIntW(L"compat", L"enforce", 1, ini.c_str()) == 0 ? 0 : 1;
    if (!PluginDir().empty()) gc.premap_dir = PluginDir() + L"\\ets2_truck_dirt_premap";  // P3
    gc.premap_prefix = g.ats ? L"ats_" : L"ets2_";
    wchar_t num[32] = {};
    dirt::Settings ds;
    GetPrivateProfileStringW(L"dust", L"rate", L"1.0", num, 32, ini.c_str());
    ds.rate = static_cast<float>(_wtof(num));
    if (!(ds.rate >= 0.f && ds.rate <= 1000.f)) ds.rate = 1.f;
    GetPrivateProfileStringW(L"dust", L"level", L"-1", num, 32, ini.c_str());
    ds.override_level = static_cast<float>(_wtof(num));
    g.dust.Configure(ds);
    g.dust_forced = ds.override_level >= 0.f;
    // D11k (N44): [dust] relief = 0 keeps the dirt flat (no change to the G-buffer's normal).
    tint::SetRelief(GetPrivateProfileIntW(L"dust", L"relief", 1, ini.c_str()) != 0);
    // N40 step 1: [glass] on = 1 lets the truck's glass take the truck's dirt (off by default while it is tested).
    {
        const int glass_mode = GetPrivateProfileIntW(L"glass", L"on", 0, ini.c_str());
        const float strength = float(atof(IniString(ini, L"glass", L"strength", L"1.0").c_str()));
        tint::SetGlassDirt(glass_mode != 0, strength, glass_mode >= 2 ? glass_mode : 0);
        gpu_probe::SetWipeWanted(glass_mode != 0);  // N70: read the game's own blade lines while the glass is on
        if (glass_mode == 2)
            ConfigLog("[glass] on = 2: the glass the veil reaches is painted magenta (N40, to see what it reaches)");
        else if (glass_mode == 3)
            ConfigLog("[glass] on = 3: the glass paints each pixel where the shader places it (N62b)");
        else if (glass_mode == 4)
            ConfigLog("[glass] on = 4: the arc the wipers sweep is painted (N64, to see where it sits)");
        else if (glass_mode == 6)
            ConfigLog("[glass] on = 6: the glass paints the first blade's ring -- red its radius from the "
                      "pivot (half a metre per unit), green where the pixel sits across its stroke, blue the "
                      "coverage (N69e, to see which of the three is wrong)");
        else if (glass_mode == 5)
            ConfigLog("[glass] on = 5: the glass paints the dirt it worked out, red how much and green what the "
                      "blades took there (N64g)");
        else if (glass_mode)
            ConfigLog("[glass] on = 1, strength " + std::to_string(strength) +
                      ": the truck's glass takes the truck's dirt (N40, being tested)");
        // N64: [glass] wiper = the pivots' distance from the middle and their height (shares of the windshield the
        // plugin measures in the game), the blade's length (a share of the pane's height) and half the sweep in
        // degrees. In the INI because the arc has to be lined up against the blades on the screen, and a number in
        // the INI costs one game start while a number in the code costs a build and an install.
        float arc[4] = {0.45f, 0.02f, 0.48f, 42.f};
        wchar_t text[96] = {};
        GetPrivateProfileStringW(L"glass", L"wiper", L"", text, 96, ini.c_str());
        if (text[0]) {
            float v[4] = {};
            const int n = swscanf(text, L"%f %f %f %f", &v[0], &v[1], &v[2], &v[3]);
            const bool ok = (n == 1 && v[0] == 0.f) ||
                            (n == 4 && v[0] >= 0.f && v[0] <= 1.f && v[1] >= -0.5f && v[1] <= 1.f && v[2] > 0.f &&
                             v[2] <= 2.f && v[3] > 0.f && v[3] <= 85.f);
            if (!ok)
                Log(SCS_LOG_TYPE_warning, "[glass] wiper not understood (4 numbers, or 0 for no wiping): the "
                                          "default stays");
            else if (n == 1)
                arc[0] = 0.f;
            else
                memcpy(arc, v, sizeof(v));
        }
        tint::SetWiperArc(arc[0], arc[1], arc[2], arc[3] * 3.14159265f / 180.f);
        // N64k: how bright a surface of the dirt is on the glass, in the scene's own units.
        float bright = float(atof(IniString(ini, L"glass", L"brightness", L"10").c_str()));
        if (!(bright >= 0.f && bright <= panel::kGlassBrightMax)) bright = 10.f;
        tint::SetGlassBright(bright);
        // N64h: and the same numbers into the test panel, where they can be moved while looking at the glass.
        const float arc01[panel::kArcValues] = {arc[0], arc[1], arc[2], arc[3] / 90.f,
                                                bright / panel::kGlassBrightMax};
        g.panel.SetArc(arc01);
        // N65: the washer jet's key. The game has no washer of its own and the SDK has no channel for one, so the
        // jet is this key. Empty = off.
        const std::string washer = IniString(ini, L"glass", L"washer_key", L"f9");
        g.washer_key = panel::Hotkey();
        if (!washer.empty() && !panel::ParseHotkey(washer, &g.washer_key)) {
            Log(SCS_LOG_TYPE_warning, "[glass] washer_key = " + washer + " not understood: the jet is off");
            g.washer_key = panel::Hotkey();
        }
        if (glass_mode && g.washer_key.vk)
            ConfigLog("[glass] washer_key = " + panel::HotkeyName(g.washer_key) +
                      ": the jet cleans everything inside the wipers' arc (N65)");
        if (glass_mode && arc[0] > 0.f)
            ConfigLog("[glass] wiper = " + std::to_string(arc[0]) + " " + std::to_string(arc[1]) + " " +
                      std::to_string(arc[2]) + " " + std::to_string(arc[3]) +
                      " deg: the wipers clear the dust inside the arc they sweep (N64)");
    }
    // N41: [dust] cab_inside = half width, below, above, ahead of, behind the eye (m); 0 = off. D11f: cab_cavity, the same.
    for (const auto& [key, box] : {std::pair<const wchar_t*, float*>{L"cab_inside", g.cab_inside},
                                   std::pair<const wchar_t*, float*>{L"cab_cavity", g.cab_cavity}}) {
        wchar_t inside[96] = {};
        GetPrivateProfileStringW(L"dust", key, L"", inside, 96, ini.c_str());
        if (!inside[0]) continue;
        float b[5] = {};
        const int n = swscanf(inside, L"%f %f %f %f %f", &b[0], &b[1], &b[2], &b[3], &b[4]);
        bool ok = n == 1 ? b[0] == 0.f : n == 5;
        for (int i = 0; i < n && ok; ++i) ok = b[i] >= 0.f && b[i] <= 4.f;
        if (ok && n == 5) memcpy(box, b, sizeof(b));
        else if (ok) box[0] = 0.f;
        else
            Log(SCS_LOG_TYPE_warning, "[dust] " + std::string(box == g.cab_inside ? "cab_inside" : "cab_cavity") +
                                          " not understood (5 numbers in m, or 0): the default stays");
    }
    const bool panel_on = GetPrivateProfileIntW(L"test", L"panel", 0, ini.c_str()) == 1;
    wchar_t key_text[64] = {};
    GetPrivateProfileStringW(L"test", L"panel_key", L"ctrl+f5", key_text, 64, ini.c_str());
    char key_utf8[64] = {};
    WideCharToMultiByte(CP_UTF8, 0, key_text, -1, key_utf8, sizeof(key_utf8), nullptr, nullptr);
    if (!panel::ParseHotkey(key_utf8, &g.panel_key)) {
        Log(SCS_LOG_TYPE_warning, std::string("[test] panel_key = ") + key_utf8 + " not understood: ctrl+f5");
        panel::ParseHotkey("ctrl+f5", &g.panel_key);
    }

    if (telemetry) {
        if (!OpenFiles()) {
            Log(SCS_LOG_TYPE_error, "cannot open telemetry.csv in TruckDirt");
            return SCS_RESULT_generic_error;
        }
        g.writer.Start();  // D11n
        ConfigLog("---- session start ----");
        p->register_for_event(SCS_TELEMETRY_EVENT_configuration, OnConfig, nullptr);
        if (p->register_for_event(SCS_TELEMETRY_EVENT_gameplay, OnGameplay, nullptr) != SCS_RESULT_ok)
            Log(SCS_LOG_TYPE_warning, "gameplay events not registered");
    }
    p->register_for_event(SCS_TELEMETRY_EVENT_frame_end, OnFrameEnd, nullptr);
    p->register_for_event(SCS_TELEMETRY_EVENT_paused, OnPause, nullptr);
    p->register_for_event(SCS_TELEMETRY_EVENT_started, OnPause, nullptr);

    auto reg = p->register_for_channel;
    RegisterChannel(reg, SCS_TELEMETRY_TRUCK_CHANNEL_speed, SCS_VALUE_TYPE_float, OnFloat, &g.speed_ms);
    RegisterChannel(reg, SCS_TELEMETRY_TRUCK_CHANNEL_wipers, SCS_VALUE_TYPE_bool, OnBool, &g.wipers);
    RegisterChannel(reg, SCS_TELEMETRY_CHANNEL_multiplayer_time_offset, SCS_VALUE_TYPE_s32, OnS32, &g.mp_offset);
    RegisterChannel(reg, SCS_TELEMETRY_TRUCK_CHANNEL_odometer, SCS_VALUE_TYPE_float, OnFloat, &g.odometer_km);
    RegisterChannel(reg, SCS_TELEMETRY_CHANNEL_game_time, SCS_VALUE_TYPE_u32, OnU32, &g.game_time);
    RegisterChannel(reg, "trailer.0.connected", SCS_VALUE_TYPE_bool, OnBool, &g.trailer_connected);
    RegisterChannel(reg, SCS_TELEMETRY_TRUCK_CHANNEL_engine_enabled, SCS_VALUE_TYPE_bool, OnBool, &g.engine);
    // D5 (L7): the refuel watch and the wash need these, with or without the CSV.
    RegisterChannel(reg, SCS_TELEMETRY_TRUCK_CHANNEL_fuel, SCS_VALUE_TYPE_float, OnFloat, &g.fuel_l);
    RegisterChannel(reg, SCS_TELEMETRY_TRUCK_CHANNEL_parking_brake, SCS_VALUE_TYPE_bool, OnBool, &g.parking_brake);
    RegisterWheels(reg, SCS_TELEMETRY_TRUCK_CHANNEL_wheel_substance, SCS_TELEMETRY_TRUCK_CHANNEL_wheel_on_ground,
                   &g.truck);
    RegisterWheels(reg, "trailer.0.wheel.substance", "trailer.0.wheel.on_ground", &g.trailer);
    if (telemetry) {
        RegisterChannel(reg, SCS_TELEMETRY_TRUCK_CHANNEL_world_placement, SCS_VALUE_TYPE_dplacement, OnDPlacement,
                        &g.truck_placement);
    }
    if (gpu) {
        // V3a: the pose of the player's units for the frame captures.
        if (!telemetry)
            RegisterChannel(reg, SCS_TELEMETRY_TRUCK_CHANNEL_world_placement, SCS_VALUE_TYPE_dplacement, OnDPlacement,
                            &g.truck_placement);
        RegisterChannel(reg, "trailer.0.world.placement", SCS_VALUE_TYPE_dplacement, OnDPlacement, &g.trailer_placement);
        RegisterChannel(reg, SCS_TELEMETRY_TRUCK_CHANNEL_cabin_offset, SCS_VALUE_TYPE_fplacement, OnFPlacement,
                        &g.cabin_offset);
        RegisterWheelPose(reg, "truck", &g.truck);
        RegisterWheelPose(reg, "trailer.0", &g.trailer);
        for (int i = 0; i < gpu_probe::kPoseMoreTrailers; ++i) {  // N21 (step 1)
            const std::string t = "trailer." + std::to_string(i + 1);
            RegisterChannel(reg, (t + ".connected").c_str(), SCS_VALUE_TYPE_bool, OnBool, &g.more_connected[i]);
            RegisterChannel(reg, (t + ".world.placement").c_str(), SCS_VALUE_TYPE_dplacement, OnDPlacement,
                            &g.more_placement[i]);
            RegisterWheelPose(reg, t, &g.more[i]);
        }
    }

    if (gpu) {
        if (Multiplayer()) {
            Log(SCS_LOG_TYPE_message, "gpu probe: off (multiplayer)");
        } else {
            g.gpu_started = true;
            gpu_probe::Start(DirtDir(), gc);
            // D5: the saved dirt (the units get theirs when their configs come).
            g.state_file = std::filesystem::path(DirtDir()) / L"dirt_state.txt";
            g.cargo_file = std::filesystem::path(DirtDir()) / L"cargo_state.txt";  // N57
            {
                std::ifstream f(g.cargo_file, std::ios::binary);
                std::stringstream ss;
                ss << f.rdbuf();
                if (cargo::Parse(ss.str(), &g.cargo)) ConfigLog("cargo (N57): " + g.cargo.key + " read back");
            }
            const dirt_state::ParseReport rep = g.saved.Load(g.state_file);
            static const char* const kResult[] = {"ok", "damaged", "unreadable", "of a newer version"};
            std::string text = std::string("dirt_state.txt: ") + kResult[static_cast<int>(rep.result)] + ", " +
                               std::to_string(rep.entries) + " units";
            if (!rep.detail.empty()) text += " - " + rep.detail;
            if (g.saved.ReadOnly()) text += " (left alone, nothing will be saved)";
            else if (rep.result != dirt_state::Parse::kOk) text += " (a copy is in dirt_state.txt.bad)";
            if (g.dust_forced) text += " ([dust] level forced: nothing will be saved)";
            Log(rep.result == dirt_state::Parse::kOk ? SCS_LOG_TYPE_message : SCS_LOG_TYPE_warning, text);
            g.state_on = true;
            g.last_save = std::chrono::steady_clock::now();
            // N10: the panel needs the GPU part (it is drawn at Present).
            g.panel.Enable(panel_on, panel::HotkeyName(g.panel_key));
            if (panel_on)
                Log(SCS_LOG_TYPE_message, "test panel: on, " + panel::HotkeyName(g.panel_key) + " opens it");
            g.block_input = GetPrivateProfileIntW(L"menu", L"block_input", 1, ini.c_str()) == 1;
            // D10 (N9): the overlay's texts in the game's language.
            LoadTexts(ini);
            // D5 (L7): the wash at the station needs the GPU part too (drawn at Present).
            g.wash_on = WashConfigure(ini);
            // V6: the keys menu (drawn at Present too).
            MenuConfigure(ini);
        }
    }
    // V4: the map's trigger actions, logged to learn the service station's ([wash] trigger_log = 1; off by default).
    if (GetPrivateProfileIntW(L"wash", L"trigger_log", 0, ini.c_str()) == 1) {
        const std::string line = Multiplayer() ? std::string("off (multiplayer)") : trigger_probe::Start();
        g.trigger_log = line.compare(0, 2, "on") == 0;
        Log(SCS_LOG_TYPE_message, "trigger log: " + line);
        ConfigLog("trigger log: " + line);
    }
    char ini_utf8[MAX_PATH * 3] = {};
    WideCharToMultiByte(CP_UTF8, 0, ini.c_str(), -1, ini_utf8, sizeof(ini_utf8), nullptr, nullptr);
    Log(SCS_LOG_TYPE_message, std::string("settings: ") + ini_utf8 + " (telemetry " + (telemetry ? "on" : "off") +
                                  ", gpu " + (g.gpu_started ? "on" : "off") + ", draw " + (!g.gpu_started || !gc.tint ? "off" : gc.tint == 1 ? "red" : gc.tint == 3 ? "position test" : gc.tint == 4 ? "diagnosis" : "dust") +
                                  ", compat " + (gc.compat_enforce ? "enforce" : "log only") + ")");
    return SCS_RESULT_ok;
}

SCSAPI_VOID scs_telemetry_shutdown(void) {
    if (g.trigger_log) trigger_probe::Stop();
    g.trigger_log = false;
    input_hook::Stop();
    if (g.gpu_started) gpu_probe::Stop();
    SaveState("shutdown");
    Flush();
    if (g.csv) WriteRow();
    ConfigLog("---- session end ----");
    g.writer.Stop();  // D11n: every file written before they close
    SaveFailures();
    if (g.csv) {
        fclose(g.csv);
        g.csv = nullptr;
    }
    if (g.configs) {
        fclose(g.configs);
        g.configs = nullptr;
    }
    Log(SCS_LOG_TYPE_message, "shutdown");
    g.log = nullptr;
}
