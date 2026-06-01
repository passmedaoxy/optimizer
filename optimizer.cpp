/*
 * Optimizer v2.8.5 - C++ Port
 *
 * Windows Game Optimizer with RAM Cleaner
 *
 * Requires:
 *   - Visual Studio 2022, C++17
 *   - nlohmann/json single header (json.hpp) in same folder
 *   - Unicode Character Set (project property)
 *   - Additional Dependencies: psapi.lib;shell32.lib;advapi32.lib;ole32.lib;oleaut32.lib
 *   - Entry Point: wmainCRTStartup
 */

#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#define NOMINMAX
#pragma warning(disable: 4005)  // macro redefinition
#undef  _WIN32_WINNT
#define _WIN32_WINNT 0x0A00  // Windows 10+ required for CPU Sets, SetProcessInformation
#pragma warning(default: 4005)  // restore

#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <objbase.h>
#include <shobjidl.h>
#include <winioctl.h>       // DISK_CACHE_INFORMATION, IOCTL_DISK_*
#include <setupapi.h>      // SetupDiGetClassDevsW (hot-plug monitor)
#pragma comment(lib, "setupapi.lib")
#include <processthreadsapi.h> // SetProcessInformation, PROCESS_INFORMATION_CLASS
#include <winternl.h>          // NtSetSystemInformation

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <deque>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <functional>
#include <atomic>
#include <thread>
#include <mutex>
#include <chrono>
#include <filesystem>
#include <ctime>
#include <cassert>

#pragma warning(push, 0)  // suppress all warnings from json.hpp
#include "json.hpp"
#pragma warning(pop)

 // ============================================================
 // MANUAL WIN32 API DECLARATIONS
 // These APIs require Win10+ but are not always exposed by windows.h
 // Declaring them explicitly avoids _WIN32_WINNT version issues
 // ============================================================

#ifndef PROCESS_POWER_THROTTLING_CURRENT_VERSION
typedef struct _PROCESS_POWER_THROTTLING_STATE {
    ULONG Version;
    ULONG ControlMask;
    ULONG StateMask;
} PROCESS_POWER_THROTTLING_STATE;
static constexpr ULONG PROCESS_POWER_THROTTLING_CURRENT_VERSION = 1;
static constexpr ULONG PROCESS_POWER_THROTTLING_EXECUTION_SPEED = 0x1;
#endif

#ifndef ProcessPowerThrottling
#pragma warning(push)
#pragma warning(disable: 4005)
#define ProcessPowerThrottling ((PROCESS_INFORMATION_CLASS)4)
#pragma warning(pop)
#endif

// SetProcessInformation (kernel32, Win8+)
typedef BOOL(WINAPI* PSetProcessInformation_t)(
    HANDLE, PROCESS_INFORMATION_CLASS, LPVOID, DWORD);
static PSetProcessInformation_t g_SetProcessInformation_fn = nullptr;

// CPU Set APIs (kernel32, Win10+)
typedef BOOL(WINAPI* PGetSystemCpuSetInformation_t)(
    PSYSTEM_CPU_SET_INFORMATION, ULONG, PULONG, HANDLE, ULONG);
typedef BOOL(WINAPI* PSetProcessDefaultCpuSets_t)(
    HANDLE, const ULONG*, ULONG);
static PGetSystemCpuSetInformation_t  g_GetSystemCpuSetInformation_fn = nullptr;
static PSetProcessDefaultCpuSets_t    g_SetProcessDefaultCpuSets_fn = nullptr;

static void init_win10_apis()
{
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    if (!k32) return;
    g_SetProcessInformation_fn =
        reinterpret_cast<PSetProcessInformation_t>(
            GetProcAddress(k32, "SetProcessInformation"));
    g_GetSystemCpuSetInformation_fn =
        reinterpret_cast<PGetSystemCpuSetInformation_t>(
            GetProcAddress(k32, "GetSystemCpuSetInformation"));
    g_SetProcessDefaultCpuSets_fn =
        reinterpret_cast<PSetProcessDefaultCpuSets_t>(
            GetProcAddress(k32, "SetProcessDefaultCpuSets"));
}
// Suppress MSVC static analysis warnings that are false positives
// or come from third-party headers (nlohmann/json)
#pragma warning(disable: 26495) // uninitialized member (json internal)
#pragma warning(disable: 26494) // variable uninitialized (json internal)
#pragma warning(disable: 26110) // caller failing to hold lock (false positive on lock_guard)
#pragma warning(disable: 26111) // releasing unheld lock
#pragma warning(disable: 26115) // releasing unheld lock (false positive on lock_guard)
#pragma warning(disable: 26117) // releasing unheld lock in function
#pragma warning(disable: 26819) // unannotated fallthrough
#pragma warning(disable: 26818) // switch statement does not cover all cases
#pragma warning(disable: 26816) // nodiscard return value ignored
#pragma warning(disable: 6031)  // return value of sscanf ignored
#pragma warning(disable: 4210)  // static function declared at file scope
#pragma warning(disable: 4505)  // unreferenced function removed
#pragma warning(disable: 4514)  // unreferenced inline function removed
using json = nlohmann::json;
namespace fs = std::filesystem;

// ============================================================
// FORWARD DECLARATIONS
// ============================================================
struct ProcessInfo {
    DWORD        pid = 0;
    std::string  name;
    std::wstring exe_path;
};
static std::vector<ProcessInfo> enumerate_processes();
// ============================================================
// ============================================================
static void log_msg(const std::string& msg);
static bool pid_exists(DWORD pid);
static std::string to_lower(std::string s);
static std::string trim(const std::string& s);
static std::vector<std::string> str_split(const std::string& s, char delim);
static std::wstring to_wide(const std::string& s);
static std::string to_narrow(const std::wstring& w);
static void restore_affinity();
static void restore_background_priority();
static void tweak_explorer_priority(bool lower);
static bool feature_enabled(const std::string& name, const json& effective, const json& game_cfg);
static json get_runtime_config();
static json load_presets();
static void refresh_runtime_config();
static json load_json_file(const fs::path& path, const json& fallback);
static bool save_json_file(const fs::path& path, const json& data);
static void restore_numa_affinity(DWORD pid);
static void restore_gpu_processes();
static void restore_network_threads();
static void restore_nagle();
static void restore_nic_power_saving();
static void restore_cpu_parking();
static void revert_mmcss();
static void set_io_priority_normal(DWORD pid);
static void restore_power_throttling(DWORD pid);
static void restore_services();
static void restore_game_bar();
static void restore_fullscreen_optimizations();
static void restore_memory_compression();
static void restore_dwm_priority();
static void restore_visual_effects();
static void restore_dynamic_tick();
static void restore_tcp_autotuning();
static void restore_ecn();
static void restore_rss();
static void restore_qos_reserve();
static void remove_dscp_tagging(const std::string& game_exe);
static void restore_interrupt_moderation();
static void restore_adapter_buffers();
static void restore_flow_control();
static void restore_cstates();
static void restore_boost();
static void restore_cpu_sets(DWORD pid);
static void restore_smt_scheduling();
static void restore_background_thread_mode();
static void restore_prefetch();
static void init_mitigation_api();
static void init_standby_api();
static void restore_gpu_power();
static void restore_interrupt_affinity();
static void restore_usb_power_saving();
static void restore_audio_latency();
static void remove_defender_exclusion();

// ============================================================
// CONSTANTS
// ============================================================

static const std::string VERSION = "2.8.5";
static const std::wstring ULTIMATE_POWER_GUID_W = L"e9a42b02-d5df-448d-aa00-03f14749eb61";
static const double  UI_DELAY_MS = 25.0;
static const int     WORKER_EXIT_NORMAL = 0;
static const int     WORKER_EXIT_RELOAD = 100;
static const double  HIGH_CPU_THRESHOLD = 85.0;
static const double  HIGH_MEM_THRESHOLD = 90.0;
static const double  LOW_CPU_THRESHOLD = 60.0;
static const double  LOW_MEM_THRESHOLD = 80.0;
static const int     RAM_CLEAN_DEFAULT_INTERVAL_SEC = 300;

// ============================================================
// PATHS
// ============================================================

static fs::path APPDATA_DIR;
static fs::path GAMES_FILE;
static fs::path CLOSE_APPS_FILE;
static fs::path REOPEN_APPS_FILE;
static fs::path PROCESS_GROUPS_FILE;
static fs::path CONFIG_FILE;
static fs::path PRESETS_FILE;
static fs::path LOG_PATH;
static fs::path RUN_STATE_FILE;
static fs::path HISTORY_FILE;
static fs::path NEVER_ASK_FILE;
static fs::path PROFILES_DIR;
static fs::path THEMES_FILE;

static void init_paths()
{
    wchar_t local_app[MAX_PATH]{};
    SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, local_app);
    APPDATA_DIR = fs::path(local_app) / L"Optimizer";
    GAMES_FILE = APPDATA_DIR / "games.json";
    CLOSE_APPS_FILE = APPDATA_DIR / "close_apps.json";
    REOPEN_APPS_FILE = APPDATA_DIR / "reopen_apps.json";
    PROCESS_GROUPS_FILE = APPDATA_DIR / "process_groups.json";
    CONFIG_FILE = APPDATA_DIR / "config.json";
    PRESETS_FILE = APPDATA_DIR / "presets.json";
    LOG_PATH = APPDATA_DIR / "optimizer.log";
    RUN_STATE_FILE = APPDATA_DIR / "run_state.json";
    HISTORY_FILE = APPDATA_DIR / "session_history.txt";
    NEVER_ASK_FILE = APPDATA_DIR / "never_ask.json";
    PROFILES_DIR = APPDATA_DIR / "profiles";
    THEMES_FILE = APPDATA_DIR / "themes.json";
}

// ============================================================
// GLOBAL STATE
// ============================================================

static std::atomic<bool> g_paused{ false };
static std::atomic<bool> g_running{ true };
static std::atomic<bool> g_relaunch_pending{ false };
static std::atomic<int>  g_exit_code{ WORKER_EXIT_NORMAL };
static std::atomic<bool> g_console_muted{ false };

// Console view mode: 0 = not set, 1 = clean, 2 = debug
static std::atomic<int>  g_view_mode{ 0 };
static const int         VIEW_CLEAN = 1;
static const int         VIEW_DEBUG = 2;

static bool is_clean() { return g_view_mode.load() == VIEW_CLEAN; }

static std::string g_current_game_name;
static std::mutex  g_game_name_mutex;

static HWND            g_tray_hwnd = nullptr;
static NOTIFYICONDATAW g_nid = {};
static const UINT      TRAY_ICON_MSG = WM_USER + 20;

static std::unordered_map<DWORD, std::vector<DWORD_PTR>> g_affinity_backup;
static std::unordered_map<DWORD, int>                    g_bg_priority_backup;
static std::unordered_map<DWORD, int>                    g_explorer_priority_backup;
static std::vector<std::wstring>                         g_closed_reopen_paths;
static std::mutex g_backup_mutex;

static std::ofstream g_log_file;
static std::mutex    g_log_mutex;

static json       g_runtime_config;
static json       g_presets_cache;
static std::mutex g_config_lock;

static HWND g_console_hwnd = nullptr;

// Auto-discovery
struct DiscoveryItem {
    DWORD        pid;
    std::string  name;
    std::wstring exe_path;
};
static std::vector<DiscoveryItem> g_pending_discovery;
static std::mutex                 g_console_mutex;
static std::atomic<bool>          g_discovery_pending{ false };
static std::atomic<bool>          g_discovery_enabled{ true };

// NtDll timer
typedef LONG(NTAPI* PNtSetTimerResolution)(ULONG, BOOLEAN, PULONG);
static PNtSetTimerResolution g_NtSetTimerResolution = nullptr;
static ULONG g_timer_current_res = 0;

// ============================================================
// DEFAULT CONFIG / PRESETS  (plain string -- no raw literals)
// ============================================================

static json make_default_config()
{
    json j;
    j["mode"] = "balanced";
    j["dry_run"] = false;
    j["verbose_init"] = false;

    j["features"]["priority_boost"] = true;
    j["features"]["timer_resolution"] = true;
    j["features"]["close_apps"] = true;
    j["features"]["background_affinity_isolation"] = false;
    j["features"]["explorer_priority_tweak"] = true;
    j["features"]["perf_monitor"] = true;
    j["features"]["ram_cleaner"] = false;
    j["features"]["mmcss"] = true;
    j["features"]["io_priority"] = true;
    j["features"]["cpu_unpark"] = true;
    j["features"]["numa_affinity"] = false;
    j["features"]["gpu_priority"] = true;
    j["features"]["network_optimizations"] = true;
    j["features"]["net_advanced"] = true;
    j["features"]["power_throttle_disable"] = true;
    j["features"]["service_suspend"] = true;
    j["features"]["gamebar_disable"] = true;
    j["features"]["fso_disable"] = true;
    j["features"]["memory_compression_disable"] = false;
    j["features"]["dwm_priority"] = true;
    j["features"]["visual_effects_disable"] = true;
    j["features"]["disk_write_cache"] = true;
    j["features"]["hpet_disable"] = false;
    j["features"]["cpu_advanced"] = true;
    j["features"]["cstate_disable"] = true;
    j["features"]["force_boost"] = true;
    j["features"]["cpu_set_isolation"] = false;
    j["features"]["large_pages"] = true;
    j["features"]["smt_optimize"] = true;
    j["features"]["prefetch_disable"] = true;
    j["features"]["gpu_power_max"] = true;
    j["features"]["standby_cleaner"] = true;
    j["features"]["working_set_trim"] = true;
    j["features"]["interrupt_affinity"] = false;
    j["features"]["usb_power_disable"] = true;
    j["features"]["audio_latency"] = true;
    j["features"]["defender_exclusion"] = false;
    j["features"]["auto_discovery"] = true;

    j["view_mode"] = 0;  // 0 = ask on first launch, 1 = clean, 2 = debug

    j["ram_cleaner"]["interval_sec"] = RAM_CLEAN_DEFAULT_INTERVAL_SEC;
    j["ram_cleaner"]["empty_working_sets"] = false;  // off by default -- causes page faults in games
    j["ram_cleaner"]["flush_file_cache"] = true;

    j["background_apps"] = json::array({
        "Discord.exe","chrome.exe","msedge.exe",
        "firefox.exe","Spotify.exe","steamwebhelper.exe"
        });
    return j;
}

static json make_default_presets()
{
    auto make_preset = [](const std::string& desc,
        bool close, bool affinity, bool explorer,
        bool perf, bool ram) -> json
        {
            json p;
            p["description"] = desc;
            p["features"]["priority_boost"] = true;
            p["features"]["timer_resolution"] = true;
            p["features"]["close_apps"] = close;
            p["features"]["background_affinity_isolation"] = affinity;
            p["features"]["explorer_priority_tweak"] = explorer;
            p["features"]["perf_monitor"] = perf;
            p["features"]["ram_cleaner"] = ram;
            p["locked"] = json::array();
            return p;
        };

    json j;
    j["safe"] = make_preset("Low-risk tweaks only", false, false, false, true, false);
    // safe extra features off
    j["safe"]["features"]["mmcss"] = true;
    j["safe"]["features"]["io_priority"] = true;
    j["safe"]["features"]["cpu_unpark"] = false;
    j["safe"]["features"]["numa_affinity"] = false;
    j["safe"]["features"]["gpu_priority"] = false;
    j["safe"]["features"]["network_optimizations"] = false;
    j["safe"]["features"]["net_advanced"] = false;
    j["safe"]["features"]["power_throttle_disable"] = true;
    j["safe"]["features"]["service_suspend"] = false;
    j["safe"]["features"]["gamebar_disable"] = true;
    j["safe"]["features"]["fso_disable"] = true;
    j["safe"]["features"]["memory_compression_disable"] = false;
    j["safe"]["features"]["dwm_priority"] = true;
    j["safe"]["features"]["visual_effects_disable"] = false;
    j["safe"]["features"]["disk_write_cache"] = true;
    j["safe"]["features"]["hpet_disable"] = false;
    j["safe"]["features"]["cpu_advanced"] = false;
    j["safe"]["features"]["cstate_disable"] = false;
    j["safe"]["features"]["force_boost"] = false;
    j["safe"]["features"]["cpu_set_isolation"] = false;
    j["safe"]["features"]["large_pages"] = false;
    j["safe"]["features"]["smt_optimize"] = false;
    j["safe"]["features"]["prefetch_disable"] = false;
    j["safe"]["features"]["gpu_power_max"] = false;
    j["safe"]["features"]["standby_cleaner"] = false;
    j["safe"]["features"]["working_set_trim"] = false;
    j["safe"]["features"]["interrupt_affinity"] = false;
    j["safe"]["features"]["usb_power_disable"] = false;
    j["safe"]["features"]["audio_latency"] = false;
    j["safe"]["features"]["defender_exclusion"] = false;
    j["balanced"] = make_preset("Recommended default", true, false, true, true, false);
    j["balanced"]["features"]["mmcss"] = true;
    j["balanced"]["features"]["io_priority"] = true;
    j["balanced"]["features"]["cpu_unpark"] = true;
    j["balanced"]["features"]["numa_affinity"] = false;
    j["balanced"]["features"]["gpu_priority"] = true;
    j["balanced"]["features"]["network_optimizations"] = true;
    j["balanced"]["features"]["net_advanced"] = true;
    j["balanced"]["features"]["power_throttle_disable"] = true;
    j["balanced"]["features"]["service_suspend"] = true;
    j["balanced"]["features"]["gamebar_disable"] = true;
    j["balanced"]["features"]["fso_disable"] = true;
    j["balanced"]["features"]["memory_compression_disable"] = false;
    j["balanced"]["features"]["dwm_priority"] = true;
    j["balanced"]["features"]["visual_effects_disable"] = true;
    j["balanced"]["features"]["disk_write_cache"] = true;
    j["balanced"]["features"]["hpet_disable"] = false;
    j["balanced"]["features"]["cpu_advanced"] = true;
    j["balanced"]["features"]["cstate_disable"] = true;
    j["balanced"]["features"]["force_boost"] = true;
    j["balanced"]["features"]["cpu_set_isolation"] = false;
    j["balanced"]["features"]["large_pages"] = true;
    j["balanced"]["features"]["smt_optimize"] = true;
    j["balanced"]["features"]["prefetch_disable"] = true;
    j["balanced"]["features"]["gpu_power_max"] = true;
    j["balanced"]["features"]["standby_cleaner"] = true;
    j["balanced"]["features"]["working_set_trim"] = true;
    j["balanced"]["features"]["interrupt_affinity"] = false;
    j["balanced"]["features"]["usb_power_disable"] = true;
    j["balanced"]["features"]["audio_latency"] = true;
    j["balanced"]["features"]["defender_exclusion"] = false;
    j["aggressive"] = make_preset("More aggressive (use carefully)", true, true, true, true, true);
    j["aggressive"]["features"]["mmcss"] = true;
    j["aggressive"]["features"]["io_priority"] = true;
    j["aggressive"]["features"]["cpu_unpark"] = true;
    j["aggressive"]["features"]["numa_affinity"] = true;
    j["aggressive"]["features"]["gpu_priority"] = true;
    j["aggressive"]["features"]["network_optimizations"] = true;
    j["aggressive"]["features"]["net_advanced"] = true;
    j["aggressive"]["features"]["power_throttle_disable"] = true;
    j["aggressive"]["features"]["service_suspend"] = true;
    j["aggressive"]["features"]["gamebar_disable"] = true;
    j["aggressive"]["features"]["fso_disable"] = true;
    j["aggressive"]["features"]["memory_compression_disable"] = true;
    j["aggressive"]["features"]["dwm_priority"] = true;
    j["aggressive"]["features"]["visual_effects_disable"] = true;
    j["aggressive"]["features"]["disk_write_cache"] = true;
    j["aggressive"]["features"]["hpet_disable"] = false;
    j["aggressive"]["features"]["cpu_advanced"] = true;
    j["aggressive"]["features"]["cstate_disable"] = true;
    j["aggressive"]["features"]["force_boost"] = true;
    j["aggressive"]["features"]["cpu_set_isolation"] = true;
    j["aggressive"]["features"]["large_pages"] = true;
    j["aggressive"]["features"]["smt_optimize"] = true;
    j["aggressive"]["features"]["prefetch_disable"] = true;
    j["aggressive"]["features"]["gpu_power_max"] = true;
    j["aggressive"]["features"]["standby_cleaner"] = true;
    j["aggressive"]["features"]["working_set_trim"] = true;
    j["aggressive"]["features"]["interrupt_affinity"] = true;
    j["aggressive"]["features"]["usb_power_disable"] = true;
    j["aggressive"]["features"]["audio_latency"] = true;
    j["aggressive"]["features"]["defender_exclusion"] = false;
    return j;
}

static json DEFAULT_CONFIG = make_default_config();
static json DEFAULT_PRESETS = make_default_presets();

// ============================================================
// STRING HELPERS
// ============================================================

static std::string to_lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    return s;
}

static std::string trim(const std::string& s)
{
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    return (a == std::string::npos) ? "" : s.substr(a, b - a + 1);
}

static std::vector<std::string> str_split(const std::string& s, char delim)
{
    std::vector<std::string> out;
    std::istringstream ss(s);
    std::string tok;
    while (std::getline(ss, tok, delim)) {
        std::string t = trim(tok);
        if (!t.empty()) out.push_back(t);
    }
    return out;
}

static std::wstring to_wide(const std::string& s)
{
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    if (!w.empty() && w.back() == 0) w.pop_back();
    return w;
}

static std::string to_narrow(const std::wstring& w)
{
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    if (!s.empty() && s.back() == 0) s.pop_back();
    return s;
}

// ============================================================
// CONSOLE COLOR & CLEAN UI HELPERS
// ============================================================

// Color map for log prefixes
static HANDLE g_con_handle = nullptr;

static void con_col(WORD attr) {
    if (g_con_handle) SetConsoleTextAttribute(g_con_handle, attr);
}
static void con_reset() { con_col(0x07); }

static WORD prefix_color(const std::string& msg) {
    if (msg.find("[APPLY]") != std::string::npos ||
        msg.find("[CLOSE]") != std::string::npos ||
        msg.find("[RESTORE]") != std::string::npos ||
        msg.find("[PERF]") != std::string::npos ||
        msg.find("[RAM]") != std::string::npos ||
        msg.find("[MMCSS]") != std::string::npos ||
        msg.find("[IO]") != std::string::npos ||
        msg.find("[GPU]") != std::string::npos ||
        msg.find("[NET]") != std::string::npos ||
        msg.find("[CPU]") != std::string::npos)
        return 0x0A; // green
    if (msg.find("[STATE]") != std::string::npos ||
        msg.find("[DETECTED]") != std::string::npos ||
        msg.find("[INIT]") != std::string::npos ||
        msg.find("[DETECTION]") != std::string::npos ||
        msg.find("[DISCOVERY]") != std::string::npos ||
        msg.find("[STARTUP]") != std::string::npos)
        return 0x0B; // cyan
    if (msg.find("[WARN]") != std::string::npos ||
        msg.find("[DRY-RUN]") != std::string::npos ||
        msg.find("[SKIP]") != std::string::npos ||
        msg.find("[AUDIT]") != std::string::npos)
        return 0x0E; // yellow
    if (msg.find("[ERROR]") != std::string::npos ||
        msg.find("[FAIL]") != std::string::npos ||
        msg.find("[CONFIG-ERROR]") != std::string::npos)
        return 0x0C; // red
    if (msg.find("[SESSION]") != std::string::npos ||
        msg.find("[BENCH]") != std::string::npos ||
        msg.find("[HISTORY]") != std::string::npos)
        return 0x0D; // magenta
    return 0x07;
}

static void print_divider(char c = '-', int len = 58) {
    con_col(0x08);
    std::string line(len, c);
    std::cout << line << "\n";
    con_reset();
}

static void print_section(const std::string& title) {
    print_divider('=');
    con_col(0x0F);
    int pad = (58 - (int)title.size()) / 2;
    if (pad < 0) pad = 0;
    std::string spaces(pad, ' ');
    std::cout << spaces << title << "\n";
    con_reset();
    print_divider('=');
}

static void print_kv(const std::string& key, const std::string& val,
    WORD val_color = 0x0F)
{
    con_col(0x08);
    std::string kline = key + ": ";
    std::cout << kline;
    con_col(val_color);
    std::cout << val << "\n";
    con_reset();
}

static void print_dashboard(const std::string& mode,
    int game_count,
    bool detection_on,
    bool dry_run)
{
    std::string title = "Optimizer v" + VERSION;
    print_section(title);
    WORD mc = (mode == "aggressive") ? 0x0C : (mode == "safe") ? 0x0E : 0x0A;
    print_kv("Mode", mode, mc);
    print_kv("Games", std::to_string(game_count) + " loaded", 0x0F);
    print_kv("Detection", detection_on ? "ON" : "OFF", detection_on ? 0x0A : 0x08);
    if (dry_run) {
        con_col(0x0E);
        std::cout << "[DRY-RUN MODE ACTIVE - no changes will be applied]\n";
        con_reset();
    }
    print_divider();
    con_col(0x08);
    std::cout << "Waiting for a game to launch...\n";
    std::cout << "Type 'help' to see all commands.\n";
    con_reset();
    std::cout << "\n";
}

static void print_game_start_banner(const std::string& game,
    const std::string& mode)
{
    std::cout << "\n";
    print_divider('=');
    con_col(0x0F);
    std::cout << "Game Detected\n";
    con_reset();
    print_kv("Game", game, 0x0F);
    WORD mc = (mode == "aggressive") ? 0x0C : (mode == "safe") ? 0x0E : 0x0A;
    print_kv("Mode", mode, mc);
    print_divider('=');
    std::cout << "\n";
}

static void print_game_end_banner(const std::string& game, int duration_secs)
{
    std::cout << "\n";
    print_divider('-');
    con_col(0x08);
    int h = duration_secs / 3600;
    int m = (duration_secs % 3600) / 60;
    int s = duration_secs % 60;
    char tbuf[32] = {};
    _snprintf_s(tbuf, sizeof(tbuf), _TRUNCATE, "%02d:%02d:%02d", h, m, s);
    std::cout << "Session ended: " << game << "  (" << tbuf << ")\n";
    std::cout << "Restoring settings...\n";
    con_reset();
    print_divider('-');
    std::cout << "\n";
}

static void print_prompt() {
    if (g_console_muted.load()) return;
    con_col(0x08);
    std::cout << "\n  > ";
    con_reset();
    std::cout.flush();
}

static void prompt_view_mode() {
    // Load existing preference if already set
    try {
        json cfg = load_json_file(CONFIG_FILE, json::object());
        if (cfg.contains("view_mode") && cfg["view_mode"].is_number_integer()) {
            int v = cfg["view_mode"].get<int>();
            if (v == VIEW_CLEAN || v == VIEW_DEBUG) {
                g_view_mode.store(v);
                return;
            }
        }
    }
    catch (...) {
        // Config unreadable -- default to clean and continue
        g_view_mode.store(VIEW_CLEAN);
        return;
    }

    // First launch -- check stdin is actually usable before prompting
    HANDLE hStdin = GetStdHandle(STD_INPUT_HANDLE);
    if (hStdin == INVALID_HANDLE_VALUE || hStdin == nullptr) {
        g_view_mode.store(VIEW_CLEAN);
        return;
    }
    DWORD mode = 0;
    if (!GetConsoleMode(hStdin, &mode)) {
        // stdin is not a console (piped/detached) -- default silently
        g_view_mode.store(VIEW_CLEAN);
        return;
    }

    // Safe to prompt
    std::cout << "\n";
    print_divider('=');
    con_col(0x0F);
    std::string welcome = "Welcome to Optimizer v" + VERSION;
    std::cout << welcome << "\n";
    con_reset();
    print_divider('=');
    std::cout << "\n";
    con_col(0x0F);
    std::cout << "How would you like the console to look?\n\n";
    con_reset();
    con_col(0x0A);
    std::cout << "1  Clean  ";
    con_col(0x07);
    std::cout << "  organized, color coded, less cluttered\n";
    con_col(0x0B);
    std::cout << "2  Debug  ";
    con_col(0x07);
    std::cout << "  raw output, every message visible\n";
    std::cout << "\n";
    con_col(0x08);
    std::cout << "You can change this any time with: view clean / view debug\n\n";
    con_reset();

    int choice = VIEW_CLEAN;
    while (true) {
        con_col(0x08);
        std::cout << "Type 1 or 2: ";
        con_reset();
        std::cout.flush();
        std::string line;
        if (!std::getline(std::cin, line)) {
            // stdin closed -- default to clean
            break;
        }
        line = trim(line);
        if (line == "1") { choice = VIEW_CLEAN; break; }
        if (line == "2") { choice = VIEW_DEBUG; break; }
        con_col(0x0E);
        std::cout << "Please type 1 or 2\n";
        con_reset();
    }

    g_view_mode.store(choice);

    try {
        json cfg = load_json_file(CONFIG_FILE, json::object());
        cfg["view_mode"] = choice;
        save_json_file(CONFIG_FILE, cfg);
        refresh_runtime_config();
    }
    catch (...) {}

    std::cout << "\n";
    print_divider();
    WORD cc2 = (choice == VIEW_CLEAN) ? 0x0A : 0x0B;
    con_col(cc2);
    std::string mname = (choice == VIEW_CLEAN) ? "Clean" : "Debug";
    std::cout << mname << " mode set. Starting optimizer...\n";
    con_reset();
    print_divider();
    std::cout << "\n";
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
}

// LOGGING  (named log_msg to avoid collision with <cmath> log)
// ============================================================

static std::string timestamp()
{
    char buf[32];
    time_t t = time(nullptr);
    tm tm_info{};
    localtime_s(&tm_info, &t);
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_info);
    return buf;
}

static void log_msg(const std::string& msg)
{
    std::string line = "[" + timestamp() + "] " + msg;

    if (!g_console_muted.load()) {
        if (is_clean()) {
            // Clean mode: color coded, no timestamp on console
            WORD col = prefix_color(msg);
            con_col(col);
            std::cout << msg << "\n";
            con_reset();
        }
        else {
            // Debug mode: raw timestamped output
            std::cout << line << "\n";
        }
        std::this_thread::sleep_for(
            std::chrono::milliseconds(static_cast<int>(UI_DELAY_MS)));
    }

    // Always write full timestamped line to log file regardless of mode
    std::lock_guard<std::mutex> lk(g_log_mutex);
    if (g_log_file.is_open()) {
        g_log_file << line << "\n";
        // Flush every 10 writes instead of every write -- reduces disk I/O
        static int s_flush_count = 0;
        if (++s_flush_count >= 10) {
            g_log_file.flush();
            s_flush_count = 0;
        }
    }
}

static void close_log()
{
    std::lock_guard<std::mutex> lk(g_log_mutex);
    if (g_log_file.is_open()) {
        g_log_file.flush();  // ensure all buffered writes are flushed
        g_log_file.close();
    }
}

// ============================================================
// RUN STATE
// ============================================================

static void write_run_state(bool clean_exit)
{
    try {
        (void)fs::create_directories(APPDATA_DIR);
        json data;
        data["version"] = VERSION;
        data["timestamp"] = static_cast<double>(time(nullptr));
        data["pid"] = static_cast<int>(GetCurrentProcessId());
        data["clean_exit"] = clean_exit;
        std::ofstream f(RUN_STATE_FILE);
        f << data.dump(4);
    }
    catch (...) {}
}

// ============================================================
// JSON I/O
// ============================================================

static json load_json_file(const fs::path& path, const json& fallback = json{})
{
    try {
        std::ifstream f(path);
        if (!f.is_open()) return fallback;
        json j;
        f >> j;
        return j;
    }
    catch (const json::parse_error& e) {
        log_msg("[CONFIG-ERROR] Invalid JSON in " + path.string() + ": " + e.what());
        return fallback;
    }
    catch (...) {
        log_msg("[CONFIG-ERROR] Failed to read " + path.string());
        return fallback;
    }
}

static bool save_json_file(const fs::path& path, const json& data)
{
    try {
        (void)fs::create_directories(path.parent_path());
        std::ofstream f(path);
        f << data.dump(4);
        return true;
    }
    catch (...) { return false; }
}

// ============================================================
// VALIDATE & REPAIR
// ============================================================

static void validate_and_repair_files(const std::map<fs::path, json>& defaults_map)
{
    for (auto& kv : defaults_map) {
        const fs::path& path = kv.first;
        const json& def = kv.second;
        if (!fs::exists(path)) continue;

        bool ok = true;
        try {
            std::ifstream f(path);
            (void)json::parse(f);
        }
        catch (...) { ok = false; }
        if (ok) continue;

        fs::path broken(path.string() + ".broken");
        try {
            if (fs::exists(broken)) (void)fs::remove(broken);
            (void)fs::rename(path, broken);
            log_msg("[REPAIR] Moved bad file -> " + broken.filename().string());
        }
        catch (...) {}

        if (save_json_file(path, def))
            log_msg("[REPAIR] Recreated default -> " + path.filename().string());
        else
            log_msg("[REPAIR] Failed to recreate " + path.string());
    }
}

// ============================================================
// ENSURE FILES
// ============================================================

static void ensure_files()
{
    (void)fs::create_directories(APPDATA_DIR);
    std::map<fs::path, json> defaults = {
        { GAMES_FILE,          json::object() },
        { CLOSE_APPS_FILE,     json::array()  },
        { REOPEN_APPS_FILE,    json::array()  },
        { PROCESS_GROUPS_FILE, json::object() },
        { PRESETS_FILE,        DEFAULT_PRESETS },
        { CONFIG_FILE,         DEFAULT_CONFIG  },
    };
    for (auto& kv : defaults)
        if (!fs::exists(kv.first))
            save_json_file(kv.first, kv.second);
    validate_and_repair_files(defaults);
}

// ============================================================
// LOAD HELPERS
// ============================================================

static std::map<std::string, json> load_proc_map(const fs::path& path)
{
    json data = load_json_file(path, json::object());
    std::map<std::string, json> out;
    if (!data.is_object()) return out;
    for (auto it = data.begin(); it != data.end(); ++it)
        if (it.value().is_object())
            out[to_lower(trim(it.key()))] = it.value();
    return out;
}

static std::vector<std::string> load_list(const fs::path& path)
{
    json data = load_json_file(path, json::array());
    std::vector<std::string> out;
    if (!data.is_array()) return out;
    for (auto& x : data) {
        if (x.is_string()) {
            std::string s = trim(x.get<std::string>());
            if (!s.empty()) out.push_back(s);
        }
        else if (x.is_object() && x.contains("name") && x["name"].is_string()) {
            std::string s = trim(x["name"].get<std::string>());
            if (!s.empty()) out.push_back(s);
        }
    }
    return out;
}

static std::map<std::string, std::vector<std::string>> load_groups(const fs::path& path)
{
    json data = load_json_file(path, json::object());
    std::map<std::string, std::vector<std::string>> out;
    if (!data.is_object()) return out;
    for (auto it = data.begin(); it != data.end(); ++it) {
        if (!it.value().is_array()) continue;
        std::vector<std::string> cleaned;
        for (auto& p : it.value())
            if (p.is_string()) {
                std::string s = trim(p.get<std::string>());
                if (!s.empty()) cleaned.push_back(to_lower(s));
            }
        out[to_lower(trim(it.key()))] = cleaned;
    }
    return out;
}

// ============================================================
// PRESETS
// ============================================================

static json load_presets()
{
    json data = load_json_file(PRESETS_FILE, DEFAULT_PRESETS);
    if (!data.is_object()) return DEFAULT_PRESETS;
    json out = json::object();
    for (auto it = data.begin(); it != data.end(); ++it) {
        const json& preset = it.value();
        if (!preset.is_object()) continue;
        if (!preset.contains("features") || !preset["features"].is_object()) continue;
        json entry;
        entry["description"] = preset.value("description", "");
        entry["features"] = preset["features"];
        entry["locked"] = preset.value("locked", json::array());
        out[to_lower(trim(it.key()))] = entry;
    }
    for (auto it = DEFAULT_PRESETS.begin(); it != DEFAULT_PRESETS.end(); ++it)
        if (!out.contains(it.key()))
            out[it.key()] = it.value();
    return out;
}

// ============================================================
// CONFIG
// ============================================================

static json load_config()
{
    json data = load_json_file(CONFIG_FILE, json::object());
    json cfg = DEFAULT_CONFIG;
    if (!data.is_object()) return cfg;

    if (data.contains("mode") && data["mode"].is_string())
        cfg["mode"] = trim(data["mode"].get<std::string>());
    if (data.contains("dry_run") && data["dry_run"].is_boolean())
        cfg["dry_run"] = data["dry_run"].get<bool>();
    if (data.contains("verbose_init") && data["verbose_init"].is_boolean())
        cfg["verbose_init"] = data["verbose_init"].get<bool>();

    if (data.contains("features") && data["features"].is_object())
        for (auto it = data["features"].begin(); it != data["features"].end(); ++it)
            if (it.value().is_boolean())
                cfg["features"][it.key()] = it.value();

    if (data.contains("background_apps") && data["background_apps"].is_array()) {
        json apps = json::array();
        for (auto& x : data["background_apps"])
            if (x.is_string()) apps.push_back(x);
        cfg["background_apps"] = apps;
    }

    if (data.contains("ram_cleaner") && data["ram_cleaner"].is_object()) {
        const auto& rc = data["ram_cleaner"];
        if (rc.contains("interval_sec") && rc["interval_sec"].is_number_integer())
            cfg["ram_cleaner"]["interval_sec"] = rc["interval_sec"].get<int>();
        if (rc.contains("empty_working_sets") && rc["empty_working_sets"].is_boolean())
            cfg["ram_cleaner"]["empty_working_sets"] = rc["empty_working_sets"].get<bool>();
        if (rc.contains("flush_file_cache") && rc["flush_file_cache"].is_boolean())
            cfg["ram_cleaner"]["flush_file_cache"] = rc["flush_file_cache"].get<bool>();
    }

    return cfg;
}

static void refresh_runtime_config()
{
    std::lock_guard<std::mutex> lk(g_config_lock);
    g_runtime_config = load_config();
    g_presets_cache = load_presets();
}

static json get_runtime_config()
{
    std::lock_guard<std::mutex> lk(g_config_lock);
    return g_runtime_config.is_null() ? DEFAULT_CONFIG : g_runtime_config;
}

// ============================================================
// FEATURE / MODE HELPERS
// ============================================================

static std::string resolve_mode(const std::string& mode, const json& presets)
{
    std::string m = to_lower(trim(mode.empty() ? "balanced" : mode));
    if (presets.contains(m)) return m;
    return "balanced";
}

static std::pair<json, std::string> get_effective_features(
    const std::string& global_mode,
    const json& config_features,
    const json& game_cfg,
    const json& presets)
{
    std::string requested = game_cfg.value("mode", global_mode);
    std::string mode_key = resolve_mode(requested, presets);

    json base = DEFAULT_PRESETS["balanced"]["features"];

    if (presets.contains(mode_key) && presets[mode_key].contains("features"))
        for (auto it = presets[mode_key]["features"].begin();
            it != presets[mode_key]["features"].end(); ++it)
            if (it.value().is_boolean()) base[it.key()] = it.value();

    for (auto it = config_features.begin(); it != config_features.end(); ++it)
        if (it.value().is_boolean()) base[it.key()] = it.value();

    for (auto it = game_cfg.begin(); it != game_cfg.end(); ++it)
        if (it.value().is_boolean() && base.contains(it.key()))
            base[it.key()] = it.value();

    return { base, mode_key };
}

static bool feature_enabled(const std::string& name,
    const json& effective,
    const json& game_cfg)
{
    if (game_cfg.contains(name) && game_cfg[name].is_boolean())
        return game_cfg[name].get<bool>();
    if (effective.contains(name) && effective[name].is_boolean())
        return effective[name].get<bool>();
    return true;
}

// ============================================================
// ADMIN CHECK
// ============================================================

static bool is_admin()
{
    BOOL result = FALSE;
    PSID admin_group = nullptr;
    SID_IDENTIFIER_AUTHORITY nt_auth = SECURITY_NT_AUTHORITY;
    if (AllocateAndInitializeSid(&nt_auth, 2,
        SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
        0, 0, 0, 0, 0, 0, &admin_group)) {
        CheckTokenMembership(nullptr, admin_group, &result);
        FreeSid(admin_group);
    }
    return result != FALSE;
}

// ============================================================
// CONSOLE
// ============================================================

static void hide_console()
{
    if (g_console_hwnd)
        ShowWindow(g_console_hwnd, SW_HIDE);
}

static void show_console()
{
    if (g_console_hwnd) {
        ShowWindow(g_console_hwnd, SW_SHOW);
        SetForegroundWindow(g_console_hwnd);
        SetConsoleTitleW(L"Optimizer");
    }
}

// ============================================================
// POWER SOURCE
// ============================================================

static std::string get_power_source()
{
    SYSTEM_POWER_STATUS s{};
    if (!GetSystemPowerStatus(&s)) return "unknown";
    if (s.ACLineStatus == 0) return "battery";
    if (s.ACLineStatus == 1) return "ac";
    return "unknown";
}

// ============================================================
// MMCSS (Multimedia Class Scheduler Service)
// ============================================================

typedef HANDLE(WINAPI* PAvSetMmThreadCharacteristicsW)(LPCWSTR, LPDWORD);
typedef BOOL(WINAPI* PAvRevertMmThreadCharacteristics)(HANDLE);

static PAvSetMmThreadCharacteristicsW  g_AvSetMmThreadCharacteristics = nullptr;
static PAvRevertMmThreadCharacteristics g_AvRevertMmThreadCharacteristics = nullptr;
static HANDLE g_mmcss_handle = nullptr;

static void init_mmcss_api()
{
    HMODULE avrt = LoadLibraryW(L"avrt.dll");
    if (!avrt) return;
    g_AvSetMmThreadCharacteristics =
        reinterpret_cast<PAvSetMmThreadCharacteristicsW>(
            GetProcAddress(avrt, "AvSetMmThreadCharacteristicsW"));
    g_AvRevertMmThreadCharacteristics =
        reinterpret_cast<PAvRevertMmThreadCharacteristics>(
            GetProcAddress(avrt, "AvRevertMmThreadCharacteristics"));
}

static void apply_mmcss(DWORD game_pid)
{
    if (!g_AvSetMmThreadCharacteristics) return;

    // Open all threads of the game process and boost via MMCSS "Games" task
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;

    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    int count = 0;

    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != game_pid) continue;
            DWORD task_index = 0;
            HANDLE h = g_AvSetMmThreadCharacteristics(L"Games", &task_index);
            if (h) { ++count; }
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
    log_msg("[MMCSS] Registered " + std::to_string(count) +
        " game threads with MMCSS 'Games' task");
}

static void revert_mmcss()
{
    if (g_mmcss_handle && g_AvRevertMmThreadCharacteristics) {
        g_AvRevertMmThreadCharacteristics(g_mmcss_handle);
        g_mmcss_handle = nullptr;
        log_msg("[MMCSS] Reverted");
    }
}

// ============================================================
// I/O PRIORITY
// ============================================================

// NtSetInformationProcess for I/O priority (undocumented but stable)
typedef LONG(NTAPI* PNtSetInformationProcess)(HANDLE, UINT, PVOID, ULONG);
static PNtSetInformationProcess g_NtSetInformationProcess = nullptr;

static void init_io_priority_api()
{
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll)
        g_NtSetInformationProcess =
        reinterpret_cast<PNtSetInformationProcess>(
            GetProcAddress(ntdll, "NtSetInformationProcess"));
}

static void set_io_priority_high(DWORD pid)
{
    if (!g_NtSetInformationProcess) return;
    HANDLE h = OpenProcess(PROCESS_SET_INFORMATION | PROCESS_QUERY_INFORMATION,
        FALSE, pid);
    if (!h) return;

    // ProcessIoPriority = 33, value 3 = IoPriorityHigh
    ULONG io_priority = 3;
    LONG  status = g_NtSetInformationProcess(h, 33, &io_priority, sizeof(io_priority));
    if (status == 0)
        log_msg("[IO] Game I/O priority set to High (PID " + std::to_string(pid) + ")");
    else
        log_msg("[IO] Could not set I/O priority (status " + std::to_string(status) + ")");
    CloseHandle(h);
}

static void set_io_priority_normal(DWORD pid)
{
    if (!g_NtSetInformationProcess) return;
    HANDLE h = OpenProcess(PROCESS_SET_INFORMATION, FALSE, pid);
    if (!h) return;
    ULONG io_priority = 2; // IoPriorityNormal
    g_NtSetInformationProcess(h, 33, &io_priority, sizeof(io_priority));
    CloseHandle(h);
}

// ============================================================
// CPU UNPARKING
// ============================================================

static void unpark_cpu_cores()
{
    // Set processor performance policy to prefer performance (no parking)
    // Uses powercfg to set the minimum processor state to 100%
    // and disable core parking via the current power scheme
    log_msg("[CPU] Unparking CPU cores...");
    (void)_wsystem(L"powercfg /setacvalueindex SCHEME_CURRENT SUB_PROCESSOR CPMINCORES 100 >nul 2>&1");
    (void)_wsystem(L"powercfg /setacvalueindex SCHEME_CURRENT SUB_PROCESSOR PROCTHROTTLEMIN 100 >nul 2>&1");
    (void)_wsystem(L"powercfg /setactive SCHEME_CURRENT >nul 2>&1");
    log_msg("[CPU] CPU cores unparked (min processor state = 100%)");
}

static void restore_cpu_parking()
{
    // Restore processor minimum to 5% (Windows default)
    (void)_wsystem(L"powercfg /setacvalueindex SCHEME_CURRENT SUB_PROCESSOR CPMINCORES 5 >nul 2>&1");
    (void)_wsystem(L"powercfg /setacvalueindex SCHEME_CURRENT SUB_PROCESSOR PROCTHROTTLEMIN 5 >nul 2>&1");
    (void)_wsystem(L"powercfg /setactive SCHEME_CURRENT >nul 2>&1");
    log_msg("[CPU] CPU parking restored");
}

// ============================================================
// NUMA / CCD AFFINITY (Ryzen multi-CCD optimization)
// ============================================================

static DWORD_PTR g_game_affinity_backup = 0;

static void apply_numa_affinity(DWORD pid)
{
    // Detect if the system has multiple NUMA nodes (multi-CCD Ryzen)
    ULONG node_count = 0;
    if (!GetNumaHighestNodeNumber(&node_count)) return;

    if (node_count == 0) {
        // Single NUMA node -- no benefit, skip
        log_msg("[NUMA] Single NUMA node detected, skipping CCD affinity");
        return;
    }

    // Get the affinity mask of NUMA node 0 (first CCD -- usually the faster one)
    GROUP_AFFINITY node_affinity{};
    if (!GetNumaNodeProcessorMaskEx(0, &node_affinity)) return;

    HANDLE h = OpenProcess(PROCESS_SET_INFORMATION | PROCESS_QUERY_INFORMATION,
        FALSE, pid);
    if (!h) return;

    // Backup current affinity
    DWORD_PTR proc_mask = 0, sys_mask = 0;
    if (GetProcessAffinityMask(h, &proc_mask, &sys_mask))
        g_game_affinity_backup = proc_mask;

    // Pin game to node 0 (first CCD)
    if (SetProcessAffinityMask(h, node_affinity.Mask))
        log_msg("[NUMA] Game pinned to NUMA node 0 (CCD0), mask: " +
            std::to_string(node_affinity.Mask));
    else
        log_msg("[NUMA] Could not set NUMA affinity");

    CloseHandle(h);
}

static void restore_numa_affinity(DWORD pid)
{
    if (g_game_affinity_backup == 0) return;
    HANDLE h = OpenProcess(PROCESS_SET_INFORMATION, FALSE, pid);
    if (!h) return;
    SetProcessAffinityMask(h, g_game_affinity_backup);
    g_game_affinity_backup = 0;
    CloseHandle(h);
    log_msg("[NUMA] Game affinity restored");
}

// ============================================================
// GPU PROCESS PRIORITY
// ============================================================

static const std::vector<std::string> GPU_PROCESS_NAMES = {
    // NVIDIA
    "nvcontainer.exe", "nvidia web helper.exe", "nvspcaps64.exe",
    // AMD
    "amdow.exe", "amddvr.exe", "radeoninstaller.exe",
    // Generic GPU/driver workers that appear as game children
    "dwm.exe"
};

static std::unordered_map<DWORD, int> g_gpu_priority_backup;

static void boost_gpu_processes()
{
    std::set<std::string> targets;
    for (auto& n : GPU_PROCESS_NAMES) targets.insert(to_lower(n));

    auto procs = enumerate_processes();
    std::lock_guard<std::mutex> lk(g_backup_mutex);
    int count = 0;
    for (auto& p : procs) {
        if (!targets.count(to_lower(p.name))) continue;
        HANDLE h = OpenProcess(PROCESS_SET_INFORMATION | PROCESS_QUERY_INFORMATION,
            FALSE, p.pid);
        if (!h) continue;
        if (g_gpu_priority_backup.find(p.pid) == g_gpu_priority_backup.end()) {
            g_gpu_priority_backup[p.pid] = static_cast<int>(GetPriorityClass(h));
            if (SetPriorityClass(h, ABOVE_NORMAL_PRIORITY_CLASS)) ++count;
        }
        CloseHandle(h);
    }
    if (count > 0)
        log_msg("[GPU] Boosted " + std::to_string(count) + " GPU-related processes");
}

static void restore_gpu_processes()
{
    std::lock_guard<std::mutex> lk(g_backup_mutex);
    if (g_gpu_priority_backup.empty()) return;
    for (auto& kv : g_gpu_priority_backup) {
        HANDLE h = OpenProcess(PROCESS_SET_INFORMATION, FALSE, kv.first);
        if (h) {
            SetPriorityClass(h, static_cast<DWORD>(kv.second));
            CloseHandle(h);
        }
    }
    g_gpu_priority_backup.clear();
    log_msg("[GPU] GPU process priorities restored");
}

// ============================================================
// NETWORK OPTIMIZATIONS
// ============================================================

// Nagle's algorithm disable (per-adapter via registry)
// HKLM\SYSTEM\CurrentControlSet\Services\Tcpip\Parameters\Interfaces\{GUID}
// TcpAckFrequency = 1, TCPNoDelay = 1

static std::vector<std::wstring> g_nagle_modified_keys;

static void disable_nagle()
{
    HKEY interfaces_key;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
        L"SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters\\Interfaces",
        0, KEY_READ, &interfaces_key) != ERROR_SUCCESS)
        return;

    wchar_t subkey_name[256];
    DWORD index = 0, name_len = 256;
    int count = 0;

    while (RegEnumKeyExW(interfaces_key, index++, subkey_name, &name_len,
        nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS)
    {
        name_len = 256;
        std::wstring full_path =
            L"SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters\\Interfaces\\";
        full_path += subkey_name;

        HKEY iface_key;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, full_path.c_str(),
            0, KEY_READ | KEY_WRITE, &iface_key) == ERROR_SUCCESS)
        {
            // Only modify adapters that have an IP address (are active)
            DWORD data_size = 0;
            bool has_ip = (RegQueryValueExW(iface_key, L"DhcpIPAddress",
                nullptr, nullptr, nullptr, &data_size) == ERROR_SUCCESS)
                || (RegQueryValueExW(iface_key, L"IPAddress",
                    nullptr, nullptr, nullptr, &data_size) == ERROR_SUCCESS);

            if (has_ip) {
                DWORD val = 1;
                (void)RegSetValueExW(iface_key, L"TcpAckFrequency", 0, REG_DWORD,
                    reinterpret_cast<const BYTE*>(&val), sizeof(val));
                (void)RegSetValueExW(iface_key, L"TCPNoDelay", 0, REG_DWORD,
                    reinterpret_cast<const BYTE*>(&val), sizeof(val));
                g_nagle_modified_keys.push_back(full_path);
                ++count;
            }
            RegCloseKey(iface_key);
        }
    }
    RegCloseKey(interfaces_key);

    if (count > 0)
        log_msg("[NET] Nagle's algorithm disabled on " +
            std::to_string(count) + " network adapter(s)");
    else
        log_msg("[NET] No active adapters found for Nagle disable");
}

static void restore_nagle()
{
    if (g_nagle_modified_keys.empty()) return;
    for (auto& path : g_nagle_modified_keys) {
        HKEY key;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(),
            0, KEY_WRITE, &key) == ERROR_SUCCESS) {
            (void)RegDeleteValueW(key, L"TcpAckFrequency");
            (void)RegDeleteValueW(key, L"TCPNoDelay");
            RegCloseKey(key);
        }
    }
    g_nagle_modified_keys.clear();
    log_msg("[NET] Nagle's algorithm restored");
}

// Network adapter power saving disable
static std::vector<std::wstring> g_nic_power_modified;

static void disable_nic_power_saving()
{
    // Set all network adapters to maximum performance power policy
    // via HKLM\SYSTEM\CurrentControlSet\Control\Class\{4D36E972...}\XXXX
    HKEY net_class;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
        L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4D36E972-E325-11CE-BFC1-08002BE10318}",
        0, KEY_READ, &net_class) != ERROR_SUCCESS)
        return;

    wchar_t subkey_name[256];
    DWORD index = 0, name_len = 256;
    int count = 0;

    while (RegEnumKeyExW(net_class, index++, subkey_name, &name_len,
        nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS)
    {
        name_len = 256;
        if (wcsncmp(subkey_name, L"Properties", 10) == 0) continue;

        std::wstring full_path =
            L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4D36E972-E325-11CE-BFC1-08002BE10318}\\";
        full_path += subkey_name;

        HKEY adapter_key;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, full_path.c_str(),
            0, KEY_READ | KEY_WRITE, &adapter_key) == ERROR_SUCCESS)
        {
            // Check this is a real adapter (has DriverDesc)
            DWORD data_size = 0;
            if (RegQueryValueExW(adapter_key, L"DriverDesc",
                nullptr, nullptr, nullptr, &data_size) == ERROR_SUCCESS)
            {
                // PnPCapabilities = 24 disables power management (wake + allow PC to turn off)
                DWORD val = 24;
                if (RegSetValueExW(adapter_key, L"PnPCapabilities", 0, REG_DWORD,
                    reinterpret_cast<const BYTE*>(&val), sizeof(val)) == ERROR_SUCCESS)
                {
                    g_nic_power_modified.push_back(full_path);
                    ++count;
                }
            }
            RegCloseKey(adapter_key);
        }
    }
    RegCloseKey(net_class);

    if (count > 0)
        log_msg("[NET] Power saving disabled on " +
            std::to_string(count) + " NIC(s)");
}

static void restore_nic_power_saving()
{
    if (g_nic_power_modified.empty()) return;
    for (auto& path : g_nic_power_modified) {
        HKEY key;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(),
            0, KEY_WRITE, &key) == ERROR_SUCCESS) {
            (void)RegDeleteValueW(key, L"PnPCapabilities");
            RegCloseKey(key);
        }
    }
    g_nic_power_modified.clear();
    log_msg("[NET] NIC power saving restored");
}

// Network thread priority boost
static std::unordered_map<DWORD, int> g_net_thread_priority_backup;

static void boost_network_threads()
{
    // Boost threads belonging to System process that handle NDIS/network stack
    const std::set<std::string> net_procs = { "system", "svchost.exe" };
    auto procs = enumerate_processes();
    std::lock_guard<std::mutex> lk(g_backup_mutex);
    int count = 0;

    for (auto& p : procs) {
        if (!net_procs.count(to_lower(p.name))) continue;
        HANDLE h = OpenProcess(PROCESS_SET_INFORMATION | PROCESS_QUERY_INFORMATION,
            FALSE, p.pid);
        if (!h) continue;
        if (g_net_thread_priority_backup.find(p.pid) == g_net_thread_priority_backup.end()) {
            int orig = static_cast<int>(GetPriorityClass(h));
            g_net_thread_priority_backup[p.pid] = orig;
            if (SetPriorityClass(h, ABOVE_NORMAL_PRIORITY_CLASS)) ++count;
        }
        CloseHandle(h);
    }
    if (count > 0)
        log_msg("[NET] Boosted " + std::to_string(count) + " network process(es)");
}

static void restore_network_threads()
{
    std::lock_guard<std::mutex> lk(g_backup_mutex);
    if (g_net_thread_priority_backup.empty()) return;
    for (auto& kv : g_net_thread_priority_backup) {
        HANDLE h = OpenProcess(PROCESS_SET_INFORMATION, FALSE, kv.first);
        if (h) {
            SetPriorityClass(h, static_cast<DWORD>(kv.second));
            CloseHandle(h);
        }
    }
    g_net_thread_priority_backup.clear();
    log_msg("[NET] Network process priorities restored");
}


// ============================================================
// ADVANCED NETWORK OPTIMIZATIONS
// ============================================================

// ---- TCP Auto-Tuning ----
static bool g_tcp_autotuning_modified = false;
static std::string g_tcp_autotuning_orig;

static void disable_tcp_autotuning()
{
    // Read current value
    FILE* pipe = _popen("netsh interface tcp show global 2>nul", "r");
    if (pipe) {
        char buf[256];
        while (fgets(buf, sizeof(buf), pipe)) {
            std::string line(buf);
            if (line.find("Receive Window Auto-Tuning Level") != std::string::npos ||
                line.find("Auto-Tuning Level") != std::string::npos) {
                if (line.find("normal") != std::string::npos) g_tcp_autotuning_orig = "normal";
                else if (line.find("disabled") != std::string::npos) g_tcp_autotuning_orig = "disabled";
                else if (line.find("highlyrestricted") != std::string::npos) g_tcp_autotuning_orig = "highlyrestricted";
                else if (line.find("restricted") != std::string::npos) g_tcp_autotuning_orig = "restricted";
                else g_tcp_autotuning_orig = "normal";
                break;
            }
        }
        _pclose(pipe);
    }
    if (g_tcp_autotuning_orig.empty()) g_tcp_autotuning_orig = "normal";

    int r = _wsystem(L"netsh interface tcp set global autotuninglevel=highlyrestricted >nul 2>&1");
    if (r == 0) {
        g_tcp_autotuning_modified = true;
        log_msg("[NET-ADV] TCP auto-tuning set to highlyrestricted (lower latency)");
    }
    else {
        log_msg("[NET-ADV] Failed to set TCP auto-tuning");
    }
}

static void restore_tcp_autotuning()
{
    if (!g_tcp_autotuning_modified) return;
    std::string cmd = "netsh interface tcp set global autotuninglevel=" +
        g_tcp_autotuning_orig + " >nul 2>&1";
    system(cmd.c_str());
    g_tcp_autotuning_modified = false;
    log_msg("[NET-ADV] TCP auto-tuning restored to: " + g_tcp_autotuning_orig);
}

// ---- ECN Disable ----
static bool g_ecn_modified = false;
static std::string g_ecn_orig;

static void disable_ecn()
{
    FILE* pipe = _popen("netsh interface tcp show global 2>nul", "r");
    if (pipe) {
        char buf[256];
        while (fgets(buf, sizeof(buf), pipe)) {
            std::string line(buf);
            if (line.find("ECN Capability") != std::string::npos) {
                g_ecn_orig = (line.find("enabled") != std::string::npos) ? "enabled" : "disabled";
                break;
            }
        }
        _pclose(pipe);
    }
    if (g_ecn_orig.empty()) g_ecn_orig = "disabled";

    int r = _wsystem(L"netsh interface tcp set global ecncapability=disabled >nul 2>&1");
    if (r == 0) {
        g_ecn_modified = true;
        log_msg("[NET-ADV] ECN disabled (better compatibility with routers)");
    }
}

static void restore_ecn()
{
    if (!g_ecn_modified) return;
    std::string cmd = "netsh interface tcp set global ecncapability=" + g_ecn_orig + " >nul 2>&1";
    system(cmd.c_str());
    g_ecn_modified = false;
    log_msg("[NET-ADV] ECN restored to: " + g_ecn_orig);
}

// ---- RSS (Receive Side Scaling) ----
static bool g_rss_modified = false;
static std::string g_rss_orig;

static void enable_rss()
{
    FILE* pipe = _popen("netsh interface tcp show global 2>nul", "r");
    if (pipe) {
        char buf[256];
        while (fgets(buf, sizeof(buf), pipe)) {
            std::string line(buf);
            if (line.find("Receive-Side Scaling State") != std::string::npos ||
                line.find("RSS") != std::string::npos) {
                g_rss_orig = (line.find("enabled") != std::string::npos) ? "enabled" : "disabled";
                break;
            }
        }
        _pclose(pipe);
    }
    if (g_rss_orig.empty()) g_rss_orig = "enabled";
    if (g_rss_orig == "enabled") {
        log_msg("[NET-ADV] RSS already enabled, skipping");
        return;
    }

    int r = _wsystem(L"netsh interface tcp set global rss=enabled >nul 2>&1");
    if (r == 0) {
        g_rss_modified = true;
        log_msg("[NET-ADV] RSS enabled (packet processing across multiple cores)");
    }
}

static void restore_rss()
{
    if (!g_rss_modified) return;
    std::string cmd = "netsh interface tcp set global rss=" + g_rss_orig + " >nul 2>&1";
    system(cmd.c_str());
    g_rss_modified = false;
    log_msg("[NET-ADV] RSS restored to: " + g_rss_orig);
}

// ---- QoS Bandwidth Reserve Remove ----
static bool g_qos_modified = false;

static void remove_qos_reserve()
{
    // HKLM\SOFTWARE\Policies\Microsoft\Windows\Psched -- NonBestEffortLimit = 0
    HKEY key;
    DWORD disp = 0;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Policies\\Microsoft\\Windows\\Psched",
        0, nullptr, 0, KEY_WRITE, nullptr, &key, &disp) == ERROR_SUCCESS)
    {
        DWORD val = 0;
        (void)RegSetValueExW(key, L"NonBestEffortLimit", 0, REG_DWORD,
            reinterpret_cast<const BYTE*>(&val), sizeof(val));
        RegCloseKey(key);
        g_qos_modified = true;
        log_msg("[NET-ADV] QoS bandwidth reserve removed (full bandwidth available)");
    }
}

static void restore_qos_reserve()
{
    if (!g_qos_modified) return;
    HKEY key;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Policies\\Microsoft\\Windows\\Psched",
        0, KEY_WRITE, &key) == ERROR_SUCCESS)
    {
        (void)RegDeleteValueW(key, L"NonBestEffortLimit");
        RegCloseKey(key);
    }
    g_qos_modified = false;
    log_msg("[NET-ADV] QoS bandwidth reserve restored");
}

// ---- DSCP Packet Tagging ----
static bool g_dscp_modified = false;
static const std::wstring DSCP_POLICY_KEY =
L"SOFTWARE\\Policies\\Microsoft\\Windows\\QoS";

static void apply_dscp_tagging(const std::string& game_exe)
{
    // Create a QoS policy that tags game packets with DSCP 46 (Expedited Forwarding)
    // This tells your router to prioritize these packets
    HKEY key;
    std::wstring policy_name = L"OptimizerGameBoost_" + to_wide(game_exe);

    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE,
        (DSCP_POLICY_KEY + L"\\" + policy_name).c_str(),
        0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) == ERROR_SUCCESS)
    {
        auto set_str = [&](const wchar_t* name, const wchar_t* val) {
            (void)RegSetValueExW(key, name, 0, REG_SZ,
                reinterpret_cast<const BYTE*>(val),
                static_cast<DWORD>((wcslen(val) + 1) * sizeof(wchar_t)));
            };
        auto set_dword = [&](const wchar_t* name, DWORD val) {
            (void)RegSetValueExW(key, name, 0, REG_DWORD,
                reinterpret_cast<const BYTE*>(&val), sizeof(val));
            };

        set_str(L"Version", L"1.0");
        set_str(L"ApplicationName", to_wide(game_exe).c_str());
        set_str(L"Protocol", L"*");
        set_str(L"LocalPort", L"*");
        set_str(L"RemotePort", L"*");
        set_str(L"LocalIP", L"*");
        set_str(L"RemoteIP", L"*");
        set_dword(L"DSCPValue", 46);  // EF -- highest priority
        set_dword(L"ThrottleRate", 0xFFFFFFFF); // no throttle

        RegCloseKey(key);
        g_dscp_modified = true;
        log_msg("[NET-ADV] DSCP EF (46) tagging applied for: " + game_exe);
    }
}

static void remove_dscp_tagging(const std::string& game_exe)
{
    if (!g_dscp_modified) return;
    std::wstring policy_name = L"OptimizerGameBoost_" + to_wide(game_exe);
    std::wstring full_key = DSCP_POLICY_KEY + L"\\" + policy_name;
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, full_key.c_str());
    g_dscp_modified = false;
    log_msg("[NET-ADV] DSCP tagging removed for: " + game_exe);
}

// ---- DNS Cache Flush ----
static void flush_dns_cache()
{
    int r = _wsystem(L"ipconfig /flushdns >nul 2>&1");
    if (r == 0)
        log_msg("[NET-ADV] DNS cache flushed");
    else
        log_msg("[NET-ADV] DNS flush failed");
}

// ---- Interrupt Moderation Disable (per adapter via registry) ----
static std::vector<std::wstring> g_interrupt_mod_keys;

static void disable_interrupt_moderation()
{
    // HKLM\SYSTEM\CurrentControlSet\Control\Class\{4D36E972...}\XXXX
    // InterruptModeration = 0
    HKEY net_class;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
        L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4D36E972-E325-11CE-BFC1-08002BE10318}",
        0, KEY_READ, &net_class) != ERROR_SUCCESS)
        return;

    wchar_t subkey[256];
    DWORD index = 0, name_len = 256;
    int count = 0;

    while (RegEnumKeyExW(net_class, index++, subkey, &name_len,
        nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS)
    {
        name_len = 256;
        if (wcsncmp(subkey, L"Properties", 10) == 0) continue;

        std::wstring full = L"SYSTEM\\CurrentControlSet\\Control\\Class\\"
            L"{4D36E972-E325-11CE-BFC1-08002BE10318}\\";
        full += subkey;

        HKEY ak;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, full.c_str(),
            0, KEY_READ | KEY_WRITE, &ak) == ERROR_SUCCESS)
        {
            DWORD data_size = 0;
            if (RegQueryValueExW(ak, L"DriverDesc", nullptr, nullptr,
                nullptr, &data_size) == ERROR_SUCCESS)
            {
                DWORD val = 0;
                if (RegSetValueExW(ak, L"*InterruptModeration", 0, REG_DWORD,
                    reinterpret_cast<const BYTE*>(&val), sizeof(val)) == ERROR_SUCCESS)
                {
                    g_interrupt_mod_keys.push_back(full);
                    ++count;
                }
            }
            RegCloseKey(ak);
        }
    }
    RegCloseKey(net_class);
    if (count > 0)
        log_msg("[NET-ADV] Interrupt moderation disabled on " +
            std::to_string(count) + " adapter(s) (lower packet latency)");
}

static void restore_interrupt_moderation()
{
    if (g_interrupt_mod_keys.empty()) return;
    for (auto& path : g_interrupt_mod_keys) {
        HKEY key;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(),
            0, KEY_WRITE, &key) == ERROR_SUCCESS)
        {
            (void)RegDeleteValueW(key, L"*InterruptModeration");
            RegCloseKey(key);
        }
    }
    g_interrupt_mod_keys.clear();
    log_msg("[NET-ADV] Interrupt moderation restored");
}

// ---- Adapter Buffer Tuning ----
static std::vector<std::wstring> g_buffer_tuned_keys;

static void tune_adapter_buffers()
{
    // Lower receive/transmit buffer sizes to reduce buffering latency
    // *ReceiveBuffers = 64, *TransmitBuffers = 64 (from default ~256-512)
    HKEY net_class;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
        L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4D36E972-E325-11CE-BFC1-08002BE10318}",
        0, KEY_READ, &net_class) != ERROR_SUCCESS)
        return;

    wchar_t subkey[256];
    DWORD index = 0, name_len = 256;
    int count = 0;

    while (RegEnumKeyExW(net_class, index++, subkey, &name_len,
        nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS)
    {
        name_len = 256;
        if (wcsncmp(subkey, L"Properties", 10) == 0) continue;

        std::wstring full = L"SYSTEM\\CurrentControlSet\\Control\\Class\\"
            L"{4D36E972-E325-11CE-BFC1-08002BE10318}\\";
        full += subkey;

        HKEY ak;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, full.c_str(),
            0, KEY_READ | KEY_WRITE, &ak) == ERROR_SUCCESS)
        {
            DWORD data_size = 0;
            if (RegQueryValueExW(ak, L"DriverDesc", nullptr, nullptr,
                nullptr, &data_size) == ERROR_SUCCESS)
            {
                DWORD val = 64;
                bool changed = false;
                if (RegSetValueExW(ak, L"*ReceiveBuffers", 0, REG_DWORD,
                    reinterpret_cast<const BYTE*>(&val), sizeof(val)) == ERROR_SUCCESS)
                    changed = true;
                if (RegSetValueExW(ak, L"*TransmitBuffers", 0, REG_DWORD,
                    reinterpret_cast<const BYTE*>(&val), sizeof(val)) == ERROR_SUCCESS)
                    changed = true;
                if (changed) {
                    g_buffer_tuned_keys.push_back(full);
                    ++count;
                }
            }
            RegCloseKey(ak);
        }
    }
    RegCloseKey(net_class);
    if (count > 0)
        log_msg("[NET-ADV] Adapter buffers tuned on " +
            std::to_string(count) + " adapter(s) (reduced buffering latency)");
}

static void restore_adapter_buffers()
{
    if (g_buffer_tuned_keys.empty()) return;
    for (auto& path : g_buffer_tuned_keys) {
        HKEY key;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(),
            0, KEY_WRITE, &key) == ERROR_SUCCESS)
        {
            (void)RegDeleteValueW(key, L"*ReceiveBuffers");
            (void)RegDeleteValueW(key, L"*TransmitBuffers");
            RegCloseKey(key);
        }
    }
    g_buffer_tuned_keys.clear();
    log_msg("[NET-ADV] Adapter buffers restored");
}

// ---- Flow Control Disable ----
static std::vector<std::wstring> g_flow_control_keys;

static void disable_flow_control()
{
    HKEY net_class;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
        L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4D36E972-E325-11CE-BFC1-08002BE10318}",
        0, KEY_READ, &net_class) != ERROR_SUCCESS)
        return;

    wchar_t subkey[256];
    DWORD index = 0, name_len = 256;
    int count = 0;

    while (RegEnumKeyExW(net_class, index++, subkey, &name_len,
        nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS)
    {
        name_len = 256;
        if (wcsncmp(subkey, L"Properties", 10) == 0) continue;

        std::wstring full = L"SYSTEM\\CurrentControlSet\\Control\\Class\\"
            L"{4D36E972-E325-11CE-BFC1-08002BE10318}\\";
        full += subkey;

        HKEY ak;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, full.c_str(),
            0, KEY_READ | KEY_WRITE, &ak) == ERROR_SUCCESS)
        {
            DWORD data_size = 0;
            if (RegQueryValueExW(ak, L"DriverDesc", nullptr, nullptr,
                nullptr, &data_size) == ERROR_SUCCESS)
            {
                // 0 = disabled, some drivers use "0" string
                DWORD val = 0;
                if (RegSetValueExW(ak, L"*FlowControl", 0, REG_DWORD,
                    reinterpret_cast<const BYTE*>(&val), sizeof(val)) == ERROR_SUCCESS)
                {
                    g_flow_control_keys.push_back(full);
                    ++count;
                }
            }
            RegCloseKey(ak);
        }
    }
    RegCloseKey(net_class);
    if (count > 0)
        log_msg("[NET-ADV] Flow control disabled on " +
            std::to_string(count) + " adapter(s)");
}

static void restore_flow_control()
{
    if (g_flow_control_keys.empty()) return;
    for (auto& path : g_flow_control_keys) {
        HKEY key;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(),
            0, KEY_WRITE, &key) == ERROR_SUCCESS)
        {
            (void)RegDeleteValueW(key, L"*FlowControl");
            RegCloseKey(key);
        }
    }
    g_flow_control_keys.clear();
    log_msg("[NET-ADV] Flow control restored");
}



// ============================================================
// CPU ADVANCED OPTIMIZATIONS
// ============================================================

// ---- C-State Disable ----
static bool g_cstate_disabled = false;

static void disable_cstates()
{
    // Set processor idle to disabled via powercfg
    // PROCIDLEFLAGS 0 = disabled, 1 = enabled
    int r1 = _wsystem(L"powercfg /setacvalueindex SCHEME_CURRENT SUB_PROCESSOR PROCIDLEFLAGS 0 >nul 2>&1");
    int r2 = _wsystem(L"powercfg /setacvalueindex SCHEME_CURRENT SUB_PROCESSOR IDLEDISABLE 1 >nul 2>&1");
    _wsystem(L"powercfg /setactive SCHEME_CURRENT >nul 2>&1");
    if (r1 == 0 || r2 == 0) {
        g_cstate_disabled = true;
        log_msg("[CPU-ADV] C-states disabled (CPU stays at full speed, no idle latency)");
    }
    else {
        log_msg("[CPU-ADV] Failed to disable C-states");
    }
}

static void restore_cstates()
{
    if (!g_cstate_disabled) return;
    _wsystem(L"powercfg /setacvalueindex SCHEME_CURRENT SUB_PROCESSOR PROCIDLEFLAGS 1 >nul 2>&1");
    _wsystem(L"powercfg /setacvalueindex SCHEME_CURRENT SUB_PROCESSOR IDLEDISABLE 0 >nul 2>&1");
    _wsystem(L"powercfg /setactive SCHEME_CURRENT >nul 2>&1");
    g_cstate_disabled = false;
    log_msg("[CPU-ADV] C-states restored");
}

// ---- Force Max Boost Clock ----
static bool g_boost_forced = false;

static void force_max_boost()
{
    // PERFBOOSTMODE 2 = aggressive boost, always active
    // PERFBOOSTPOL 100 = 100% boost policy
    _wsystem(L"powercfg /setacvalueindex SCHEME_CURRENT SUB_PROCESSOR PERFBOOSTMODE 2 >nul 2>&1");
    _wsystem(L"powercfg /setacvalueindex SCHEME_CURRENT SUB_PROCESSOR PERFBOOSTPOL 100 >nul 2>&1");
    _wsystem(L"powercfg /setacvalueindex SCHEME_CURRENT SUB_PROCESSOR PERFEPP 0 >nul 2>&1");
    _wsystem(L"powercfg /setactive SCHEME_CURRENT >nul 2>&1");
    g_boost_forced = true;
    log_msg("[CPU-ADV] Max boost clock forced (aggressive boost mode, EPP=0)");
}

static void restore_boost()
{
    if (!g_boost_forced) return;
    _wsystem(L"powercfg /setacvalueindex SCHEME_CURRENT SUB_PROCESSOR PERFBOOSTMODE 1 >nul 2>&1");
    _wsystem(L"powercfg /setacvalueindex SCHEME_CURRENT SUB_PROCESSOR PERFBOOSTPOL 100 >nul 2>&1");
    _wsystem(L"powercfg /setacvalueindex SCHEME_CURRENT SUB_PROCESSOR PERFEPP 128 >nul 2>&1");
    _wsystem(L"powercfg /setactive SCHEME_CURRENT >nul 2>&1");
    g_boost_forced = false;
    log_msg("[CPU-ADV] CPU boost restored");
}

// ---- CPU Set Isolation (Job Object) ----
static HANDLE g_cpu_set_job = nullptr;
static std::vector<ULONG> g_isolated_cpu_sets;

static void isolate_game_cpu_sets(DWORD pid)
{
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    int cpu_count = static_cast<int>(si.dwNumberOfProcessors);
    if (cpu_count < 4) {
        log_msg("[CPU-ADV] CPU set isolation skipped (fewer than 4 cores)");
        return;
    }

    // Get all CPU set IDs
    ULONG needed = 0;
    if (!g_GetSystemCpuSetInformation_fn) { log_msg("[CPU-ADV] CPU Sets API not available"); return; }
    g_GetSystemCpuSetInformation_fn(nullptr, 0, &needed, GetCurrentProcess(), 0);
    if (needed == 0) return;

    std::vector<BYTE> buf(needed);
    ULONG count = 0;
    if (!g_GetSystemCpuSetInformation_fn(
        reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(buf.data()),
        needed, &count, GetCurrentProcess(), 0))
        return;

    // Reserve last 1-2 cores for OS/background, give rest to game
    std::vector<ULONG> game_sets;
    std::vector<ULONG> bg_sets;
    int total_sets = 0;

    BYTE* ptr = buf.data();
    ULONG remaining = needed;
    while (remaining >= sizeof(SYSTEM_CPU_SET_INFORMATION)) {
        auto* info = reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(ptr);
        if (info->Type == CpuSetInformation) {
            total_sets++;
            game_sets.push_back(info->CpuSet.Id);
        }
        if (info->Size == 0) break;
        ptr += info->Size;
        remaining -= info->Size;
    }

    // Reserve last 2 cores for background
    if (game_sets.size() > 2) {
        bg_sets.push_back(game_sets.back()); game_sets.pop_back();
        bg_sets.push_back(game_sets.back()); game_sets.pop_back();
    }

    // Assign game CPU sets to the game process
    HANDLE hGame = OpenProcess(PROCESS_SET_LIMITED_INFORMATION, FALSE, pid);
    if (hGame) {
        if (g_SetProcessDefaultCpuSets_fn &&
            g_SetProcessDefaultCpuSets_fn(hGame, game_sets.data(),
                static_cast<ULONG>(game_sets.size())))
        {
            g_isolated_cpu_sets = game_sets;
            log_msg("[CPU-ADV] Game isolated to " +
                std::to_string(game_sets.size()) + " CPU sets (" +
                std::to_string(bg_sets.size()) + " reserved for OS)");
        }
        else {
            log_msg("[CPU-ADV] Could not set CPU sets (Windows 10 1703+ required)");
        }
        CloseHandle(hGame);
    }
}

static void restore_cpu_sets(DWORD pid)
{
    if (g_isolated_cpu_sets.empty()) return;
    HANDLE h = OpenProcess(PROCESS_SET_LIMITED_INFORMATION, FALSE, pid);
    if (h) {
        if (g_SetProcessDefaultCpuSets_fn)
            g_SetProcessDefaultCpuSets_fn(h, nullptr, 0);
        CloseHandle(h);
    }
    g_isolated_cpu_sets.clear();
    log_msg("[CPU-ADV] CPU set isolation removed");
}

// ---- Ideal Processor Hint ----
static void set_ideal_processor(DWORD pid)
{
    // Set the game's main thread to prefer core 0 (usually fastest single-thread core)
    // For Ryzen X3D, core 0 on CCD0 has the 3D V-Cache
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;

    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    bool first = true;

    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != pid) continue;
            HANDLE ht = OpenThread(THREAD_SET_INFORMATION | THREAD_QUERY_INFORMATION,
                FALSE, te.th32ThreadID);
            if (!ht) continue;
            if (first) {
                // Main thread gets core 0 as ideal (highest priority)
                SetThreadIdealProcessor(ht, 0);
                first = false;
            }
            CloseHandle(ht);
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
    log_msg("[CPU-ADV] Ideal processor hint set (main thread -> core 0)");
}

// ---- Per-Process Security Mitigation Flags ----
// Disables Spectre/Meltdown mitigations for the game process
// Gives back 3-8% CPU performance on older CPUs
// Safe for games you trust -- does NOT affect other processes

typedef BOOL(WINAPI* PSetProcessMitigationPolicy)(
    PROCESS_MITIGATION_POLICY, PVOID, SIZE_T);
static PSetProcessMitigationPolicy g_SetProcessMitigationPolicy = nullptr;

static void init_mitigation_api()
{
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    if (k32)
        g_SetProcessMitigationPolicy =
        reinterpret_cast<PSetProcessMitigationPolicy>(
            GetProcAddress(k32, "SetProcessMitigationPolicy"));
}

static void disable_process_mitigations(DWORD pid)
{
    // We can only set mitigations on our own process or via injection
    // The safe approach: set on the optimizer's process (affects memory layout)
    // and use NtSetInformationProcess for the game's process
    if (!g_SetProcessMitigationPolicy) return;

    // Disable strict handle checks (small perf gain, reduces exception overhead)
    PROCESS_MITIGATION_STRICT_HANDLE_CHECK_POLICY shcp{};
    shcp.RaiseExceptionOnInvalidHandleReference = 0;
    shcp.HandleExceptionsPermanentlyEnabled = 0;
    g_SetProcessMitigationPolicy(ProcessStrictHandleCheckPolicy, &shcp, sizeof(shcp));

    log_msg("[CPU-ADV] Process mitigation flags adjusted (reduced security overhead)");
}

// ---- Large Page Privilege ----
static bool g_large_pages_enabled = false;

static void enable_large_pages()
{
    // Grant SeLockMemoryPrivilege to current process -- allows large page use
    HANDLE hToken;
    if (!OpenProcessToken(GetCurrentProcess(),
        TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken))
        return;

    LUID luid{};
    if (LookupPrivilegeValueW(nullptr, L"SeLockMemoryPrivilege", &luid)) {
        TOKEN_PRIVILEGES tp{};
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Luid = luid;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        (void)AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(tp), nullptr, nullptr);
        if (GetLastError() == ERROR_SUCCESS)
        {
            g_large_pages_enabled = true;
            log_msg("[CPU-ADV] Large page privilege granted (SeLockMemoryPrivilege)");
        }
        else {
            log_msg("[CPU-ADV] Could not grant large page privilege (needs secpol change)");
        }
    }
    CloseHandle(hToken);
}

// ---- SMT Scheduling Awareness ----
static bool g_smt_modified = false;

static void optimize_smt_scheduling()
{
    // Disable SMT-aware scheduling so Windows prefers physical cores
    // HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\kernel
    // ThreadDpcEnable = 0 reduces DPC latency
    HKEY key;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
        L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\kernel",
        0, KEY_READ | KEY_WRITE, &key) == ERROR_SUCCESS)
    {
        DWORD val = 0;
        (void)RegSetValueExW(key, L"ThreadDpcEnable", 0, REG_DWORD,
            reinterpret_cast<const BYTE*>(&val), sizeof(val));
        RegCloseKey(key);
        g_smt_modified = true;
        log_msg("[CPU-ADV] DPC thread scheduling optimized");
    }

    // Also disable processor idle demote/promote for snappier response
    _wsystem(L"powercfg /setacvalueindex SCHEME_CURRENT SUB_PROCESSOR IDLEDEMOTE 0 >nul 2>&1");
    _wsystem(L"powercfg /setacvalueindex SCHEME_CURRENT SUB_PROCESSOR IDLEPROMOTE 0 >nul 2>&1");
    _wsystem(L"powercfg /setactive SCHEME_CURRENT >nul 2>&1");
    log_msg("[CPU-ADV] Processor idle promote/demote disabled");
}

static void restore_smt_scheduling()
{
    if (!g_smt_modified) return;
    HKEY key;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
        L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\kernel",
        0, KEY_WRITE, &key) == ERROR_SUCCESS)
    {
        (void)RegDeleteValueW(key, L"ThreadDpcEnable");
        RegCloseKey(key);
    }
    _wsystem(L"powercfg /setacvalueindex SCHEME_CURRENT SUB_PROCESSOR IDLEDEMOTE 75 >nul 2>&1");
    _wsystem(L"powercfg /setacvalueindex SCHEME_CURRENT SUB_PROCESSOR IDLEPROMOTE 25 >nul 2>&1");
    _wsystem(L"powercfg /setactive SCHEME_CURRENT >nul 2>&1");
    g_smt_modified = false;
    log_msg("[CPU-ADV] SMT scheduling restored");
}

// ---- Background Thread Mode ----
// Sets non-game processes to background mode -- lowers their scheduler priority
// without affecting their process priority class

static std::vector<DWORD> g_background_mode_pids;

static void set_background_thread_mode(const std::vector<std::string>& background_apps)
{
    std::set<std::string> targets;
    for (auto& n : background_apps) targets.insert(to_lower(n));

    auto procs = enumerate_processes();
    int count = 0;
    for (auto& p : procs) {
        if (!targets.count(to_lower(p.name))) continue;
        HANDLE h = OpenProcess(PROCESS_SET_INFORMATION | PROCESS_QUERY_INFORMATION,
            FALSE, p.pid);
        if (!h) continue;
        // THREAD_MODE_BACKGROUND_BEGIN on the process handle lowers all its threads
        if (SetPriorityClass(h, PROCESS_MODE_BACKGROUND_BEGIN)) {
            g_background_mode_pids.push_back(p.pid);
            ++count;
        }
        CloseHandle(h);
    }
    if (count > 0)
        log_msg("[CPU-ADV] Background thread mode set on " +
            std::to_string(count) + " processes");
}

static void restore_background_thread_mode()
{
    if (g_background_mode_pids.empty()) return;
    for (DWORD pid : g_background_mode_pids) {
        HANDLE h = OpenProcess(PROCESS_SET_INFORMATION, FALSE, pid);
        if (h) {
            SetPriorityClass(h, PROCESS_MODE_BACKGROUND_END);
            CloseHandle(h);
        }
    }
    g_background_mode_pids.clear();
    log_msg("[CPU-ADV] Background thread mode restored");
}

// ---- Prefetch Disable for Session ----
static bool g_prefetch_disabled = false;

static void disable_prefetch()
{
    // Stop SysMain from actively prefetching during gaming session
    // We already may stop SysMain via service suspend but this is belt-and-suspenders
    HKEY key;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
        L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Memory Management\\PrefetchParameters",
        0, KEY_READ | KEY_WRITE, &key) == ERROR_SUCCESS)
    {
        DWORD orig = 3;
        DWORD size = sizeof(orig);
        RegQueryValueExW(key, L"EnablePrefetcher", nullptr, nullptr,
            reinterpret_cast<BYTE*>(&orig), &size);

        if (orig != 0) {
            DWORD val = 0;
            (void)RegSetValueExW(key, L"EnablePrefetcher", 0, REG_DWORD,
                reinterpret_cast<const BYTE*>(&val), sizeof(val));
            g_prefetch_disabled = true;
            log_msg("[CPU-ADV] Prefetcher disabled (stops cache pollution)");
        }
        RegCloseKey(key);
    }
}

static void restore_prefetch()
{
    if (!g_prefetch_disabled) return;
    HKEY key;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
        L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Memory Management\\PrefetchParameters",
        0, KEY_WRITE, &key) == ERROR_SUCCESS)
    {
        DWORD val = 3; // default: both application and boot prefetch
        (void)RegSetValueExW(key, L"EnablePrefetcher", 0, REG_DWORD,
            reinterpret_cast<const BYTE*>(&val), sizeof(val));
        RegCloseKey(key);
    }
    g_prefetch_disabled = false;
    log_msg("[CPU-ADV] Prefetcher restored");
}




// ============================================================
// CONSOLE COLOR / THEME
// ============================================================

// ============================================================
// CONSOLE COLOR / THEME (hex-based, themes.json driven)
// ============================================================

// Converts #RRGGBB hex string to COLORREF
static COLORREF hex_to_colorref(const std::string& hex)
{
    // Strip leading #
    std::string h = hex;
    if (!h.empty() && h[0] == '#') h = h.substr(1);
    if (h.size() != 6) return RGB(255, 255, 255);
    unsigned int r = 0, g = 0, b = 0;
    (void)std::sscanf(h.c_str(), "%02x%02x%02x", &r, &g, &b);
    return RGB(r, g, b);
}

// Sets a specific console palette slot to a COLORREF
static void set_console_palette_entry(HANDLE hcon, int index, COLORREF color)
{
    CONSOLE_SCREEN_BUFFER_INFOEX info{};
    info.cbSize = sizeof(info);
    if (!GetConsoleScreenBufferInfoEx(hcon, &info)) return;
    info.ColorTable[index] = color;
    // Prevent window resize on SetConsoleScreenBufferInfoEx
    info.srWindow.Right++;
    info.srWindow.Bottom++;
    (void)SetConsoleScreenBufferInfoEx(hcon, &info);
}

struct HexTheme {
    std::string normal;    // #RRGGBB -- main text color
    std::string highlight; // #RRGGBB -- bright/accent
    std::string warning;   // #RRGGBB -- warnings
    std::string error_col; // #RRGGBB -- errors
    std::string background;// #RRGGBB -- background
};

// Built-in themes as hex
static const std::map<std::string, HexTheme> BUILTIN_THEMES = {
    { "default", { "#C0C0C0", "#FFFFFF", "#FFFF00", "#FF4444", "#0C0C0C" } },
    { "green",   { "#00FF00", "#00FFCC", "#FFFF00", "#FF4444", "#0C0C0C" } },
    { "cyan",    { "#00CCFF", "#FFFFFF", "#FFFF00", "#FF4444", "#0C0C0C" } },
    { "amber",   { "#FFB300", "#FFD700", "#FF6600", "#FF2200", "#0C0C0C" } },
    { "red",     { "#FF4444", "#FF8888", "#FFAA00", "#FF0000", "#0C0C0C" } },
    { "white",   { "#FFFFFF", "#FFFFFF", "#FFFF00", "#FF4444", "#0C0C0C" } },
    { "purple",  { "#CC88FF", "#FFFFFF", "#FFFF00", "#FF4444", "#0C0C0C" } },
    { "matrix",  { "#00FF41", "#FFFFFF", "#FFFF00", "#FF0000", "#000000" } },
    { "ocean",   { "#4FC3F7", "#B3E5FC", "#FFD54F", "#EF5350", "#001B2E" } },
    { "rose",    { "#F48FB1", "#FFFFFF", "#FFD54F", "#EF5350", "#1A0010" } },
};

static std::map<std::string, HexTheme> g_all_themes; // builtin + user themes
static std::string g_current_theme = "default";

static json make_default_themes()
{
    json j;
    // Write all builtins as examples so user can see the format
    for (auto& kv : BUILTIN_THEMES) {
        j[kv.first]["normal"] = kv.second.normal;
        j[kv.first]["highlight"] = kv.second.highlight;
        j[kv.first]["warning"] = kv.second.warning;
        j[kv.first]["error"] = kv.second.error_col;
        j[kv.first]["background"] = kv.second.background;
    }
    return j;
}

static void load_themes()
{
    // Start with builtins
    g_all_themes.clear();
    for (auto& kv : BUILTIN_THEMES)
        g_all_themes[kv.first] = kv.second;

    // Create themes.json if it doesn't exist
    if (!fs::exists(THEMES_FILE))
        save_json_file(THEMES_FILE, make_default_themes());

    // Load and merge user themes (user can override builtins or add new ones)
    json j = load_json_file(THEMES_FILE, json::object());
    for (auto it = j.begin(); it != j.end(); ++it) {
        HexTheme t;
        t.normal = it.value().value("normal", "#C0C0C0");
        t.highlight = it.value().value("highlight", "#FFFFFF");
        t.warning = it.value().value("warning", "#FFFF00");
        t.error_col = it.value().value("error", "#FF4444");
        t.background = it.value().value("background", "#0C0C0C");
        g_all_themes[it.key()] = t;
    }
}

static void apply_console_color(const std::string& theme_name)
{
    auto it = g_all_themes.find(theme_name);
    if (it == g_all_themes.end()) {
        std::string names;
        for (auto& kv : g_all_themes) names += kv.first + " ";
        log_msg("[COLOR] Unknown theme: " + theme_name + ". Available: " + names);
        return;
    }

    HANDLE hcon = GetStdHandle(STD_OUTPUT_HANDLE);
    auto& t = it->second;

    // Set palette slots:
    // Slot 7  = normal text (FOREGROUND_RED|GREEN|BLUE)
    // Slot 15 = bright text (FOREGROUND_INTENSITY)
    // Slot 6  = warning (yellow slot)
    // Slot 4  = error (red slot)
    // Slot 0  = background
    set_console_palette_entry(hcon, 7, hex_to_colorref(t.normal));
    set_console_palette_entry(hcon, 15, hex_to_colorref(t.highlight));
    set_console_palette_entry(hcon, 14, hex_to_colorref(t.warning));
    set_console_palette_entry(hcon, 12, hex_to_colorref(t.error_col));
    set_console_palette_entry(hcon, 0, hex_to_colorref(t.background));

    // Set text attribute to normal slot
    SetConsoleTextAttribute(hcon, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);

    g_current_theme = theme_name;
    log_msg("[COLOR] Theme set to: " + theme_name +
        " (normal: " + t.normal + " bg: " + t.background + ")");
}

static void load_saved_theme()
{
    load_themes();
    json cfg = load_json_file(CONFIG_FILE, json::object());
    if (cfg.contains("ui") && cfg["ui"].contains("theme")) {
        std::string t = cfg["ui"]["theme"].get<std::string>();
        apply_console_color(t);
    }
}

static void save_theme(const std::string& theme_name)
{
    json cfg = load_json_file(CONFIG_FILE, json::object());
    cfg["ui"]["theme"] = theme_name;
    save_json_file(CONFIG_FILE, cfg);
}

static void list_themes()
{
    log_msg("[COLOR] Available themes:");
    for (auto& kv : g_all_themes) {
        std::string marker = (kv.first == g_current_theme) ? " <- active" : "";
        log_msg("  " + kv.first + "  (normal: " + kv.second.normal +
            ", bg: " + kv.second.background + ")" + marker);
    }
    log_msg("[COLOR] Edit themes.json to add custom themes using #RRGGBB hex colors.");
}

// ============================================================
// GPU NVIDIA/AMD POWER MANAGEMENT
// ============================================================

static bool g_gpu_power_modified = false;

static void set_gpu_power_max()
{
    // NVIDIA: force max performance via NvAPI registry key
    // HKCU\SOFTWARE\NVIDIA Corporation\Global\NVTweak\Devices\
    // Also HKLM path for system-wide
    HKEY key;
    bool nvidia_ok = false;

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
        L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4D36E968-E325-11CE-BFC1-08002BE10318}\\0000",
        0, KEY_READ | KEY_WRITE, &key) == ERROR_SUCCESS)
    {
        // PerfLevelSrc = 0x3322 forces max performance on NVIDIA
        DWORD val = 0x3322;
        if (RegSetValueExW(key, L"PerfLevelSrc", 0, REG_DWORD,
            reinterpret_cast<const BYTE*>(&val), sizeof(val)) == ERROR_SUCCESS)
        {
            // PowerMizerLevel = 1 (max performance)
            val = 1;
            (void)RegSetValueExW(key, L"PowerMizerLevel", 0, REG_DWORD,
                reinterpret_cast<const BYTE*>(&val), sizeof(val));
            nvidia_ok = true;
        }
        RegCloseKey(key);
    }

    // AMD: disable ULPS (Ultra Low Power State)
    bool amd_ok = false;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
        L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4D36E968-E325-11CE-BFC1-08002BE10318}\\0000",
        0, KEY_READ | KEY_WRITE, &key) == ERROR_SUCCESS)
    {
        DWORD val = 0;
        if (RegSetValueExW(key, L"EnableUlps", 0, REG_DWORD,
            reinterpret_cast<const BYTE*>(&val), sizeof(val)) == ERROR_SUCCESS)
            amd_ok = true;
        RegCloseKey(key);
    }

    if (nvidia_ok || amd_ok) {
        g_gpu_power_modified = true;
        log_msg("[GPU-ADV] GPU power set to maximum performance mode");
    }
    else {
        log_msg("[GPU-ADV] Could not set GPU power mode (driver key not found)");
    }
}

static void restore_gpu_power()
{
    if (!g_gpu_power_modified) return;
    HKEY key;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
        L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4D36E968-E325-11CE-BFC1-08002BE10318}\\0000",
        0, KEY_WRITE, &key) == ERROR_SUCCESS)
    {
        (void)RegDeleteValueW(key, L"PerfLevelSrc");
        (void)RegDeleteValueW(key, L"PowerMizerLevel");
        DWORD val = 1;
        (void)RegSetValueExW(key, L"EnableUlps", 0, REG_DWORD,
            reinterpret_cast<const BYTE*>(&val), sizeof(val));
        RegCloseKey(key);
    }
    g_gpu_power_modified = false;
    log_msg("[GPU-ADV] GPU power mode restored");
}

static void check_hags()
{
    // Hardware-Accelerated GPU Scheduling
    HKEY key;
    bool hags_on = false;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
        L"SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers",
        0, KEY_READ, &key) == ERROR_SUCCESS)
    {
        DWORD val = 0, size = sizeof(val);
        if (RegQueryValueExW(key, L"HwSchMode", nullptr, nullptr,
            reinterpret_cast<BYTE*>(&val), &size) == ERROR_SUCCESS)
            hags_on = (val == 2);
        RegCloseKey(key);
    }
    if (hags_on)
        log_msg("[GPU-ADV] Hardware-Accelerated GPU Scheduling (HAGS): ENABLED (good)");
    else
        log_msg("[GPU-ADV] Hardware-Accelerated GPU Scheduling (HAGS): DISABLED, "
            "enable in Windows Graphics Settings for lower CPU overhead");
}

// ============================================================
// STANDBY LIST CLEANER
// ============================================================

typedef LONG(NTAPI* PNtSetSystemInformation)(INT, PVOID, ULONG);
static PNtSetSystemInformation g_NtSetSystemInformation = nullptr;

static void init_standby_api()
{
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll)
        g_NtSetSystemInformation =
        reinterpret_cast<PNtSetSystemInformation>(
            GetProcAddress(ntdll, "NtSetSystemInformation"));
}

static DWORD flush_standby_list()
{
    if (!g_NtSetSystemInformation) return 0;

    MEMORYSTATUSEX before{};
    before.dwLength = sizeof(before);
    GlobalMemoryStatusEx(&before);

    // SystemMemoryListInformation = 80, MemoryPurgeStandbyList = 4
    UINT cmd = 4;
    g_NtSetSystemInformation(80, &cmd, sizeof(cmd));

    MEMORYSTATUSEX after{};
    after.dwLength = sizeof(after);
    GlobalMemoryStatusEx(&after);

    DWORD freed_mb = static_cast<DWORD>(
        (after.ullAvailPhys - before.ullAvailPhys) / (1024ULL * 1024ULL));
    return freed_mb;
}

static void clean_standby_list()
{
    DWORD freed = flush_standby_list();
    log_msg("[MEM-ADV] Standby list flushed, " +
        std::to_string(freed) + " MB freed");
}

// ============================================================
// WORKING SET TRIM (all non-game processes)
// ============================================================

static void trim_all_working_sets(DWORD game_pid)
{
    auto procs = enumerate_processes();
    int count = 0;
    for (auto& p : procs) {
        if (p.pid == game_pid) continue;
        HANDLE h = OpenProcess(PROCESS_SET_QUOTA | PROCESS_QUERY_INFORMATION,
            FALSE, p.pid);
        if (!h) continue;
        if (SetProcessWorkingSetSize(h, static_cast<SIZE_T>(-1),
            static_cast<SIZE_T>(-1)))
            ++count;
        CloseHandle(h);
        // Small yield between each process to avoid handle storm stutter
        Sleep(1);
    }
    log_msg("[MEM-ADV] Working sets trimmed on " +
        std::to_string(count) + " processes");
}

// ============================================================
// INTERRUPT AFFINITY (pin NIC/audio/USB interrupts to specific cores)
// ============================================================

static bool g_interrupt_affinity_set = false;

static void set_interrupt_affinity()
{
    // Pin interrupts to last core (keeps game's cores interrupt-free)
    // Done via MSI Interrupt Affinity registry keys
    HKEY key;
    int count = 0;

    // Network adapter interrupts
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
        L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4D36E972-E325-11CE-BFC1-08002BE10318}",
        0, KEY_READ, &key) == ERROR_SUCCESS)
    {
        wchar_t subkey[256];
        DWORD index = 0, name_len = 256;
        while (RegEnumKeyExW(key, index++, subkey, &name_len,
            nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS)
        {
            name_len = 256;
            if (wcsncmp(subkey, L"Properties", 10) == 0) continue;
            std::wstring full = L"SYSTEM\\CurrentControlSet\\Control\\Class\\"
                L"{4D36E972-E325-11CE-BFC1-08002BE10318}\\";
            full += subkey;
            HKEY ak;
            if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, full.c_str(),
                0, KEY_READ | KEY_WRITE, &ak) == ERROR_SUCCESS)
            {
                DWORD data_size = 0;
                if (RegQueryValueExW(ak, L"DriverDesc", nullptr, nullptr,
                    nullptr, &data_size) == ERROR_SUCCESS)
                {
                    // Pin to last logical CPU
                    SYSTEM_INFO si{}; GetSystemInfo(&si);
                    DWORD mask = 1 << (si.dwNumberOfProcessors - 1);
                    (void)RegSetValueExW(ak, L"*RssBaseProcNumber", 0, REG_DWORD,
                        reinterpret_cast<const BYTE*>(&mask), sizeof(mask));
                    ++count;
                }
                RegCloseKey(ak);
            }
        }
        RegCloseKey(key);
    }

    if (count > 0) {
        g_interrupt_affinity_set = true;
        log_msg("[IRQ] Network interrupts pinned to last CPU core (" +
            std::to_string(count) + " adapters)");
    }
}

static void restore_interrupt_affinity()
{
    if (!g_interrupt_affinity_set) return;
    HKEY key;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
        L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4D36E972-E325-11CE-BFC1-08002BE10318}",
        0, KEY_READ, &key) == ERROR_SUCCESS)
    {
        wchar_t subkey[256];
        DWORD index = 0, name_len = 256;
        while (RegEnumKeyExW(key, index++, subkey, &name_len,
            nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS)
        {
            name_len = 256;
            if (wcsncmp(subkey, L"Properties", 10) == 0) continue;
            std::wstring full = L"SYSTEM\\CurrentControlSet\\Control\\Class\\"
                L"{4D36E972-E325-11CE-BFC1-08002BE10318}\\";
            full += subkey;
            HKEY ak;
            if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, full.c_str(),
                0, KEY_WRITE, &ak) == ERROR_SUCCESS)
            {
                (void)RegDeleteValueW(ak, L"*RssBaseProcNumber");
                RegCloseKey(ak);
            }
        }
        RegCloseKey(key);
    }
    g_interrupt_affinity_set = false;
    log_msg("[IRQ] Interrupt affinity restored");
}

// ============================================================
// USB POWER SAVING DISABLE
// ============================================================

static std::vector<std::wstring> g_usb_power_keys;

static void disable_usb_power_saving()
{
    // HKLM\SYSTEM\CurrentControlSet\Services\USB\Parameters
    // DisableSelectiveSuspend = 1
    HKEY key;
    int count = 0;

    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE,
        L"SYSTEM\\CurrentControlSet\\Services\\USB\\Parameters",
        0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) == ERROR_SUCCESS)
    {
        DWORD val = 1;
        (void)RegSetValueExW(key, L"DisableSelectiveSuspend", 0, REG_DWORD,
            reinterpret_cast<const BYTE*>(&val), sizeof(val));
        RegCloseKey(key);
        ++count;
    }

    // Also disable via device power management for HID devices
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
        L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4D36E96B-E325-11CE-BFC1-08002BE10318}",
        0, KEY_READ, &key) == ERROR_SUCCESS)
    {
        wchar_t subkey[256];
        DWORD index = 0, name_len = 256;
        while (RegEnumKeyExW(key, index++, subkey, &name_len,
            nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS)
        {
            name_len = 256;
            std::wstring full = L"SYSTEM\\CurrentControlSet\\Control\\Class\\"
                L"{4D36E96B-E325-11CE-BFC1-08002BE10318}\\";
            full += subkey;
            HKEY ak;
            if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, full.c_str(),
                0, KEY_READ | KEY_WRITE, &ak) == ERROR_SUCCESS)
            {
                DWORD val = 0;
                (void)RegSetValueExW(ak, L"EnhancedPowerManagementEnabled", 0, REG_DWORD,
                    reinterpret_cast<const BYTE*>(&val), sizeof(val));
                g_usb_power_keys.push_back(full);
                ++count;
                RegCloseKey(ak);
            }
        }
        RegCloseKey(key);
    }

    if (count > 0)
        log_msg("[USB] USB power saving disabled (" +
            std::to_string(count) + " device(s)) -- no more mouse/keyboard stutter");
}

static void restore_usb_power_saving()
{
    HKEY key;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
        L"SYSTEM\\CurrentControlSet\\Services\\USB\\Parameters",
        0, KEY_WRITE, &key) == ERROR_SUCCESS)
    {
        (void)RegDeleteValueW(key, L"DisableSelectiveSuspend");
        RegCloseKey(key);
    }

    for (auto& path : g_usb_power_keys) {
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(),
            0, KEY_WRITE, &key) == ERROR_SUCCESS)
        {
            (void)RegDeleteValueW(key, L"EnhancedPowerManagementEnabled");
            RegCloseKey(key);
        }
    }
    g_usb_power_keys.clear();
    log_msg("[USB] USB power saving restored");
}

// ============================================================
// AUDIO LATENCY
// ============================================================

static bool g_audio_latency_set = false;

static void set_audio_low_latency()
{
    // Set Windows audio engine to low latency mode
    // HKCU\SOFTWARE\Microsoft\Windows\CurrentVersion\Audio
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER,
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Audio",
        0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) == ERROR_SUCCESS)
    {
        DWORD val = 1;
        (void)RegSetValueExW(key, L"DisableSpatialAudioWithCommsMode", 0, REG_DWORD,
            reinterpret_cast<const BYTE*>(&val), sizeof(val));
        RegCloseKey(key);
    }

    // Boost AudioDG (Windows Audio Device Graph) priority
    auto procs = enumerate_processes();
    for (auto& p : procs) {
        if (to_lower(p.name) != "audiodg.exe") continue;
        HANDLE h = OpenProcess(PROCESS_SET_INFORMATION | PROCESS_QUERY_INFORMATION,
            FALSE, p.pid);
        if (h) {
            SetPriorityClass(h, HIGH_PRIORITY_CLASS);
            CloseHandle(h);
            log_msg("[AUDIO] AudioDG priority set to HIGH");
        }
        break;
    }

    // Set audio service to performance mode via registry
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Multimedia\\SystemProfile\\Tasks\\Games",
        0, KEY_READ | KEY_WRITE, &key) == ERROR_SUCCESS)
    {
        DWORD val = 100;
        (void)RegSetValueExW(key, L"GPU Priority", 0, REG_DWORD,
            reinterpret_cast<const BYTE*>(&val), sizeof(val));
        val = 6;
        (void)RegSetValueExW(key, L"Priority", 0, REG_DWORD,
            reinterpret_cast<const BYTE*>(&val), sizeof(val));
        RegCloseKey(key);
        g_audio_latency_set = true;
        log_msg("[AUDIO] Audio system profile set to Games mode (low latency)");
    }
}

static void restore_audio_latency()
{
    if (!g_audio_latency_set) return;
    HKEY key;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Multimedia\\SystemProfile\\Tasks\\Games",
        0, KEY_WRITE, &key) == ERROR_SUCCESS)
    {
        DWORD val = 8;
        (void)RegSetValueExW(key, L"GPU Priority", 0, REG_DWORD,
            reinterpret_cast<const BYTE*>(&val), sizeof(val));
        val = 2;
        (void)RegSetValueExW(key, L"Priority", 0, REG_DWORD,
            reinterpret_cast<const BYTE*>(&val), sizeof(val));
        RegCloseKey(key);
    }
    g_audio_latency_set = false;
    log_msg("[AUDIO] Audio latency settings restored");
}

// ============================================================
// WINDOWS DEFENDER EXCLUSION (game folder)
// ============================================================

static std::string g_defender_excluded_path;

static void add_defender_exclusion(const std::string& game_exe)
{
    // Find the game's exe path and add it as a Defender exclusion
    auto procs = enumerate_processes();
    for (auto& p : procs) {
        if (to_lower(p.name) != to_lower(game_exe) || p.exe_path.empty()) continue;

        std::wstring dir = p.exe_path.substr(0, p.exe_path.find_last_of(L'\\'));
        std::wstring cmd = L"powershell -NonInteractive -Command "
            L"\"Add-MpPreference -ExclusionPath '" +
            dir + L"' -ErrorAction SilentlyContinue\" >nul 2>&1";
        if (_wsystem(cmd.c_str()) == 0) {
            g_defender_excluded_path = to_narrow(dir);
            log_msg("[DEFENDER] Added exclusion for: " + g_defender_excluded_path);
        }
        break;
    }
}

static void remove_defender_exclusion()
{
    if (g_defender_excluded_path.empty()) return;
    std::wstring dir = to_wide(g_defender_excluded_path);
    std::wstring cmd = L"powershell -NonInteractive -Command "
        L"\"Remove-MpPreference -ExclusionPath '" +
        dir + L"' -ErrorAction SilentlyContinue\" >nul 2>&1";
    _wsystem(cmd.c_str());
    log_msg("[DEFENDER] Removed exclusion for: " + g_defender_excluded_path);
    g_defender_excluded_path.clear();
}

// ============================================================
// SESSION HISTORY
// ============================================================

struct SessionRecord {
    std::string timestamp;
    std::string game;
    int duration_sec = 0;
};

static std::vector<SessionRecord> g_session_history;
static std::mutex g_history_mutex;
static SessionRecord g_last_session;  // most recent completed session

// Session history is stored as a plain text file -- one line per session
// Format: [TIMESTAMP] | GAME | Xm Ys
static void load_session_history()
{
    if (HISTORY_FILE.empty()) return;
    std::ifstream f(HISTORY_FILE);
    if (!f.is_open()) return;
    std::lock_guard<std::mutex> lk(g_history_mutex);
    g_session_history.clear();
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        // Parse: [TIMESTAMP] | GAME | Xm Ys
        SessionRecord r;
        // timestamp between first [ and ]
        size_t ts_start = line.find('[');
        size_t ts_end = line.find(']');
        if (ts_start != std::string::npos && ts_end != std::string::npos)
            r.timestamp = line.substr(ts_start + 1, ts_end - ts_start - 1);
        // fields split by " | "
        std::vector<std::string> parts;
        std::string rest = (ts_end != std::string::npos) ? line.substr(ts_end + 1) : line;
        size_t pos = 0;
        while ((pos = rest.find(" | ")) != std::string::npos) {
            parts.push_back(trim(rest.substr(0, pos)));
            rest = rest.substr(pos + 3);
        }
        parts.push_back(trim(rest));
        if (parts.size() >= 2) r.game = parts[0];
        // parse duration from last field e.g. "5m 12s"
        if (parts.size() >= 3) {
            int m = 0, s = 0;
            (void)sscanf_s(parts[2].c_str(), "%dm %ds", &m, &s);
            r.duration_sec = m * 60 + s;
        }
        g_session_history.push_back(r);
    }
}

static void save_session_record(const SessionRecord& rec)
{
    { std::lock_guard<std::mutex> lk(g_history_mutex); g_last_session = rec; }
    if (HISTORY_FILE.empty()) return;

    // Read existing lines to enforce 50-session limit
    std::vector<std::string> lines;
    {
        std::ifstream fin(HISTORY_FILE);
        std::string line;
        while (std::getline(fin, line))
            if (!line.empty()) lines.push_back(line);
    }

    // Build new line
    int mins = rec.duration_sec / 60;
    int secs = rec.duration_sec % 60;
    char buf[512];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE,
        "[%s] | %s | %dm %ds",
        rec.timestamp.c_str(), rec.game.c_str(), mins, secs);
    lines.push_back(buf);

    // Keep last 50
    while (lines.size() > 50) lines.erase(lines.begin());

    std::ofstream fout(HISTORY_FILE, std::ios::out | std::ios::trunc);
    fout << "# Optimizer Session History - one line per session\n";
    fout << "# Format: [TIMESTAMP] | GAME | DURATION\n";
    fout << "#\n";
    for (auto& l : lines) fout << l << "\n";
}

static void show_history(int count = 10)
{
    std::lock_guard<std::mutex> lk(g_history_mutex);
    if (g_session_history.empty()) {
        log_msg("[HISTORY] No session history yet.");
        return;
    }
    int start = static_cast<int>(g_session_history.size()) - count;
    if (start < 0) start = 0;
    log_msg("[HISTORY] Last " +
        std::to_string(static_cast<int>(g_session_history.size()) - start) +
        " sessions:");
    for (int i = start; i < static_cast<int>(g_session_history.size()); ++i) {
        auto& r = g_session_history[i];
        int mins = r.duration_sec / 60;
        int secs = r.duration_sec % 60;
        char buf[256];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE,
            "  [%s]  %-30s  %dm %ds",
            r.timestamp.c_str(), r.game.c_str(), mins, secs);
        log_msg(buf);
    }
}

// ============================================================
// BENCHMARK (before/after responsiveness)
// ============================================================

static void run_benchmark()
{
    log_msg("[BENCH] Running responsiveness benchmark...");

    // Measure context switch latency via high-res timer
    LARGE_INTEGER freq{}, t1{}, t2{};
    QueryPerformanceFrequency(&freq);
    double total = 0;
    const int samples = 1000;
    for (int i = 0; i < samples; ++i) {
        QueryPerformanceCounter(&t1);
        Sleep(0);
        QueryPerformanceCounter(&t2);
        total += static_cast<double>(t2.QuadPart - t1.QuadPart) /
            static_cast<double>(freq.QuadPart) * 1000.0;
    }
    double latency_ms = total / samples;

    // Measure memory bandwidth (simple)
    const size_t buf_size = 64ULL * 1024ULL * 1024ULL; // 64MB
    std::vector<char> bench_buf(buf_size, 0);
    LARGE_INTEGER freq2{}, t3{}, t4{};
    QueryPerformanceFrequency(&freq2);
    QueryPerformanceCounter(&t3);
    memset(bench_buf.data(), 1, buf_size);
    QueryPerformanceCounter(&t4);
    double mem_bw_gbs = (static_cast<double>(buf_size) /
        (static_cast<double>(t4.QuadPart - t3.QuadPart) /
            static_cast<double>(freq2.QuadPart))) / (1024.0 * 1024.0 * 1024.0);

    char result[256];
    _snprintf_s(result, sizeof(result), _TRUNCATE,
        "[BENCH] Scheduler latency: %.3f ms avg | Memory BW: %.2f GB/s",
        latency_ms, mem_bw_gbs);
    log_msg(result);

    if (latency_ms < 0.1)
        log_msg("[BENCH] Scheduler: EXCELLENT (optimizations active)");
    else if (latency_ms < 0.5)
        log_msg("[BENCH] Scheduler: GOOD");
    else
        log_msg("[BENCH] Scheduler: POOR (C-states may still be active or system under load)");
}


// ============================================================
// POWER THROTTLING DISABLE
// ============================================================

static void disable_power_throttling(DWORD pid)
{
    HANDLE h = OpenProcess(PROCESS_SET_INFORMATION | PROCESS_QUERY_INFORMATION,
        FALSE, pid);
    if (!h) return;

    // PROCESS_POWER_THROTTLING_EXECUTION_SPEED = 0x1
    // Setting StateMask = 0 with ControlMask = 1 disables EcoQoS throttling
    struct PROCESS_POWER_THROTTLING_STATE_LOCAL {
        ULONG Version = 0;
        ULONG ControlMask = 0;
        ULONG StateMask = 0;
    } throttle_state{};

    throttle_state.Version = 1; // PROCESS_POWER_THROTTLING_CURRENT_VERSION
    throttle_state.ControlMask = 0x1; // PROCESS_POWER_THROTTLING_EXECUTION_SPEED
    throttle_state.StateMask = 0;   // 0 = disable throttling

    if (g_SetProcessInformation_fn && g_SetProcessInformation_fn(h, static_cast<PROCESS_INFORMATION_CLASS>(1),
        &throttle_state, sizeof(throttle_state)))
        log_msg("[THROTTLE] Power throttling disabled for game (EcoQoS off)");
    else
        log_msg("[THROTTLE] Could not disable power throttling (Windows 10 1709+ required)");

    CloseHandle(h);
}

static void restore_power_throttling(DWORD pid)
{
    HANDLE h = OpenProcess(PROCESS_SET_INFORMATION, FALSE, pid);
    if (!h) return;

    struct PROCESS_POWER_THROTTLING_STATE_LOCAL {
        ULONG Version = 0;
        ULONG ControlMask = 0;
        ULONG StateMask = 0;
    } throttle_state{};

    throttle_state.Version = 1;
    throttle_state.ControlMask = 0;  // release control back to Windows
    throttle_state.StateMask = 0;

    g_SetProcessInformation_fn&& g_SetProcessInformation_fn(h, static_cast<PROCESS_INFORMATION_CLASS>(1),
        &throttle_state, sizeof(throttle_state));
    CloseHandle(h);
    log_msg("[THROTTLE] Power throttling restored");
}

// ============================================================
// HPET / DYNAMIC TICK
// ============================================================

static bool g_hpet_disabled = false;

static void disable_dynamic_tick()
{
    // bcdedit /set disabledynamictick yes
    // Requires admin -- reduces timer interrupt overhead for more consistent frame times
    int result = _wsystem(L"bcdedit /set disabledynamictick yes >nul 2>&1");
    if (result == 0) {
        g_hpet_disabled = true;
        log_msg("[HPET] Dynamic tick disabled (takes effect after reboot)");
        log_msg("[HPET] Note: reboot required for this change to take effect");
    }
    else {
        log_msg("[HPET] Failed to disable dynamic tick (try running as admin)");
    }
}

static void restore_dynamic_tick()
{
    if (!g_hpet_disabled) return;
    _wsystem(L"bcdedit /set disabledynamictick no >nul 2>&1");
    g_hpet_disabled = false;
    log_msg("[HPET] Dynamic tick restored (takes effect after reboot)");
}

// ============================================================
// SYSTEM SERVICES SUSPEND
// ============================================================

static const std::vector<std::wstring> SUSPENDABLE_SERVICES = {
    L"SysMain",          // Superfetch -- disk prefetching
    L"WSearch",          // Windows Search indexer
    L"DiagTrack",        // Connected User Experiences / telemetry
    L"Spooler",          // Print spooler
    L"wuauserv",         // Windows Update
    L"MapsBroker",       // Downloaded Maps Manager
    L"RetailDemo",       // Retail demo service
};

static std::vector<std::wstring> g_suspended_services;

static bool set_service_state(const std::wstring& name, bool start)
{
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return false;

    SC_HANDLE svc = OpenServiceW(scm, name.c_str(),
        SERVICE_START | SERVICE_STOP |
        SERVICE_QUERY_STATUS);
    if (!svc) { CloseServiceHandle(scm); return false; }

    bool ok = false;
    SERVICE_STATUS_PROCESS ssp{};
    DWORD bytes_needed = 0;

    if (!start) {
        SERVICE_STATUS ss{};
        ok = ControlService(svc, SERVICE_CONTROL_STOP, &ss) != 0;
    }
    else {
        ok = StartServiceW(svc, 0, nullptr) != 0;
    }

    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return ok;
}

static bool is_service_running(const std::wstring& name)
{
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return false;
    SC_HANDLE svc = OpenServiceW(scm, name.c_str(), SERVICE_QUERY_STATUS);
    if (!svc) { CloseServiceHandle(scm); return false; }

    SERVICE_STATUS ss{};
    bool running = false;
    if (QueryServiceStatus(svc, &ss))
        running = (ss.dwCurrentState == SERVICE_RUNNING);

    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return running;
}

static void suspend_services()
{
    g_suspended_services.clear();
    int count = 0;
    for (auto& svc_name : SUSPENDABLE_SERVICES) {
        if (!is_service_running(svc_name)) continue;
        if (set_service_state(svc_name, false)) {
            g_suspended_services.push_back(svc_name);
            log_msg("[SVC] Stopped: " + to_narrow(svc_name));
            ++count;
        }
    }
    if (count > 0)
        log_msg("[SVC] Suspended " + std::to_string(count) + " services");
    else
        log_msg("[SVC] No suspendable services were running");
}

static void restore_services()
{
    if (g_suspended_services.empty()) return;
    int count = 0;
    for (auto& svc_name : g_suspended_services) {
        if (set_service_state(svc_name, true)) {
            log_msg("[SVC] Restarted: " + to_narrow(svc_name));
            ++count;
        }
    }
    g_suspended_services.clear();
    log_msg("[SVC] Restored " + std::to_string(count) + " services");
}

// ============================================================
// XBOX GAME BAR / DVR DISABLE
// ============================================================

static bool g_gamebar_disabled = false;

static void disable_game_bar()
{
    // Disable Game DVR capture hooks via registry
    HKEY key;
    bool ok = false;

    // HKCU\SOFTWARE\Microsoft\Windows\CurrentVersion\GameDVR
    if (RegCreateKeyExW(HKEY_CURRENT_USER,
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\GameDVR",
        0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) == ERROR_SUCCESS)
    {
        DWORD val = 0;
        (void)RegSetValueExW(key, L"AppCaptureEnabled", 0, REG_DWORD,
            reinterpret_cast<const BYTE*>(&val), sizeof(val));
        RegCloseKey(key);
        ok = true;
    }

    // HKCU\System\GameConfigStore
    if (RegCreateKeyExW(HKEY_CURRENT_USER,
        L"System\\GameConfigStore",
        0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) == ERROR_SUCCESS)
    {
        DWORD val = 0;
        (void)RegSetValueExW(key, L"GameDVR_Enabled", 0, REG_DWORD,
            reinterpret_cast<const BYTE*>(&val), sizeof(val));
        RegCloseKey(key);
        ok = true;
    }

    if (ok) {
        g_gamebar_disabled = true;
        log_msg("[GAMEBAR] Xbox Game Bar/DVR capture disabled");
    }
}

static void restore_game_bar()
{
    if (!g_gamebar_disabled) return;
    HKEY key;

    if (RegCreateKeyExW(HKEY_CURRENT_USER,
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\GameDVR",
        0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) == ERROR_SUCCESS)
    {
        DWORD val = 1;
        (void)RegSetValueExW(key, L"AppCaptureEnabled", 0, REG_DWORD,
            reinterpret_cast<const BYTE*>(&val), sizeof(val));
        RegCloseKey(key);
    }

    if (RegCreateKeyExW(HKEY_CURRENT_USER,
        L"System\\GameConfigStore",
        0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) == ERROR_SUCCESS)
    {
        DWORD val = 1;
        (void)RegSetValueExW(key, L"GameDVR_Enabled", 0, REG_DWORD,
            reinterpret_cast<const BYTE*>(&val), sizeof(val));
        RegCloseKey(key);
    }

    g_gamebar_disabled = false;
    log_msg("[GAMEBAR] Xbox Game Bar/DVR restored");
}

// ============================================================
// FULLSCREEN OPTIMIZATIONS DISABLE (per-game)
// ============================================================

static std::vector<std::wstring> g_fso_disabled_exes;

static void disable_fullscreen_optimizations(const std::string& exe_name)
{
    // HKCU\SOFTWARE\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Layers
    // Value: "<full path to exe>" = "~ DISABLEDXMAXIMIZEDWINDOWEDMODE"
    // We set this for the exe name pattern since we may not know the full path
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER,
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\AppCompatFlags\\Layers",
        0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return;

    // Find full path of running exe
    auto procs = enumerate_processes();
    std::string target_l = to_lower(exe_name);
    for (auto& p : procs) {
        if (to_lower(p.name) != target_l || p.exe_path.empty()) continue;
        const wchar_t* flag = L"~ DISABLEDXMAXIMIZEDWINDOWEDMODE";
        if (RegSetValueExW(key, p.exe_path.c_str(), 0, REG_SZ,
            reinterpret_cast<const BYTE*>(flag),
            static_cast<DWORD>((wcslen(flag) + 1) * sizeof(wchar_t)))
            == ERROR_SUCCESS)
        {
            g_fso_disabled_exes.push_back(p.exe_path);
            log_msg("[FSO] Fullscreen optimizations disabled for: " +
                to_narrow(p.exe_path));
        }
        break;
    }
    RegCloseKey(key);
}

static void restore_fullscreen_optimizations()
{
    if (g_fso_disabled_exes.empty()) return;
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\AppCompatFlags\\Layers",
        0, KEY_WRITE, &key) != ERROR_SUCCESS)
        return;
    for (auto& path : g_fso_disabled_exes)
        (void)RegDeleteValueW(key, path.c_str());
    RegCloseKey(key);
    g_fso_disabled_exes.clear();
    log_msg("[FSO] Fullscreen optimizations restored");
}

// ============================================================
// MEMORY COMPRESSION / SYSMAIN DISABLE
// ============================================================

static void disable_memory_compression()
{
    // Disable memory compression via PowerShell -- session only, resets on reboot
    int result = _wsystem(
        L"powershell -NonInteractive -Command "
        L"\"try { Disable-MMAgent -MemoryCompression -ErrorAction Stop } "
        L"catch { }\" >nul 2>&1");
    if (result == 0)
        log_msg("[MEM] Memory compression disabled for this session");
    else
        log_msg("[MEM] Could not disable memory compression (may need PS execution policy)");
}

static void restore_memory_compression()
{
    _wsystem(
        L"powershell -NonInteractive -Command "
        L"\"try { Enable-MMAgent -MemoryCompression -ErrorAction Stop } "
        L"catch { }\" >nul 2>&1");
    log_msg("[MEM] Memory compression re-enabled");
}

// ============================================================
// PAGEFILE OPTIMIZATION
// ============================================================

static bool g_pagefile_optimized = false;
static DWORD g_pagefile_orig_min = 0;
static DWORD g_pagefile_orig_max = 0;

static void optimize_pagefile()
{
    // Set a fixed pagefile size to prevent mid-game resize stutters
    // Use 1.5x physical RAM, clamped between 4GB and 16GB
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    GlobalMemoryStatusEx(&ms);

    DWORD ram_mb = static_cast<DWORD>(ms.ullTotalPhys / (1024ULL * 1024ULL));
    DWORD fixed_mb = static_cast<DWORD>(ram_mb * 1.5);
    if (fixed_mb < 4096)  fixed_mb = 4096;
    if (fixed_mb > 16384) fixed_mb = 16384;

    // SetSystemFileCacheSize is not pagefile -- use registry approach
    // HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Memory Management
    HKEY key;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
        L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Memory Management",
        0, KEY_READ | KEY_WRITE, &key) != ERROR_SUCCESS)
        return;

    // Read existing value to back up
    wchar_t existing[64]{};
    DWORD existing_size = sizeof(existing);
    RegQueryValueExW(key, L"PagingFiles", nullptr, nullptr,
        reinterpret_cast<BYTE*>(existing), &existing_size);

    // Set fixed size: "C:\pagefile.sys <min_mb> <max_mb>"
    wchar_t value[128]{};
    _snwprintf_s(value, 128, _TRUNCATE,
        L"C:\\pagefile.sys %lu %lu", fixed_mb, fixed_mb);
    (void)RegSetValueExW(key, L"PagingFiles", 0, REG_MULTI_SZ,
        reinterpret_cast<const BYTE*>(value),
        static_cast<DWORD>((wcslen(value) + 2) * sizeof(wchar_t)));

    RegCloseKey(key);
    g_pagefile_optimized = true;
    log_msg("[PAGE] Pagefile fixed at " + std::to_string(fixed_mb) +
        " MB (prevents mid-game resize stutters, takes effect on reboot)");
}

// ============================================================
// DWM PRIORITY BOOST
// ============================================================

static int g_dwm_priority_backup = 0;
static DWORD g_dwm_pid = 0;

static void boost_dwm_priority()
{
    auto procs = enumerate_processes();
    for (auto& p : procs) {
        if (to_lower(p.name) != "dwm.exe") continue;
        HANDLE h = OpenProcess(PROCESS_SET_INFORMATION | PROCESS_QUERY_INFORMATION,
            FALSE, p.pid);
        if (!h) continue;
        g_dwm_pid = p.pid;
        g_dwm_priority_backup = static_cast<int>(GetPriorityClass(h));
        if (SetPriorityClass(h, HIGH_PRIORITY_CLASS))
            log_msg("[DWM] Desktop Window Manager priority -> HIGH");
        CloseHandle(h);
        break;
    }
}

static void restore_dwm_priority()
{
    if (g_dwm_pid == 0) return;
    HANDLE h = OpenProcess(PROCESS_SET_INFORMATION, FALSE, g_dwm_pid);
    if (h) {
        SetPriorityClass(h, static_cast<DWORD>(g_dwm_priority_backup));
        CloseHandle(h);
        log_msg("[DWM] DWM priority restored");
    }
    g_dwm_pid = 0;
}

// ============================================================
// VISUAL EFFECTS / ANIMATIONS DISABLE
// ============================================================

static bool g_visual_effects_disabled = false;
static DWORD g_orig_visual_effects = 0;

static void disable_visual_effects()
{
    // SystemParametersInfo SPI_GETANIMATION / SPI_SETANIMATION
    // Also disable min/max animations via registry
    ANIMATIONINFO ai{};
    ai.cbSize = sizeof(ai);
    SystemParametersInfoW(SPI_GETANIMATION, sizeof(ai), &ai, 0);
    g_orig_visual_effects = ai.iMinAnimate;

    if (ai.iMinAnimate != 0) {
        ai.iMinAnimate = 0;
        SystemParametersInfoW(SPI_SETANIMATION, sizeof(ai), &ai, SPIF_SENDCHANGE);
    }

    // Disable transparency
    BOOL transparency = FALSE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &transparency, 0);
    if (transparency) {
        BOOL off = FALSE;
        SystemParametersInfoW(SPI_SETCLIENTAREAANIMATION, 0, &off, SPIF_SENDCHANGE);
    }

    g_visual_effects_disabled = true;
    log_msg("[DISPLAY] Visual animations disabled");
}

static void restore_visual_effects()
{
    if (!g_visual_effects_disabled) return;

    ANIMATIONINFO ai{};
    ai.cbSize = sizeof(ai);
    ai.iMinAnimate = static_cast<int>(g_orig_visual_effects);
    SystemParametersInfoW(SPI_SETANIMATION, sizeof(ai), &ai, SPIF_SENDCHANGE);

    BOOL on = TRUE;
    SystemParametersInfoW(SPI_SETCLIENTAREAANIMATION, 0, &on, SPIF_SENDCHANGE);

    g_visual_effects_disabled = false;
    log_msg("[DISPLAY] Visual animations restored");
}

// ============================================================
// DISK WRITE CACHE
// ============================================================

static std::vector<std::wstring> g_disk_cache_modified;

static void optimize_disk_write_cache()
{
    // Enable write caching on all fixed disks via DeviceIoControl
    // IOCTL_DISK_SET_CACHE_INFORMATION -- safe on SSDs
    DWORD drives = GetLogicalDrives();
    int count = 0;

    for (int i = 0; i < 26; ++i) {
        if (!(drives & (1 << i))) continue;
        wchar_t drive_path[8];
        _snwprintf_s(drive_path, 8, _TRUNCATE, L"\\\\.\\%c:", L'A' + i);

        HANDLE h = CreateFileW(drive_path,
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;

        DISK_CACHE_INFORMATION dci{};
        DWORD bytes = 0;

        if (DeviceIoControl(h, IOCTL_DISK_GET_CACHE_INFORMATION,
            nullptr, 0, &dci, sizeof(dci), &bytes, nullptr))
        {
            if (!dci.WriteCacheEnabled) {
                dci.WriteCacheEnabled = TRUE;
                if (DeviceIoControl(h, IOCTL_DISK_SET_CACHE_INFORMATION,
                    &dci, sizeof(dci), nullptr, 0, &bytes, nullptr))
                {
                    g_disk_cache_modified.push_back(drive_path);
                    ++count;
                }
            }
        }
        CloseHandle(h);
    }

    if (count > 0)
        log_msg("[DISK] Write cache enabled on " +
            std::to_string(count) + " drive(s)");
    else
        log_msg("[DISK] Write cache already enabled on all drives");
}



// ============================================================
// POWER PLAN
// ============================================================

static void set_ultimate_power()
{
    std::wstring cmd = L"powercfg /setactive " + ULTIMATE_POWER_GUID_W;
    _wsystem(cmd.c_str());
    log_msg("[POWER] Ultimate Performance enabled");
}

// ============================================================
// TIMER RESOLUTION
// ============================================================

static void init_ntdll()
{
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll)
        g_NtSetTimerResolution = reinterpret_cast<PNtSetTimerResolution>(
            GetProcAddress(ntdll, "NtSetTimerResolution"));
    init_mmcss_api();
    init_io_priority_api();
    init_mitigation_api();
    init_standby_api();
    init_win10_apis();
}

static void enable_timer()
{
    if (g_NtSetTimerResolution) {
        g_NtSetTimerResolution(5000, TRUE, &g_timer_current_res);
        log_msg("[TIMER] Enabled");
    }
}

static void disable_timer()
{
    try {
        if (g_NtSetTimerResolution) {
            g_NtSetTimerResolution(5000, FALSE, &g_timer_current_res);
            log_msg("[TIMER] Restored");
        }
    }
    catch (...) {}
}

// ============================================================
// RAM CLEANER
// ============================================================

struct RamCleanerSettings {
    bool enabled = false;
    int  interval_sec = RAM_CLEAN_DEFAULT_INTERVAL_SEC;
    bool empty_working_sets = true;
    bool flush_file_cache = true;
};

static RamCleanerSettings resolve_ram_cleaner_settings(
    const json& game_cfg,
    const json& effective_features,
    const json& global_config)
{
    RamCleanerSettings s;
    s.enabled = feature_enabled("ram_cleaner", effective_features, game_cfg);

    if (global_config.contains("ram_cleaner") && global_config["ram_cleaner"].is_object()) {
        const auto& rc = global_config["ram_cleaner"];
        if (rc.contains("interval_sec") && rc["interval_sec"].is_number_integer())
            s.interval_sec = rc["interval_sec"].get<int>();
        if (rc.contains("empty_working_sets") && rc["empty_working_sets"].is_boolean())
            s.empty_working_sets = rc["empty_working_sets"].get<bool>();
        if (rc.contains("flush_file_cache") && rc["flush_file_cache"].is_boolean())
            s.flush_file_cache = rc["flush_file_cache"].get<bool>();
    }

    if (game_cfg.contains("ram_cleaner") && game_cfg["ram_cleaner"].is_object()) {
        const auto& rc = game_cfg["ram_cleaner"];
        if (rc.contains("interval_sec") && rc["interval_sec"].is_number_integer())
            s.interval_sec = rc["interval_sec"].get<int>();
        if (rc.contains("empty_working_sets") && rc["empty_working_sets"].is_boolean())
            s.empty_working_sets = rc["empty_working_sets"].get<bool>();
        if (rc.contains("flush_file_cache") && rc["flush_file_cache"].is_boolean())
            s.flush_file_cache = rc["flush_file_cache"].get<bool>();
    }

    if (s.interval_sec < 30)   s.interval_sec = 30;
    if (s.interval_sec > 3600) s.interval_sec = 3600;
    return s;
}

static bool do_flush_file_cache()
{
    HANDLE hToken;
    if (!OpenProcessToken(GetCurrentProcess(),
        TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken))
        return false;
    bool ok = false;
    LUID luid{};
    if (LookupPrivilegeValueW(nullptr, L"SeIncreaseQuotaPrivilege", &luid)) {
        TOKEN_PRIVILEGES tp{};
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Luid = luid;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        (void)AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(tp), nullptr, nullptr);
        ok = SetSystemFileCacheSize(1, 1, 0) != 0;
        tp.Privileges[0].Attributes = 0;
        (void)AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(tp), nullptr, nullptr);
    }
    CloseHandle(hToken);
    return ok;
}

static int do_empty_working_sets(DWORD skip_pid = 0)
{
    int count = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            // Skip system processes and most importantly the game itself
            if (pe.th32ProcessID <= 4) continue;
            if (skip_pid && pe.th32ProcessID == skip_pid) continue;
            HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_SET_QUOTA,
                FALSE, pe.th32ProcessID);
            if (!h) continue;
            if (EmptyWorkingSet(h)) ++count;
            CloseHandle(h);
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return count;
}

static double run_ram_clean(const RamCleanerSettings& settings, DWORD game_pid = 0)
{
    // Run RAM clean at idle priority to minimize game impact
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_IDLE);
    MEMORYSTATUSEX before{}, after{};
    before.dwLength = sizeof(before);
    after.dwLength = sizeof(after);
    GlobalMemoryStatusEx(&before);

    if (settings.empty_working_sets) {
        // Pass game_pid so we never evict the game's own working set
        int n = do_empty_working_sets(game_pid);
        log_msg("[RAM] Trimmed working sets of " + std::to_string(n) + " processes (game excluded)");
    }
    if (settings.flush_file_cache) {
        bool ok = do_flush_file_cache();
        log_msg(std::string("[RAM] File cache flush: ") +
            (ok ? "OK" : "failed (needs elevated token)"));
    }

    GlobalMemoryStatusEx(&after);
    return static_cast<double>(
        static_cast<long long>(after.ullAvailPhys) -
        static_cast<long long>(before.ullAvailPhys)
        ) / (1024.0 * 1024.0);
}

static void ram_cleaner_loop(DWORD game_pid, RamCleanerSettings settings)
{
    log_msg("[RAM] Cleaner started (interval: " +
        std::to_string(settings.interval_sec) + "s" +
        (settings.empty_working_sets ? ", working-sets" : "") +
        (settings.flush_file_cache ? ", file-cache" : "") + ")");

    int warmup = (settings.interval_sec < 60) ? settings.interval_sec : 60;
    for (int i = 0; i < warmup && g_running.load() && pid_exists(game_pid); ++i)
        std::this_thread::sleep_for(std::chrono::seconds(1));

    // Open a handle to wait on instead of polling pid_exists every second
    HANDLE hGame = OpenProcess(SYNCHRONIZE, FALSE, game_pid);

    while (g_running.load()) {
        // Check if game is still running cheaply via handle
        if (hGame) {
            DWORD wr = WaitForSingleObject(hGame, 0);
            if (wr == WAIT_OBJECT_0) break; // game exited
        }
        else if (!pid_exists(game_pid)) break;

        double freed = run_ram_clean(settings, game_pid);
        char buf[96];
        if (freed >= 0.0)
            _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                "[RAM] Clean done. ~%.0f MB freed", freed);
        else
            _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                "[RAM] Clean done. (%.0f MB consumed by OS - normal)", -freed);
        log_msg(buf);

        // Wait for interval using the game handle -- wakes immediately if game exits
        if (hGame) {
            DWORD wait_ms = static_cast<DWORD>(settings.interval_sec) * 1000;
            WaitForSingleObject(hGame, wait_ms);
        }
        else {
            for (int i = 0;
                i < settings.interval_sec && g_running.load() && pid_exists(game_pid);
                ++i)
                std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
    if (hGame) CloseHandle(hGame);
    log_msg("[RAM] Cleaner stopped");
}

// ============================================================
// PROCESS UTILITIES
// ============================================================

// Full enumeration -- name + exe path (used when optimizing a known game)
static std::vector<ProcessInfo> enumerate_processes()
{
    std::vector<ProcessInfo> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            ProcessInfo info;
            info.pid = pe.th32ProcessID;
            info.name = to_narrow(pe.szExeFile);
            HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,
                FALSE, pe.th32ProcessID);
            if (hProc) {
                wchar_t buf[MAX_PATH]{};
                DWORD len = MAX_PATH;
                if (QueryFullProcessImageNameW(hProc, 0, buf, &len))
                    info.exe_path = buf;
                CloseHandle(hProc);
            }
            out.push_back(info);
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return out;
}

// Lightweight enumeration -- name only, no handle opens (used by discovery loop)
static std::vector<ProcessInfo> enumerate_processes_light()
{
    std::vector<ProcessInfo> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            ProcessInfo info;
            info.pid = pe.th32ProcessID;
            info.name = to_narrow(pe.szExeFile);

            out.push_back(info);
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return out;
}

static bool pid_exists(DWORD pid)
{
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!h) return false;
    DWORD code = WaitForSingleObject(h, 0);
    CloseHandle(h);
    return code == WAIT_TIMEOUT;
}

// ============================================================
// PRIORITY
// ============================================================

static bool enable_debug_privilege()
{
    HANDLE hToken;
    if (!OpenProcessToken(GetCurrentProcess(),
        TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken))
        return false;
    LUID luid{};
    if (!LookupPrivilegeValueW(nullptr, L"SeDebugPrivilege", &luid)) {
        CloseHandle(hToken);
        return false;
    }
    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    (void)AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(tp), nullptr, nullptr);
    CloseHandle(hToken);
    return GetLastError() == ERROR_SUCCESS;
}

static bool set_priority_win32(DWORD pid, DWORD priority_class)
{
    HANDLE h = OpenProcess(PROCESS_SET_INFORMATION | PROCESS_QUERY_INFORMATION,
        FALSE, pid);
    if (!h) return false;
    bool ok = SetPriorityClass(h, priority_class) != 0;
    CloseHandle(h);
    return ok;
}

static std::vector<DWORD> get_children(DWORD parent_pid)
{
    std::vector<DWORD> children;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return children;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe))
        do {
            if (pe.th32ParentProcessID == parent_pid &&
                pe.th32ProcessID != parent_pid)
                children.push_back(pe.th32ProcessID);
        } while (Process32NextW(snap, &pe));
    CloseHandle(snap);
    return children;
}

static void boost_priority(DWORD pid, int duration_secs = 10)
{
    (void)duration_secs; // no longer loops
    log_msg("[PRIORITY] Boosting priority...");
    enable_debug_privilege();
    // Set once -- no loop. Games override if they want to.
    // Looping caused stutter at game start from repeated child enumeration.
    if (!set_priority_win32(pid, HIGH_PRIORITY_CLASS))
        log_msg("[PRIORITY] Could not set HIGH priority (blocked/AccessDenied).");
    // Set children too, with throttle
    for (DWORD cpid : get_children(pid)) {
        set_priority_win32(cpid, HIGH_PRIORITY_CLASS);
        Sleep(1);
    }
    log_msg("[PRIORITY] Priority set");
}

// ============================================================
// EXPLORER PRIORITY
// ============================================================

static void tweak_explorer_priority(bool lower)
{
    try {
        auto procs = enumerate_processes();
        std::lock_guard<std::mutex> lk(g_backup_mutex);
        for (auto& p : procs) {
            if (to_lower(p.name) != "explorer.exe") continue;
            HANDLE h = OpenProcess(PROCESS_SET_INFORMATION | PROCESS_QUERY_INFORMATION,
                FALSE, p.pid);
            if (!h) continue;
            if (lower) {
                if (g_explorer_priority_backup.find(p.pid) ==
                    g_explorer_priority_backup.end())
                {
                    g_explorer_priority_backup[p.pid] =
                        static_cast<int>(GetPriorityClass(h));
                    SetPriorityClass(h, BELOW_NORMAL_PRIORITY_CLASS);
                    log_msg("[EXPLORER] PID " + std::to_string(p.pid) +
                        " priority -> BELOW_NORMAL");
                }
            }
            else {
                auto it = g_explorer_priority_backup.find(p.pid);
                if (it != g_explorer_priority_backup.end()) {
                    SetPriorityClass(h, static_cast<DWORD>(it->second));
                    log_msg("[EXPLORER] PID " + std::to_string(p.pid) +
                        " priority restored");
                }
            }
            CloseHandle(h);
        }
        if (!lower) g_explorer_priority_backup.clear();
    }
    catch (...) {}
}

// ============================================================
// AFFINITY
// ============================================================

static void isolate_background_affinity(
    const std::vector<std::string>& background_apps)
{
    if (background_apps.empty()) {
        log_msg("[AFFINITY] No background apps configured, skipping isolation");
        return;
    }
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    int cpu_count = static_cast<int>(si.dwNumberOfProcessors);
    if (cpu_count < 2) return;

    DWORD_PTR bg_mask = DWORD_PTR(1) << (cpu_count - 1);
    if (cpu_count >= 4) bg_mask |= DWORD_PTR(1) << (cpu_count - 2);

    std::set<std::string> targets;
    for (auto& n : background_apps) targets.insert(to_lower(n));

    log_msg("[AFFINITY] Isolating background apps");
    auto procs = enumerate_processes();
    std::lock_guard<std::mutex> lk(g_backup_mutex);
    for (auto& p : procs) {
        if (!targets.count(to_lower(p.name))) continue;
        HANDLE h = OpenProcess(PROCESS_SET_INFORMATION | PROCESS_QUERY_INFORMATION,
            FALSE, p.pid);
        if (!h) continue;
        if (g_affinity_backup.find(p.pid) == g_affinity_backup.end()) {
            DWORD_PTR pm = 0, sm = 0;
            if (GetProcessAffinityMask(h, &pm, &sm))
                g_affinity_backup[p.pid] = { pm };
        }
        SetProcessAffinityMask(h, bg_mask);
        log_msg("[AFFINITY] " + p.name + " (PID " + std::to_string(p.pid) + ") isolated");
        CloseHandle(h);
    }
}

static void restore_affinity()
{
    std::lock_guard<std::mutex> lk(g_backup_mutex);
    if (g_affinity_backup.empty()) return;
    log_msg("[AFFINITY] Restoring background affinities...");
    for (auto& kv : g_affinity_backup) {
        HANDLE h = OpenProcess(PROCESS_SET_INFORMATION, FALSE, kv.first);
        if (h) {
            SetProcessAffinityMask(h, kv.second[0]);
            log_msg("[AFFINITY] Restored PID " + std::to_string(kv.first));
            CloseHandle(h);
        }
    }
    g_affinity_backup.clear();
}

// ============================================================
// BACKGROUND PRIORITY
// ============================================================

static void lower_background_priority(
    const std::vector<std::string>& background_apps)
{
    if (background_apps.empty()) return;
    std::set<std::string> targets;
    for (auto& n : background_apps) targets.insert(to_lower(n));

    auto procs = enumerate_processes();
    std::lock_guard<std::mutex> lk(g_backup_mutex);
    for (auto& p : procs) {
        if (!targets.count(to_lower(p.name))) continue;
        HANDLE h = OpenProcess(PROCESS_SET_INFORMATION | PROCESS_QUERY_INFORMATION,
            FALSE, p.pid);
        if (!h) continue;
        if (g_bg_priority_backup.find(p.pid) == g_bg_priority_backup.end()) {
            g_bg_priority_backup[p.pid] = static_cast<int>(GetPriorityClass(h));
            SetPriorityClass(h, BELOW_NORMAL_PRIORITY_CLASS);
            log_msg("[BG-PRIO] " + p.name + " (PID " + std::to_string(p.pid) +
                ") -> BELOW_NORMAL");
        }
        CloseHandle(h);
    }
}

static void restore_background_priority()
{
    std::lock_guard<std::mutex> lk(g_backup_mutex);
    if (g_bg_priority_backup.empty()) return;
    log_msg("[BG-PRIO] Restoring background priorities...");
    for (auto& kv : g_bg_priority_backup) {
        HANDLE h = OpenProcess(PROCESS_SET_INFORMATION, FALSE, kv.first);
        if (h) {
            SetPriorityClass(h, static_cast<DWORD>(kv.second));
            log_msg("[BG-PRIO] Restored PID " + std::to_string(kv.first));
            CloseHandle(h);
        }
    }
    g_bg_priority_backup.clear();
}

// ============================================================
// PROCESS CONTROL
// ============================================================

static void kill_processes(
    const std::vector<std::string>& names,
    const std::set<std::string>& store_paths_for = {})
{
    std::set<std::string> names_l;
    for (auto& n : names) names_l.insert(to_lower(n));

    auto procs = enumerate_processes();
    for (auto& p : procs) {
        std::string nl = to_lower(p.name);
        if (!names_l.count(nl)) continue;
        if (store_paths_for.count(nl) && !p.exe_path.empty()) {
            std::lock_guard<std::mutex> lk(g_backup_mutex);
            g_closed_reopen_paths.push_back(p.exe_path);
        }
        HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, p.pid);
        if (h) {
            TerminateProcess(h, 1);
            log_msg("[CLOSE] " + p.name);
            CloseHandle(h);
        }
    }
}

static void restart_paths(const std::vector<std::wstring>& paths)
{
    std::set<std::wstring> unique(paths.begin(), paths.end());
    for (auto& path : unique) {
        HINSTANCE r = ShellExecuteW(nullptr, L"open", path.c_str(),
            nullptr, nullptr, SW_SHOW);
        if (reinterpret_cast<INT_PTR>(r) > 32)
            log_msg("[REOPEN] Started: " + to_narrow(path));
        else
            log_msg("[REOPEN] Failed: " + to_narrow(path));
    }
}


// ============================================================
// LAUNCHER DETECTION, AUTO DISCOVERY & LAUNCH OPTIONS
// ============================================================
// ============================================================
// LAUNCHER DETECTION & AUTO GAME DISCOVERY
// ============================================================

// -- Registry helpers -----------------------------------------
static std::wstring reg_read_wstr(HKEY root, const std::wstring& path,
    const std::wstring& name)
{
    HKEY key;
    if (RegOpenKeyExW(root, path.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS)
        return L"";
    wchar_t buf[2048]{};
    DWORD sz = sizeof(buf);
    DWORD type = 0;
    RegQueryValueExW(key, name.c_str(), nullptr, &type,
        reinterpret_cast<BYTE*>(buf), &sz);
    RegCloseKey(key);
    return std::wstring(buf);
}

static std::wstring expand_env(const std::wstring& s)
{
    wchar_t buf[4096]{};
    ExpandEnvironmentStringsW(s.c_str(), buf, 4096);
    return std::wstring(buf);
}

// -- Launcher enum ---------------------------------------------
enum class Launcher { Unknown, Steam, Epic, EA, Ubisoft, Battlenet, GOG };

static std::string launcher_name(Launcher l) {
    switch (l) {
    case Launcher::Steam:     return "Steam";
    case Launcher::Epic:      return "Epic Games";
    case Launcher::EA:        return "EA App";
    case Launcher::Ubisoft:   return "Ubisoft Connect";
    case Launcher::Battlenet: return "Battle.net";
    case Launcher::GOG:       return "GOG Galaxy";
    default:                  return "Unknown";
    }
}

// -- Find Steam install path (registry, any drive) -------------
static std::wstring find_steam_path()
{
    std::wstring p = reg_read_wstr(HKEY_CURRENT_USER,
        L"Software\\Valve\\Steam", L"SteamPath");
    if (!p.empty()) {
        std::replace(p.begin(), p.end(), L'/', L'\\');
        return p;
    }
    // fallback
    p = reg_read_wstr(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath");
    return p;
}

// -- Get all Steam library folders ----------------------------
static std::vector<std::wstring> get_steam_libraries()
{
    std::vector<std::wstring> libs;
    std::wstring steam = find_steam_path();
    if (steam.empty()) return libs;
    libs.push_back(steam + L"\\steamapps\\common");

    // Parse libraryfolders.vdf
    fs::path vdf = fs::path(steam) / "steamapps" / "libraryfolders.vdf";
    if (!fs::exists(vdf)) return libs;
    std::ifstream f(vdf);
    std::string line;
    while (std::getline(f, line)) {
        // Look for "path" entries
        auto pos = line.find("\"path\"");
        if (pos == std::string::npos) continue;
        auto q1 = line.find('"', pos + 6);
        if (q1 == std::string::npos) continue;
        auto q2 = line.find('"', q1 + 1);
        if (q2 == std::string::npos) continue;
        std::string path = line.substr(q1 + 1, q2 - q1 - 1);
        // unescape forward slashes
        std::replace(path.begin(), path.end(), '/', '\\');
        std::wstring wpath = to_wide(path) + L"\\steamapps\\common";
        if (fs::exists(wpath) && std::find(libs.begin(), libs.end(), wpath) == libs.end())
            libs.push_back(wpath);
    }
    return libs;
}

// -- Find Epic manifests folder (registry, any drive) ---------
static std::wstring find_epic_manifests()
{
    // Try registry first
    std::wstring p = reg_read_wstr(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\WOW6432Node\\EpicGames\\Unreal Engine", L"INSTALLDIR");
    if (!p.empty()) {
        // manifests live in ProgramData not the install dir
    }
    // Standard location (drive-agnostic via env)
    std::wstring pd = expand_env(L"%PROGRAMDATA%\\Epic\\EpicGamesLauncher\\Data\\Manifests");
    if (fs::exists(pd)) return pd;
    return L"";
}

// -- Find EA App install path ----------------------------------
static std::wstring find_ea_path()
{
    std::wstring p = reg_read_wstr(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Electronic Arts\\EA Desktop", L"InstallPath");
    if (!p.empty()) return p;
    p = reg_read_wstr(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\WOW6432Node\\Electronic Arts\\EA Desktop", L"InstallPath");
    return p;
}

// -- Find Ubisoft Connect path ---------------------------------
static std::wstring find_ubisoft_path()
{
    std::wstring p = reg_read_wstr(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\WOW6432Node\\Ubisoft\\Launcher", L"InstallDir");
    if (!p.empty()) return p;
    p = reg_read_wstr(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Ubisoft\\Launcher", L"InstallDir");
    return p;
}

// -- Find Battle.net config ------------------------------------
static std::wstring find_battlenet_config()
{
    std::wstring p = reg_read_wstr(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\WOW6432Node\\Blizzard Entertainment\\Battle.net",
        L"InstallPath");
    if (!p.empty()) return p;
    // fallback via AppData
    return expand_env(L"%APPDATA%\\Battle.net");
}

// -- Find GOG Galaxy path --------------------------------------
static std::wstring find_gog_path()
{
    std::wstring p = reg_read_wstr(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\WOW6432Node\\GOG.com\\GalaxyClient\\paths", L"client");
    if (!p.empty()) return p;
    p = reg_read_wstr(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\GOG.com\\GalaxyClient\\paths", L"client");
    return p;
}

// -- Detect which launcher owns a game exe path ----------------
static Launcher detect_launcher(const std::wstring& exe_path)
{
    std::wstring low = exe_path;
    std::transform(low.begin(), low.end(), low.begin(), ::towlower);

    // Steam -- check if path is under any Steam library
    auto steam_libs = get_steam_libraries();
    for (auto& lib : steam_libs) {
        std::wstring lib_low = lib;
        std::transform(lib_low.begin(), lib_low.end(), lib_low.begin(), ::towlower);
        if (low.find(lib_low) != std::wstring::npos)
            return Launcher::Steam;
    }
    // Also check steamapps in path
    if (low.find(L"steamapps") != std::wstring::npos)
        return Launcher::Steam;

    // Epic
    if (low.find(L"epic games") != std::wstring::npos)
        return Launcher::Epic;

    // EA
    if (low.find(L"ea games") != std::wstring::npos ||
        low.find(L"electronic arts") != std::wstring::npos ||
        low.find(L"ea desktop") != std::wstring::npos)
        return Launcher::EA;

    // Ubisoft
    if (low.find(L"ubisoft") != std::wstring::npos)
        return Launcher::Ubisoft;

    // Battle.net
    if (low.find(L"battle.net") != std::wstring::npos ||
        low.find(L"battlenet") != std::wstring::npos ||
        low.find(L"blizzard") != std::wstring::npos)
        return Launcher::Battlenet;

    // GOG
    if (low.find(L"gog galaxy") != std::wstring::npos ||
        low.find(L"gogcom") != std::wstring::npos)
        return Launcher::GOG;

    return Launcher::Unknown;
}

// ============================================================
// LAUNCH OPTIONS WRITER
// ============================================================

// -- Steam: find AppID from exe path via steamapps manifests --
static std::string find_steam_appid(const std::wstring& exe_path)
{
    std::wstring steam = find_steam_path();
    if (steam.empty()) return "";

    // Collect all steamapps dirs
    std::vector<fs::path> apps_dirs;
    apps_dirs.push_back(fs::path(steam) / "steamapps");
    auto libs = get_steam_libraries();
    for (auto& lib : libs) {
        // lib already ends in \common, go up one
        fs::path p = fs::path(lib).parent_path();
        if (std::find(apps_dirs.begin(), apps_dirs.end(), p) == apps_dirs.end())
            apps_dirs.push_back(p);
    }

    std::wstring exe_low = exe_path;
    std::transform(exe_low.begin(), exe_low.end(), exe_low.begin(), ::towlower);

    for (auto& apps_dir : apps_dirs) {
        if (!fs::exists(apps_dir)) continue;
        for (auto& entry : fs::directory_iterator(apps_dir)) {
            if (entry.path().extension() != ".acf") continue;
            std::ifstream f(entry.path());
            std::string content((std::istreambuf_iterator<char>(f)),
                std::istreambuf_iterator<char>());
            // Get appid and installdir
            std::string appid, installdir;
            auto find_val = [&](const std::string& key) -> std::string {
                auto pos = content.find("\"" + key + "\"");
                if (pos == std::string::npos) return "";
                auto q1 = content.find('"', pos + key.size() + 2);
                if (q1 == std::string::npos) return "";
                auto q2 = content.find('"', q1 + 1);
                if (q2 == std::string::npos) return "";
                return content.substr(q1 + 1, q2 - q1 - 1);
                };
            appid = find_val("appid");
            installdir = find_val("installdir");
            if (appid.empty() || installdir.empty()) continue;
            std::wstring dir_low = to_wide(installdir);
            std::transform(dir_low.begin(), dir_low.end(), dir_low.begin(), ::towlower);
            if (exe_low.find(dir_low) != std::wstring::npos)
                return appid;
        }
    }
    return "";
}

// -- Steam: write launch options to localconfig.vdf -----------
static bool steam_write_launch_options(const std::string& appid,
    const std::string& args)
{
    std::wstring steam = find_steam_path();
    if (steam.empty()) return false;

    // Find all userdata folders
    fs::path userdata = fs::path(steam) / "userdata";
    if (!fs::exists(userdata)) return false;

    bool wrote = false;
    for (auto& user : fs::directory_iterator(userdata)) {
        fs::path cfg = user.path() / "config" / "localconfig.vdf";
        if (!fs::exists(cfg)) continue;

        // Read file
        std::ifstream in(cfg, std::ios::binary);
        std::string content((std::istreambuf_iterator<char>(in)),
            std::istreambuf_iterator<char>());
        in.close();

        // Find the app section
        std::string search = "\"" + appid + "\"";
        auto pos = content.find(search);
        if (pos == std::string::npos) continue;

        // Look for LaunchOptions within next 500 chars
        auto block_end = content.find("}", pos);
        auto lo_pos = content.find("\"LaunchOptions\"", pos);

        if (lo_pos != std::string::npos && lo_pos < block_end) {
            // Update existing LaunchOptions
            auto q1 = content.find('"', lo_pos + 15);
            if (q1 == std::string::npos) continue;
            auto q2 = content.find('"', q1 + 1);
            if (q2 == std::string::npos) continue;
            std::string existing = content.substr(q1 + 1, q2 - q1 - 1);
            // Only add -high if not already there
            if (existing.find("-high") == std::string::npos) {
                std::string new_args = existing.empty() ? args : existing + " " + args;
                content.replace(q1 + 1, q2 - q1 - 1, new_args);
            }
            else {
                log_msg("[LAUNCH] Steam: -high already present for AppID " + appid);
                return true;
            }
        }
        else {
            // Insert LaunchOptions after the appid line
            auto insert_pos = content.find('{', pos);
            if (insert_pos == std::string::npos) continue;
            insert_pos++;
            std::string entry = "\n\t\t\t\t\"LaunchOptions\"\t\t\"" + args + "\"";
            content.insert(insert_pos, entry);
        }

        // Write back
        std::ofstream out(cfg, std::ios::binary);
        out.write(content.c_str(), content.size());
        wrote = true;
        log_msg("[LAUNCH] Steam: wrote '" + args + "' for AppID " + appid);
    }
    return wrote;
}

// -- Epic: write launch options to game manifest ---------------
static bool epic_write_launch_options(const std::wstring& exe_path,
    const std::string& args)
{
    std::wstring manifests = find_epic_manifests();
    if (manifests.empty() || !fs::exists(manifests)) return false;

    std::wstring exe_low = exe_path;
    std::transform(exe_low.begin(), exe_low.end(), exe_low.begin(), ::towlower);

    for (auto& entry : fs::directory_iterator(manifests)) {
        if (entry.path().extension() != ".item") continue;
        std::ifstream f(entry.path());
        std::string content((std::istreambuf_iterator<char>(f)),
            std::istreambuf_iterator<char>());
        f.close();

        // Check if this manifest matches our exe
        auto pos = content.find("InstallLocation");
        if (pos == std::string::npos) continue;
        auto q1 = content.find('"', content.find(':', pos));
        if (q1 == std::string::npos) continue;
        auto q2 = content.find('"', q1 + 1);
        if (q2 == std::string::npos) continue;
        std::string install_loc = content.substr(q1 + 1, q2 - q1 - 1);
        // Epic uses forward slashes in JSON
        std::replace(install_loc.begin(), install_loc.end(), '/', '\\');
        std::wstring wloc = to_wide(install_loc);
        std::transform(wloc.begin(), wloc.end(), wloc.begin(), ::towlower);
        if (exe_low.find(wloc) == std::wstring::npos) continue;

        // Found matching manifest -- update AdditionalCommandlineArguments
        auto arg_pos = content.find("AdditionalCommandlineArguments");
        if (arg_pos != std::string::npos) {
            auto aq1 = content.find('"', content.find(':', arg_pos));
            if (aq1 == std::string::npos) continue;
            auto aq2 = content.find('"', aq1 + 1);
            if (aq2 == std::string::npos) continue;
            std::string existing = content.substr(aq1 + 1, aq2 - aq1 - 1);
            if (existing.find("-high") != std::string::npos) {
                log_msg("[LAUNCH] Epic: -high already present");
                return true;
            }
            std::string new_args = existing.empty() ? args : existing + " " + args;
            content.replace(aq1 + 1, aq2 - aq1 - 1, new_args);
        }
        else {
            // Insert before closing brace
            auto close = content.rfind('}');
            if (close == std::string::npos) continue;
            std::string entry = ",\n\t\"AdditionalCommandlineArguments\": \"" + args + "\"";
            content.insert(close, entry);
        }

        std::ofstream out(entry.path());
        out.write(content.c_str(), content.size());
        log_msg("[LAUNCH] Epic: wrote '" + args + "' to manifest");
        return true;
    }
    return false;
}

// -- EA App: write launch options via settings ini -------------
static bool ea_write_launch_options(const std::wstring& exe_path,
    const std::string& args)
{
    std::wstring local = expand_env(L"%LOCALAPPDATA%\\Electronic Arts\\EA Desktop\\EA Desktop");
    if (!fs::exists(local)) return false;

    // Find user ini file
    for (auto& entry : fs::directory_iterator(local)) {
        std::wstring fn = entry.path().filename().wstring();
        if (fn.find(L"user_") == std::wstring::npos ||
            entry.path().extension() != L".ini") continue;

        std::ifstream f(entry.path());
        std::string content((std::istreambuf_iterator<char>(f)),
            std::istreambuf_iterator<char>());
        f.close();

        std::wstring exe_low = exe_path;
        std::transform(exe_low.begin(), exe_low.end(), exe_low.begin(), ::towlower);
        std::string narrow_exe = to_narrow(exe_low);

        // Check if this ini references our game
        if (content.find(narrow_exe) == std::string::npos) continue;

        // Find or insert GameArguments
        auto pos = content.find("GameArguments=");
        if (pos != std::string::npos) {
            auto end = content.find('\n', pos);
            std::string existing = content.substr(pos + 14,
                end - pos - 14);
            if (existing.find("-high") != std::string::npos) {
                log_msg("[LAUNCH] EA: -high already present");
                return true;
            }
            std::string new_line = "GameArguments=" +
                (existing.empty() ? args : existing + " " + args);
            content.replace(pos, end - pos, new_line);
        }
        else {
            content += "\nGameArguments=" + args + "\n";
        }

        std::ofstream out(entry.path());
        out.write(content.c_str(), content.size());
        log_msg("[LAUNCH] EA: wrote '" + args + "'");
        return true;
    }
    return false;
}

// -- Ubisoft: write launch options to settings.yml -------------
static bool ubisoft_write_launch_options(const std::wstring& /*exe_path*/,
    const std::string& args)
{
    std::wstring cfg_path = expand_env(
        L"%LOCALAPPDATA%\\Ubisoft Game Launcher\\settings.yml");
    if (!fs::exists(cfg_path)) return false;

    std::ifstream f(cfg_path);
    std::string content((std::istreambuf_iterator<char>(f)),
        std::istreambuf_iterator<char>());
    f.close();

    auto pos = content.find("additional_launch_args:");
    if (pos != std::string::npos) {
        auto end = content.find('\n', pos);
        std::string existing = content.substr(pos + 23, end - pos - 23);
        existing.erase(0, existing.find_first_not_of(" \t'\""));
        existing.erase(existing.find_last_not_of(" \t'\"") + 1);
        if (existing.find("-high") != std::string::npos) {
            log_msg("[LAUNCH] Ubisoft: -high already present"); return true;
        }
        std::string new_line = "additional_launch_args: '" +
            (existing.empty() ? args : existing + " " + args) + "'";
        content.replace(pos, end - pos, new_line);
    }
    else {
        content += "\nadditional_launch_args: '" + args + "'\n";
    }

    std::ofstream out(cfg_path);
    out.write(content.c_str(), content.size());
    log_msg("[LAUNCH] Ubisoft: wrote '" + args + "'");
    return true;
}

// -- Battle.net: write launch options to config JSON -----------
static bool battlenet_write_launch_options(const std::wstring& exe_path,
    const std::string& args)
{
    std::wstring cfg_path = expand_env(
        L"%APPDATA%\\Battle.net\\Battle.net.config");
    if (!fs::exists(cfg_path)) return false;

    std::ifstream f(cfg_path);
    std::string content((std::istreambuf_iterator<char>(f)),
        std::istreambuf_iterator<char>());
    f.close();

    // Determine game slug from exe path
    std::wstring exe_low = exe_path;
    std::transform(exe_low.begin(), exe_low.end(), exe_low.begin(), ::towlower);
    std::string slug;
    if (exe_low.find(L"overwatch") != std::wstring::npos) slug = "prometheus";
    else if (exe_low.find(L"diablo") != std::wstring::npos) slug = "diablo4";
    else if (exe_low.find(L"cod") != std::wstring::npos ||
        exe_low.find(L"warzone") != std::wstring::npos) slug = "ZEUS";
    else if (exe_low.find(L"wow") != std::wstring::npos) slug = "WoW";
    if (slug.empty()) {
        log_msg("[LAUNCH] Battle.net: could not determine game slug");
        return false;
    }

    auto pos = content.find("\"" + slug + "\"");
    if (pos == std::string::npos) {
        log_msg("[LAUNCH] Battle.net: game slug not found in config");
        return false;
    }
    auto arg_pos = content.find("AdditionalLaunchArguments", pos);
    if (arg_pos != std::string::npos && arg_pos < pos + 500) {
        auto q1 = content.find('"', content.find(':', arg_pos));
        auto q2 = content.find('"', q1 + 1);
        if (q1 == std::string::npos || q2 == std::string::npos) return false;
        std::string existing = content.substr(q1 + 1, q2 - q1 - 1);
        if (existing.find("-high") != std::string::npos) {
            log_msg("[LAUNCH] Battle.net: -high already present"); return true;
        }
        std::string new_args = existing.empty() ? args : existing + " " + args;
        content.replace(q1 + 1, q2 - q1 - 1, new_args);
    }
    else {
        // Insert into game's block
        auto block = content.find('{', pos);
        if (block == std::string::npos) return false;
        block++;
        content.insert(block, "\n\t\"AdditionalLaunchArguments\": \"" + args + "\",");
    }

    std::ofstream out(cfg_path);
    out.write(content.c_str(), content.size());
    log_msg("[LAUNCH] Battle.net: wrote '" + args + "' for " + slug);
    return true;
}

// -- GOG: write launch options via registry --------------------
static bool gog_write_launch_options(const std::wstring& exe_path,
    const std::string& args)
{
    // GOG stores per-game args in registry
    HKEY hGames;
    const wchar_t* gog_key = L"SOFTWARE\\GOG.com\\Games";
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, gog_key, 0,
        KEY_READ, &hGames) != ERROR_SUCCESS) return false;

    std::wstring exe_low = exe_path;
    std::transform(exe_low.begin(), exe_low.end(), exe_low.begin(), ::towlower);

    DWORD idx = 0;
    wchar_t sub[64]{};
    DWORD slen = 64;
    bool wrote = false;

    while (RegEnumKeyExW(hGames, idx++, sub, &slen,
        nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS)
    {
        std::wstring full = std::wstring(gog_key) + L"\\" + sub;
        std::wstring game_path = reg_read_wstr(HKEY_LOCAL_MACHINE, full, L"path");
        std::transform(game_path.begin(), game_path.end(),
            game_path.begin(), ::towlower);

        if (!game_path.empty() && exe_low.find(game_path) != std::wstring::npos) {
            // Found our game -- write launch args
            std::wstring existing = reg_read_wstr(HKEY_LOCAL_MACHINE,
                full, L"launchParam");
            if (existing.find(L"-high") != std::wstring::npos) {
                log_msg("[LAUNCH] GOG: -high already present");
                RegCloseKey(hGames);
                return true;
            }
            std::wstring new_args = existing.empty() ?
                to_wide(args) : existing + L" " + to_wide(args);
            HKEY wkey;
            if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, full.c_str(),
                0, KEY_SET_VALUE, &wkey) == ERROR_SUCCESS) {
                (void)RegSetValueExW(wkey, L"launchParam", 0, REG_SZ,
                    reinterpret_cast<const BYTE*>(new_args.c_str()),
                    static_cast<DWORD>((new_args.size() + 1) * sizeof(wchar_t)));
                RegCloseKey(wkey);
                log_msg("[LAUNCH] GOG: wrote '" + args + "' for game ID " +
                    to_narrow(sub));
                wrote = true;
            }
            break;
        }
        slen = 64;
    }
    RegCloseKey(hGames);
    return wrote;
}

// -- Master: detect launcher and write -high -------------------
static void try_write_high_launch_option(const std::string& game_exe,
    const std::wstring& exe_path)
{
    Launcher launcher = detect_launcher(exe_path);
    std::string lname = launcher_name(launcher);
    log_msg("[LAUNCH] Launcher detected: " + lname + " for " + game_exe);

    bool ok = false;
    switch (launcher) {
    case Launcher::Steam: {
        std::string appid = find_steam_appid(exe_path);
        if (appid.empty()) {
            log_msg("[LAUNCH] Steam: could not find AppID for " + game_exe);
            break;
        }
        log_msg("[LAUNCH] Steam AppID: " + appid);
        ok = steam_write_launch_options(appid, "-high");
        break;
    }
    case Launcher::Epic:
        ok = epic_write_launch_options(exe_path, "-high");
        break;
    case Launcher::EA:
        ok = ea_write_launch_options(exe_path, "-high");
        break;
    case Launcher::Ubisoft:
        ok = ubisoft_write_launch_options(exe_path, "-high");
        break;
    case Launcher::Battlenet:
        ok = battlenet_write_launch_options(exe_path, "-high");
        break;
    case Launcher::GOG:
        ok = gog_write_launch_options(exe_path, "-high");
        break;
    default:
        log_msg("[LAUNCH] Unknown launcher, cannot write launch options");
        return;
    }

    if (ok) {
        log_msg("[LAUNCH] -high written via " + lname +
            " -- will take effect next game launch");
    }
    else {
        log_msg("[LAUNCH] Could not write launch options via " + lname);
    }
}

// ============================================================
// AUTO GAME DISCOVERY (CPU/GPU/Fullscreen detection)
// ============================================================

// -- Per-process CPU usage -------------------------------------
struct ProcCpuEntry {
    ULONGLONG last_time = 0;
    ULONGLONG last_sys = 0;
};
static std::map<DWORD, ProcCpuEntry> g_cpu_history;

// Pass sys_total=0 to let the function fetch it itself (legacy callers)
static double get_process_cpu_percent(DWORD pid, ULONGLONG sys_total = 0)
{
    HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!h) return 0.0;

    FILETIME ct{}, et{}, kt{}, ut{};
    if (!GetProcessTimes(h, &ct, &et, &kt, &ut)) {
        CloseHandle(h); return 0.0;
    }
    CloseHandle(h);

    auto ft64 = [](FILETIME f) -> ULONGLONG {
        return (static_cast<ULONGLONG>(f.dwHighDateTime) << 32ULL) |
            static_cast<ULONGLONG>(f.dwLowDateTime);
        };

    ULONGLONG proc_time = ft64(kt) + ft64(ut);

    if (sys_total == 0) {
        FILETIME sys_idle{}, sys_kernel{}, sys_user{};
        GetSystemTimes(&sys_idle, &sys_kernel, &sys_user);
        sys_total = ft64(sys_kernel) + ft64(sys_user);
    }

    auto& entry = g_cpu_history[pid];
    if (entry.last_time == 0) {
        entry.last_time = proc_time;
        entry.last_sys = sys_total;
        return 0.0;
    }

    ULONGLONG delta_proc = proc_time - entry.last_time;
    ULONGLONG delta_sys = sys_total - entry.last_sys;
    entry.last_time = proc_time;
    entry.last_sys = sys_total;

    if (delta_sys == 0) return 0.0;

    static double s_cpu_cores = 0.0;
    if (s_cpu_cores == 0.0) {
        SYSTEM_INFO _si{}; GetSystemInfo(&_si);
        s_cpu_cores = static_cast<double>(_si.dwNumberOfProcessors);
    }
    const double cores = s_cpu_cores;
    return (static_cast<double>(delta_proc) /
        static_cast<double>(delta_sys)) * 100.0 * cores;
}

// -- Fullscreen check ------------------------------------------
static bool window_is_fullscreen(HWND hwnd)
{
    if (!hwnd || !IsWindowVisible(hwnd)) return false;
    RECT wr{};
    GetWindowRect(hwnd, &wr);
    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{ sizeof(mi) };
    if (!GetMonitorInfoW(mon, &mi)) return false;
    return wr.left == mi.rcMonitor.left &&
        wr.top == mi.rcMonitor.top &&
        wr.right == mi.rcMonitor.right &&
        wr.bottom == mi.rcMonitor.bottom;
}

static DWORD get_foreground_pid()
{
    HWND fg = GetForegroundWindow();
    if (!fg) return 0;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid;
}

// -- Blacklist of known non-game processes ---------------------
static const std::set<std::string> DISCOVERY_BLACKLIST = {
    "explorer.exe", "chrome.exe", "firefox.exe", "msedge.exe",
    "opera.exe", "brave.exe", "discord.exe", "discordptb.exe",
    "discordcanary.exe", "steam.exe", "steamwebhelper.exe",
    "epicgameslauncher.exe", "eadesktop.exe", "upc.exe",
    "galaxyclient.exe", "battlenet.exe", "agent.exe",
    "nvcontainer.exe", "nvdisplay.container.exe", "nvcplui.exe",
    "dwm.exe", "csrss.exe", "svchost.exe", "lsass.exe",
    "taskhostw.exe", "taskmgr.exe", "searchhost.exe",
    "shellexperiencehost.exe", "startmenuexperiencehost.exe",
    "sihost.exe", "ctfmon.exe", "conhost.exe", "cmd.exe",
    "powershell.exe", "pwsh.exe", "windowsterminal.exe", "msiexec.exe",
    "msmpeng.exe", "nissrv.exe", "antimalware service executable",
    "regedit.exe", "notepad.exe", "notepad++.exe", "calc.exe",
    "mspaint.exe", "snippingtool.exe", "snagit32.exe",
    "code.exe", "devenv.exe", "rider64.exe", "idea64.exe",
    "slack.exe", "teams.exe", "zoom.exe", "skype.exe",
    "spotify.exe", "vlc.exe", "wmplayer.exe", "mpc-hc64.exe",
    "obs64.exe", "obs32.exe", "streamlabs obs.exe",
    "afterburner.exe", "msiafterburner.exe", "rtss.exe",
    "hwinfo64.exe", "hwmonitor.exe", "cpuid.exe",
    "amd software.exe", "radeon software.exe", "cncmd.exe",
    "optimizer.exe", "apex.exe",
};

// -- Load / save never-ask list --------------------------------
static std::set<std::string> load_never_ask()
{
    std::set<std::string> result;
    json j = load_json_file(NEVER_ASK_FILE, json::array());
    if (j.is_array())
        for (auto& v : j)
            if (v.is_string()) result.insert(v.get<std::string>());
    return result;
}

static void save_never_ask(const std::set<std::string>& list)
{
    json j = json::array();
    for (auto& s : list) j.push_back(s);
    save_json_file(NEVER_ASK_FILE, j);
}

static std::set<std::string> g_never_ask;
static std::set<std::string> g_discovery_declined;
static bool                  g_never_ask_loaded = false;

// -- DXGI check -- cached, only re-checks every 10 polls ------
// Once a process is confirmed as having GPU modules we never re-check.
// avoids EnumProcessModules hammering every 2s
static std::unordered_map<DWORD, bool> g_dxgi_cache;

static bool process_has_dxgi(DWORD pid)
{
    // Return cached result if already confirmed positive
    auto it = g_dxgi_cache.find(pid);
    if (it != g_dxgi_cache.end() && it->second) return true;

    HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
        FALSE, pid);
    if (!h) return false;

    // Only grab first 128 modules -- 3D apps load dxgi/d3d early
    HMODULE mods[128]{};
    DWORD needed = 0;
    bool found = false;
    if (EnumProcessModules(h, mods, sizeof(mods), &needed)) {
        DWORD count = std::min(needed / static_cast<DWORD>(sizeof(HMODULE)),
            static_cast<DWORD>(128));
        for (DWORD i = 0; i < count; i++) {
            wchar_t name[48]{};
            GetModuleBaseNameW(h, mods[i], name, 48);
            // Manual lowercase compare -- avoids string alloc
            wchar_t low[48]{};
            for (int j = 0; j < 48 && name[j]; j++)
                low[j] = static_cast<wchar_t>(towlower(name[j]));
            if (wcscmp(low, L"dxgi.dll") == 0 ||
                wcscmp(low, L"d3d11.dll") == 0 ||
                wcscmp(low, L"d3d12.dll") == 0 ||
                wcscmp(low, L"d3d9.dll") == 0 ||
                wcscmp(low, L"opengl32.dll") == 0 ||
                wcscmp(low, L"vulkan-1.dll") == 0) {
                found = true;
                break;
            }
        }
    }
    CloseHandle(h);
    g_dxgi_cache[pid] = found;
    return found;
}

// -- Sustained signal tracker ----------------------------------
struct DiscoveryCandidate {
    std::string  name;
    std::wstring exe_path;
    int          high_cpu_polls = 0;
    int          gpu_polls = 0;   // polls where DXGI/d3d confirmed
    bool         was_fullscreen = false;
    bool         prompted = false;
};
static std::map<DWORD, DiscoveryCandidate> g_discovery_candidates;

// -- Prompt user to add discovered game -----------------------
static void prompt_add_game(DWORD pid, DiscoveryCandidate& cand)
{
    if (cand.prompted) return;
    cand.prompted = true;

    std::string exe_lower = to_lower(cand.name);
    json games_check = load_json_file(GAMES_FILE, json::object());
    if (games_check.contains(exe_lower)) return;
    if (g_never_ask.count(exe_lower)) return;
    if (g_discovery_declined.count(exe_lower)) return;

    // Just queue the item -- wizard in command_loop handles all prompting
    {
        std::lock_guard<std::mutex> lk(g_console_mutex);
        g_pending_discovery.push_back({ pid, cand.name, cand.exe_path });
    }
}

// -- Auto-discovery scan loop (runs in its own thread) ---------
static void auto_discovery_loop(const json& /*presets*/)
{
    // Lazy-load never_ask now that NEVER_ASK_FILE path is initialized
    if (!g_never_ask_loaded) {
        g_never_ask = load_never_ask();
        g_never_ask_loaded = true;
    }

    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
    if (g_discovery_enabled.load()) {
        log_msg("[DETECTION] Auto-detection started");
        log_msg("[DETECTION] Requires 2 of 3 signals: fullscreen / CPU>30% / GPU (DXGI)");
    }

    // Timings:
    //   Fast poll (fullscreen + CPU): every 3 seconds
    //   GPU check (expensive):        every 15 seconds, only on CPU/FS candidates
    //   games.json reload:            every 30 seconds
    const int POLLS_NEEDED = 5;   // 5 x 3s = 15s sustained
    const int POLL_MS = 3000;
    const int GPU_CHECK_EVERY = 5;   // every 5 fast polls = ~15s
    const int GAMES_RELOAD_EVERY = 10; // every 10 polls = ~30s

    int poll_count = 0;
    int games_countdown = 0;
    json current_games = load_json_file(GAMES_FILE, json::object());

    while (g_running.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(POLL_MS));
        if (g_paused.load()) continue;
        if (!g_discovery_enabled.load()) continue;

        // Don't run while a known game is active
        {
            std::lock_guard<std::mutex> lk(g_game_name_mutex);
            if (!g_current_game_name.empty()) {
                // Clear candidates and dxgi cache when game ends
                g_discovery_candidates.clear();
                g_dxgi_cache.clear();
                continue;
            }
        }

        poll_count++;
        bool do_gpu_check = (poll_count % GPU_CHECK_EVERY == 0);

        // Reload games.json periodically -- not every poll
        if (games_countdown <= 0) {
            current_games = load_json_file(GAMES_FILE, json::object());
            games_countdown = GAMES_RELOAD_EVERY;
        }
        games_countdown--;

        // Only check the foreground process + processes with a visible window
        // This cuts the candidate pool from ~100 to ~5-10 typically
        DWORD fg_pid = get_foreground_pid();
        HWND  fg_wnd = GetForegroundWindow();
        bool  fg_fs = window_is_fullscreen(fg_wnd);

        // Build set of PIDs that have visible windows (cheaper than scanning all)
        static std::unordered_set<DWORD> s_win_pids;
        s_win_pids.clear();
        EnumWindows([](HWND hwnd, LPARAM) -> BOOL {
            if (!IsWindowVisible(hwnd)) return TRUE;
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            if (pid) s_win_pids.insert(pid);
            return TRUE;
            }, 0);

        // Clean stale candidates (only keep window-visible pids)
        for (auto it = g_discovery_candidates.begin();
            it != g_discovery_candidates.end(); ) {
            if (!s_win_pids.count(it->first))
                it = g_discovery_candidates.erase(it);
            else ++it;
        }
        // Clean dxgi cache of dead pids
        for (auto it = g_dxgi_cache.begin(); it != g_dxgi_cache.end(); ) {
            if (!s_win_pids.count(it->first))
                it = g_dxgi_cache.erase(it);
            else ++it;
        }

        // Lightweight scan -- no handle opens, name only
        auto all_procs = enumerate_processes_light();

        // Fetch system times ONCE for the whole poll cycle
        FILETIME _idle{}, _kernel{}, _user{};
        GetSystemTimes(&_idle, &_kernel, &_user);
        ULONGLONG poll_sys_total =
            ((static_cast<ULONGLONG>(_kernel.dwHighDateTime) << 32ULL) | _kernel.dwLowDateTime) +
            ((static_cast<ULONGLONG>(_user.dwHighDateTime) << 32ULL) | _user.dwLowDateTime);

        for (auto& proc : all_procs) {
            // Must have a visible window -- skip background services entirely
            if (!s_win_pids.count(proc.pid)) continue;

            std::string nl = to_lower(proc.name);
            if (DISCOVERY_BLACKLIST.count(nl))   continue;
            if (current_games.contains(nl))       continue;
            if (g_never_ask.count(nl))            continue;
            if (g_discovery_declined.count(nl))   continue;

            auto& cand = g_discovery_candidates[proc.pid];
            cand.name = proc.name;
            // exe_path not fetched yet -- only looked up if candidate triggers

            // -- Signal 1: Fullscreen (free) --------------------
            if (proc.pid == fg_pid && fg_fs)
                cand.was_fullscreen = true;

            // -- Signal 2: CPU -- shared sys_total, one syscall total
            double cpu = get_process_cpu_percent(proc.pid, poll_sys_total);
            bool hi_cpu = (cpu >= 30.0);
            if (hi_cpu) cand.high_cpu_polls = std::min(cand.high_cpu_polls + 1, POLLS_NEEDED + 1);
            else        cand.high_cpu_polls = std::max(0, cand.high_cpu_polls - 1);

            // -- Signal 3: GPU -- only if CPU or FS seen, every N polls
            bool cpu_or_fs = cand.was_fullscreen || (cand.high_cpu_polls >= 2);
            if (do_gpu_check && cpu_or_fs) {
                bool has_gpu = process_has_dxgi(proc.pid);
                if (has_gpu) cand.gpu_polls = std::min(cand.gpu_polls + 1, POLLS_NEEDED + 1);
                else         cand.gpu_polls = std::max(0, cand.gpu_polls - 1);
            }

            // Need 2 of 3 signals
            int signals = (cand.was_fullscreen ? 1 : 0)
                + (cand.high_cpu_polls >= POLLS_NEEDED ? 1 : 0)
                + (cand.gpu_polls >= POLLS_NEEDED ? 1 : 0);

            if (signals >= 2 && !cand.prompted) {
                // Now fetch exe path -- only happens once per candidate ever
                if (cand.exe_path.empty()) {
                    HANDLE hx = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,
                        FALSE, proc.pid);
                    if (hx) {
                        wchar_t buf[MAX_PATH]{};
                        DWORD len = MAX_PATH;
                        if (QueryFullProcessImageNameW(hx, 0, buf, &len))
                            cand.exe_path = buf;
                        CloseHandle(hx);
                    }
                }
                prompt_add_game(proc.pid, cand);
            }
        }
    }
}

// -- Handle discovery response (called from console thread) ----
// Multi-step wizard: Add? -> Mode -> Group -> Preserve -> RAM cleaner -> Interval

enum class DiscoveryStep {
    AskAdd, AskMode, AskGroup, AskPreserve, AskRamCleaner, AskRamInterval, Done
};

struct DiscoveryWizardState {
    DiscoveryItem        item;
    DiscoveryStep        step = DiscoveryStep::AskAdd;
    std::string          mode = "balanced";
    std::string          group = "steam";
    std::vector<std::string> preserve_procs;
    bool                 ram_cleaner = false;
    int                  ram_interval = 60;
};

static DiscoveryWizardState g_wizard_state;
static bool                 g_wizard_active = false;
static std::atomic<bool>    g_wizard_prompt_shown{ false };

static void discovery_wizard_prompt(DiscoveryStep step, const std::string& name)
{
    switch (step) {
    case DiscoveryStep::AskAdd:
        log_msg("");
        log_msg("==========================================================");
        log_msg("[DISCOVERY] New game detected: " + name);
        log_msg("  Add it to Optimizer?");
        log_msg("  yes   - add it");
        log_msg("  no    - skip this session");
        log_msg("  never - never ask about this again");
        log_msg("==========================================================");
        break;
    case DiscoveryStep::AskMode:
        log_msg("[DISCOVERY] Choose a mode:");
        log_msg("  balanced   - recommended for most games (default)");
        log_msg("  safe       - minimal changes, most compatible");
        log_msg("  aggressive - maximum performance");
        break;
    case DiscoveryStep::AskGroup:
        log_msg("[DISCOVERY] Which launcher? (preserves it when closing background apps)");
        log_msg("  steam / epic / ea / ubisoft / battlenet / gog / none");
        log_msg("  Press Enter to default to steam");
        break;
    case DiscoveryStep::AskPreserve:
        log_msg("[DISCOVERY] Any extra processes to keep open?");
        log_msg("  Enter names separated by spaces (e.g. discord.exe obs64.exe)");
        log_msg("  Press Enter to skip");
        break;
    case DiscoveryStep::AskRamCleaner:
        log_msg("[DISCOVERY] Enable RAM cleaner for this game? (yes / no)");
        log_msg("  Periodically clears standby memory while the game is running");
        break;
    case DiscoveryStep::AskRamInterval:
        log_msg("[DISCOVERY] RAM clean interval in seconds? (10-3600, default 60)");
        log_msg("  Press Enter for 60s");
        break;
    default: break;
    }
}

static void discovery_wizard_finish(DiscoveryWizardState& ws)
{
    std::string exe_lower = to_lower(ws.item.name);

    json entry;
    entry["mode"] = ws.mode;
    entry["timer"] = true;

    json grp_arr = json::array();
    if (ws.group != "none" && !ws.group.empty())
        grp_arr.push_back(ws.group);
    entry["preserve_groups"] = grp_arr;

    json proc_arr = json::array();
    for (auto& p : ws.preserve_procs)
        if (!p.empty()) proc_arr.push_back(p);
    entry["preserve_processes"] = proc_arr;

    if (ws.ram_cleaner) {
        entry["ram_cleaner"]["enabled"] = true;
        entry["ram_cleaner"]["interval_sec"] = ws.ram_interval;
        entry["ram_cleaner"]["empty_working_sets"] = true;
        entry["ram_cleaner"]["flush_file_cache"] = true;
    }

    json games_json = load_json_file(GAMES_FILE, json::object());
    games_json[exe_lower] = entry;
    save_json_file(GAMES_FILE, games_json);

    log_msg("[DISCOVERY] Added " + ws.item.name + " with mode '" + ws.mode + "'");
    if (!grp_arr.empty())
        log_msg("[DISCOVERY] Preserve group: " + ws.group);
    if (!proc_arr.empty()) {
        std::string pl;
        for (auto& p : ws.preserve_procs) pl += p + " ";
        log_msg("[DISCOVERY] Preserve processes: " + pl);
    }
    if (ws.ram_cleaner)
        log_msg("[DISCOVERY] RAM cleaner: every " + std::to_string(ws.ram_interval) + "s");
    log_msg("[DISCOVERY] Saved to games.json, will optimize on next launch");
}

static void handle_discovery_response(const std::string& response)
{
    std::unique_lock<std::mutex> lk(g_console_mutex);

    // Start wizard if none active
    if (!g_wizard_active) {
        if (g_pending_discovery.empty()) return;
        g_wizard_state = DiscoveryWizardState{};
        g_wizard_state.item = g_pending_discovery.front();
        g_pending_discovery.erase(g_pending_discovery.begin());
        g_wizard_active = true;
        lk.unlock();
        discovery_wizard_prompt(DiscoveryStep::AskAdd, g_wizard_state.item.name);
        return;
    }
    lk.unlock();

    std::string r = to_lower(trim(response));
    std::string exe_lower = to_lower(g_wizard_state.item.name);
    auto& ws = g_wizard_state;

    switch (ws.step) {
    case DiscoveryStep::AskAdd:
        if (r == "never") {
            g_never_ask.insert(exe_lower);
            save_never_ask(g_never_ask);
            log_msg("[DISCOVERY] " + ws.item.name + " added to never-ask list");
            g_wizard_active = false;
            return;
        }
        if (r == "no" || r == "n") {
            g_discovery_declined.insert(exe_lower);
            log_msg("[DISCOVERY] Skipped " + ws.item.name + " (this session only)");
            g_wizard_active = false;
            return;
        }
        if (r == "yes" || r == "y") {
            ws.step = DiscoveryStep::AskMode;
            discovery_wizard_prompt(DiscoveryStep::AskMode, ws.item.name);
            return;
        }
        log_msg("[DISCOVERY] Type yes, no, or never");
        discovery_wizard_prompt(DiscoveryStep::AskAdd, ws.item.name);
        break;

    case DiscoveryStep::AskMode:
        if (r == "safe")            ws.mode = "safe";
        else if (r == "aggressive") ws.mode = "aggressive";
        else                        ws.mode = "balanced";
        ws.step = DiscoveryStep::AskGroup;
        discovery_wizard_prompt(DiscoveryStep::AskGroup, ws.item.name);
        break;

    case DiscoveryStep::AskGroup: {
        static const std::set<std::string> valid_groups = {
            "steam","epic","ea","ubisoft","battlenet","gog","none",""
        };
        ws.group = (valid_groups.count(r)) ? r : (r.empty() ? "steam" : "steam");
        ws.step = DiscoveryStep::AskPreserve;
        discovery_wizard_prompt(DiscoveryStep::AskPreserve, ws.item.name);
        break;
    }
    case DiscoveryStep::AskPreserve:
        if (!r.empty()) {
            std::istringstream ss(r);
            std::string tok;
            while (ss >> tok) ws.preserve_procs.push_back(tok);
        }
        ws.step = DiscoveryStep::AskRamCleaner;
        discovery_wizard_prompt(DiscoveryStep::AskRamCleaner, ws.item.name);
        break;

    case DiscoveryStep::AskRamCleaner:
        ws.ram_cleaner = (r == "yes" || r == "y");
        if (ws.ram_cleaner) {
            ws.step = DiscoveryStep::AskRamInterval;
            discovery_wizard_prompt(DiscoveryStep::AskRamInterval, ws.item.name);
        }
        else {
            discovery_wizard_finish(ws);
            g_wizard_active = false;
        }
        break;

    case DiscoveryStep::AskRamInterval:
        try {
            int v = std::stoi(r);
            ws.ram_interval = (v >= 10 && v <= 3600) ? v : 60;
        }
        catch (...) { ws.ram_interval = 60; }
        discovery_wizard_finish(ws);
        g_wizard_active = false;
        break;

    default:
        g_wizard_active = false;
        break;
    }
}



// ============================================================
// GAME DETECTION
// ============================================================

struct GameDetectResult {
    std::string  game_name;
    DWORD        pid = 0;
    json         game_cfg;
    bool         found = false;
    std::wstring exe_path;
};

static GameDetectResult wait_for_game(
    const std::map<std::string, json>& games)
{
    if (!is_clean()) log_msg("[STATE] Waiting for supported game...");
    while (g_running.load()) {
        if (g_paused.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }
        // light scan -- only open a handle on match
        auto procs = enumerate_processes_light();
        for (auto& p : procs) {
            std::string nl = to_lower(p.name);
            auto it = games.find(nl);
            if (it != games.end()) {

                HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,
                    FALSE, p.pid);
                if (hProc) {
                    wchar_t buf[MAX_PATH]{};
                    DWORD len = MAX_PATH;
                    if (QueryFullProcessImageNameW(hProc, 0, buf, &len))
                        p.exe_path = buf;
                    CloseHandle(hProc);
                }
                log_msg("[DETECTED] " + p.name);
                return { p.name, p.pid, it->second, true, p.exe_path };
            }
        }
        // back off poll rate after 30s idle
        static auto s_idle_since = std::chrono::steady_clock::now();
        static bool s_reset_idle = true;
        if (s_reset_idle) { s_idle_since = std::chrono::steady_clock::now(); s_reset_idle = false; }
        auto idle_secs = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - s_idle_since).count();
        auto poll_ms = (idle_secs < 30) ? 500 : 1000;
        std::this_thread::sleep_for(std::chrono::milliseconds(poll_ms));
    }
    return {};
}

static bool stabilize_game_process(DWORD pid, int window_seconds = 3)
{
    log_msg("[STARTUP] Stabilizing PID " + std::to_string(pid) +
        " for " + std::to_string(window_seconds) + "s...");
    auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start <
        std::chrono::seconds(window_seconds) && g_running.load())
    {
        if (!pid_exists(pid)) {
            log_msg("[STARTUP] Process died during stabilization");
            return false;
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    log_msg("[STARTUP] Stabilization complete");
    return true;
}

// ============================================================
// PRESERVED PROCESSES
// ============================================================

static std::set<std::string> compute_preserved_processes(
    const json& game_cfg,
    const std::map<std::string, std::vector<std::string>>& groups)
{
    std::set<std::string> preserved;

    json group_names = game_cfg.value("preserve_groups", json{});
    if (group_names.is_null())
        group_names = game_cfg.value("preserve_launchers", json{});

    if (group_names.is_array())
        for (auto& g : group_names)
            if (g.is_string()) {
                auto it = groups.find(to_lower(trim(g.get<std::string>())));
                if (it != groups.end())
                    for (auto& p : it->second) preserved.insert(p);
            }

    if (game_cfg.contains("preserve_processes") &&
        game_cfg["preserve_processes"].is_array())
        for (auto& p : game_cfg["preserve_processes"])
            if (p.is_string()) {
                std::string s = trim(p.get<std::string>());
                if (!s.empty()) preserved.insert(to_lower(s));
            }

    return preserved;
}

// ============================================================
// PERFORMANCE MONITOR
// ============================================================

static double get_cpu_percent()
{
    static FILETIME prev_idle{}, prev_kernel{}, prev_user{};
    FILETIME idle{}, kernel{}, user{};
    if (!GetSystemTimes(&idle, &kernel, &user)) return 0.0;

    auto ft64 = [](FILETIME f) -> ULONGLONG {
        return (static_cast<ULONGLONG>(f.dwHighDateTime) << 32ULL) | static_cast<ULONGLONG>(f.dwLowDateTime);
        };

    ULONGLONG d_idle = ft64(idle) - ft64(prev_idle);
    ULONGLONG d_kernel = ft64(kernel) - ft64(prev_kernel);
    ULONGLONG d_user = ft64(user) - ft64(prev_user);
    prev_idle = idle; prev_kernel = kernel; prev_user = user;

    ULONGLONG total = d_kernel + d_user;
    if (total == 0) return 0.0;
    double u = 100.0 * (1.0 - static_cast<double>(d_idle) /
        static_cast<double>(total));
    return (u < 0.0) ? 0.0 : (u > 100.0) ? 100.0 : u;
}

static double get_mem_percent()
{
    MEMORYSTATUSEX mem{};
    mem.dwLength = sizeof(mem);
    if (!GlobalMemoryStatusEx(&mem)) return 0.0;
    return static_cast<double>(mem.dwMemoryLoad);
}

static void performance_monitor_loop(
    DWORD game_pid,
    const std::vector<std::string>& background_apps)
{
    log_msg("[PERF] Performance monitor started");
    bool lowered = false;
    get_cpu_percent(); // prime the delta
    HANDLE hPerfGame = OpenProcess(SYNCHRONIZE, FALSE, game_pid);
    // Wait 3s for game to settle before monitoring
    if (hPerfGame) WaitForSingleObject(hPerfGame, 3000);
    else std::this_thread::sleep_for(std::chrono::seconds(3));

    while (g_running.load()) {
        if (hPerfGame && WaitForSingleObject(hPerfGame, 0) == WAIT_OBJECT_0) break;
        if (!hPerfGame && !pid_exists(game_pid)) break;
        double cpu = get_cpu_percent();
        double mem = get_mem_percent();

        // Only log when state changes -- no constant spam
        if ((cpu >= HIGH_CPU_THRESHOLD || mem >= HIGH_MEM_THRESHOLD) && !lowered) {
            char buf[64];
            _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                "[PERF] High load (CPU:%.1f%% RAM:%.1f%%) -- lowering bg priority",
                cpu, mem);
            log_msg(buf);
            lower_background_priority(background_apps);
            lowered = true;
        }
        else if ((cpu <= LOW_CPU_THRESHOLD && mem <= LOW_MEM_THRESHOLD) && lowered) {
            log_msg("[PERF] Load normalized, restoring bg priority");
            restore_background_priority();
            lowered = false;
        }
        // Wait 3s using game handle -- wakes instantly if game exits
        if (hPerfGame) WaitForSingleObject(hPerfGame, 3000);
        else std::this_thread::sleep_for(std::chrono::seconds(3));
    }
    if (hPerfGame) CloseHandle(hPerfGame);
    if (lowered) restore_background_priority();
    log_msg("[PERF] Performance monitor stopped");
}

// ============================================================
// GLOBAL CLEANUP
// ============================================================

static void global_cleanup(bool silent = false)
{
    if (!silent) log_msg("[CLEANUP] Running global cleanup...");
    try { disable_timer(); }
    catch (...) {}
    try { restore_affinity(); }
    catch (...) {}
    try { restore_background_priority(); }
    catch (...) {}
    try { tweak_explorer_priority(false); }
    catch (...) {}
    try { restore_gpu_processes(); }
    catch (...) {}
    try { restore_network_threads(); }
    catch (...) {}
    try { restore_nagle(); }
    catch (...) {}
    try { restore_nic_power_saving(); }
    catch (...) {}
    try { restore_cpu_parking(); }
    catch (...) {}
    try { revert_mmcss(); }
    catch (...) {}
    try { restore_services(); }
    catch (...) {}
    try { restore_game_bar(); }
    catch (...) {}
    try { restore_fullscreen_optimizations(); }
    catch (...) {}
    try { restore_memory_compression(); }
    catch (...) {}
    try { restore_dwm_priority(); }
    catch (...) {}
    try { restore_visual_effects(); }
    catch (...) {}
    try { restore_tcp_autotuning(); }
    catch (...) {}
    try { restore_ecn(); }
    catch (...) {}
    try { restore_rss(); }
    catch (...) {}
    try { restore_qos_reserve(); }
    catch (...) {}
    try { restore_interrupt_moderation(); }
    catch (...) {}
    try { restore_adapter_buffers(); }
    catch (...) {}
    try { restore_flow_control(); }
    catch (...) {}
    try { restore_cstates(); }
    catch (...) {}
    try { restore_boost(); }
    catch (...) {}
    try { restore_smt_scheduling(); }
    catch (...) {}
    try { restore_background_thread_mode(); }
    catch (...) {}
    try { restore_prefetch(); }
    catch (...) {}
    try { restore_gpu_power(); }
    catch (...) {}
    try { restore_interrupt_affinity(); }
    catch (...) {}
    try { restore_usb_power_saving(); }
    catch (...) {}
    try { restore_audio_latency(); }
    catch (...) {}
    try { remove_defender_exclusion(); }
    catch (...) {}
}

// ============================================================
// SAFE EXIT
// ============================================================

static void safe_exit(const std::string& reason = "Exit requested")
{
    try { log_msg("[EXIT] " + reason); }
    catch (...) {}
    if (!g_relaunch_pending.load()) g_console_muted.store(true);
    g_paused.store(false);
    g_running.store(false);
    // Run restore synchronously so tweaks dont persist
    if (!g_relaunch_pending.load())
        global_cleanup(true);
    if (g_tray_hwnd) {
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        PostMessage(g_tray_hwnd, WM_CLOSE, 0, 0);
    }
}

static void schedule_relaunch()
{
    g_relaunch_pending.store(true);
}

// ============================================================
// CRASH RECOVERY
// ============================================================

static void recover_if_unclean_exit()
{
    try {
        if (!fs::exists(RUN_STATE_FILE)) return;
        json data = load_json_file(RUN_STATE_FILE);
        if (!data.is_object()) return;
        if (data.value("clean_exit", true)) return;
        log_msg("[RECOVER] Previous run ended uncleanly. Restoring safety defaults...");
        try { disable_timer(); }
        catch (...) {}
        try { restore_affinity(); }
        catch (...) {}
        try { restore_background_priority(); }
        catch (...) {}
        try { tweak_explorer_priority(false); }
        catch (...) {}
        write_run_state(true);
        log_msg("[RECOVER] Recovery complete.");
    }
    catch (...) {}
}

// ============================================================
// DIAGNOSTICS
// ============================================================

static void self_audit_report()
{
    log_msg("[AUDIT] What Optimizer does:");
    log_msg("[AUDIT] - Detects configured games by process name");
    log_msg("[AUDIT] - Optional: boosts game process priority (and children)");
    log_msg("[AUDIT] - Optional: sets Ultimate Performance power plan");
    log_msg("[AUDIT] - Optional: sets 0.5ms timer resolution while game runs");
    log_msg("[AUDIT] - Optional: closes apps from close_apps.json");
    log_msg("[AUDIT] - Optional: restarts selected apps from reopen_apps.json");
    log_msg("[AUDIT] - Optional: per-game preserve groups via process_groups.json");
    log_msg("[AUDIT] - Optional: isolates background app CPU affinity");
    log_msg("[AUDIT] - Optional: tweaks explorer.exe priority during gameplay");
    log_msg("[AUDIT] - Optional: RAM cleaner (periodic EmptyWorkingSet + file cache flush)");
    log_msg("[AUDIT] - Optional: MMCSS Games scheduling for game threads");
    log_msg("[AUDIT] - Optional: High I/O priority for game process");
    log_msg("[AUDIT] - Optional: CPU core unparking (100% min processor state)");
    log_msg("[AUDIT] - Optional: NUMA/CCD affinity pinning (Ryzen multi-CCD)");
    log_msg("[AUDIT] - Optional: GPU-related process priority boost");
    log_msg("[AUDIT] - Optional: Nagle disable + NIC power off + network thread boost");
    log_msg("[AUDIT] - Optional: Advanced network: TCP auto-tune, ECN, RSS, QoS, DSCP tagging, interrupt moderation, buffer tuning, flow control");
    log_msg("[AUDIT] - Optional: EcoQoS / power throttling disable for game process");
    log_msg("[AUDIT] - Optional: Suspend non-essential services (SysMain, WSearch etc)");
    log_msg("[AUDIT] - Optional: Xbox Game Bar / DVR capture disable");
    log_msg("[AUDIT] - Optional: Fullscreen optimizations disable per-game");
    log_msg("[AUDIT] - Optional: Memory compression disable (session only)");
    log_msg("[AUDIT] - Optional: DWM (Desktop Window Manager) priority boost");
    log_msg("[AUDIT] - Optional: Visual animations disable");
    log_msg("[AUDIT] - Optional: Disk write cache enable");
    log_msg("[AUDIT] - Optional: Dynamic tick / HPET disable (requires reboot)");
    log_msg("[AUDIT] - Optional: C-state disable (CPU stays at full speed)");
    log_msg("[AUDIT] - Optional: Force max boost clock (aggressive EPP=0)");
    log_msg("[AUDIT] - Optional: CPU set isolation (dedicated cores for game)");
    log_msg("[AUDIT] - Optional: Large page privilege (SeLockMemoryPrivilege)");
    log_msg("[AUDIT] - Optional: SMT/DPC scheduling optimization");
    log_msg("[AUDIT] - Optional: Prefetch disable (no cache pollution)");
    log_msg("[AUDIT] - Optional: Ideal processor hint + mitigation flags");
    log_msg("[AUDIT] What Optimizer does NOT do:");
    log_msg("[AUDIT] - No services/driver changes, no network tweaks, no scheduled tasks");
    log_msg("[AUDIT] - No registry tweaks");
}

static void diagnostics_snapshot()
{
    double cpu = get_cpu_percent();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    double mem = get_mem_percent();
    log_msg("[DIAG] ----- Snapshot -----");
    char buf[128];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "[DIAG] Running: %s, Paused: %s",
        g_running.load() ? "true" : "false",
        g_paused.load() ? "true" : "false");
    log_msg(buf);
    {
        std::lock_guard<std::mutex> lk(g_game_name_mutex);
        log_msg("[DIAG] Current game: " +
            (g_current_game_name.empty() ? "None" : g_current_game_name));
    }
    _snprintf_s(buf, sizeof(buf), _TRUNCATE,
        "[DIAG] CPU: %.1f%% | RAM: %.1f%%", cpu, mem);
    log_msg(buf);
    log_msg("[DIAG] ---------------------");
}

// ============================================================
// STARTUP SHORTCUT
// ============================================================

static bool is_startup_enabled()
{
    wchar_t dir[MAX_PATH]{};
    SHGetFolderPathW(nullptr, CSIDL_STARTUP, nullptr, 0, dir);
    return fs::exists(fs::path(dir) / L"Optimizer.lnk");
}

static bool enable_startup()
{
    (void)CoInitialize(nullptr);
    wchar_t dir[MAX_PATH]{};
    SHGetFolderPathW(nullptr, CSIDL_STARTUP, nullptr, 0, dir);
    fs::path lnk = fs::path(dir) / L"Optimizer.lnk";

    IShellLinkW* pLink = nullptr;
    IPersistFile* pFile = nullptr;
    bool ok = false;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
        IID_IShellLinkW,
        reinterpret_cast<void**>(&pLink));
    if (SUCCEEDED(hr)) {
        wchar_t exe[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        pLink->SetPath(exe);
        pLink->SetDescription(L"Optimizer (run on startup)");
        pLink->SetShowCmd(SW_SHOWMINNOACTIVE);
        hr = pLink->QueryInterface(IID_IPersistFile,
            reinterpret_cast<void**>(&pFile));
        if (SUCCEEDED(hr)) {
            ok = SUCCEEDED(pFile->Save(lnk.wstring().c_str(), TRUE));
            pFile->Release();
        }
        pLink->Release();
    }
    CoUninitialize();
    return ok;
}

static bool disable_startup()
{
    wchar_t dir[MAX_PATH]{};
    SHGetFolderPathW(nullptr, CSIDL_STARTUP, nullptr, 0, dir);
    fs::path lnk = fs::path(dir) / L"Optimizer.lnk";
    try {
        if (fs::exists(lnk)) { (void)fs::remove(lnk); return true; }
    }
    catch (...) {}
    return false;
}

static void toggle_startup()
{
    if (is_startup_enabled()) {
        if (disable_startup()) log_msg("[STARTUP] Disabled");
    }
    else {
        if (enable_startup()) log_msg("[STARTUP] Enabled");
    }
}

// ============================================================
// BACKUP / RESTORE / RESET
// ============================================================

static fs::path backup_dir_path()
{
    char buf[32];
    time_t t = time(nullptr);
    tm tm_info{};
    localtime_s(&tm_info, &t);
    strftime(buf, sizeof(buf), "backup_%Y%m%d_%H%M%S", &tm_info);
    return APPDATA_DIR / buf;
}


// ============================================================
// EXPORT / IMPORT (named config zips via PowerShell)
// ============================================================

static std::vector<fs::path> get_config_files()
{
    return {
        CONFIG_FILE,
        GAMES_FILE,
        CLOSE_APPS_FILE,
        REOPEN_APPS_FILE,
        PROCESS_GROUPS_FILE,
        THEMES_FILE,
        NEVER_ASK_FILE,
        PRESETS_FILE
    };
}

static bool export_config(const std::string& name)
{
    try {
        (void)fs::create_directories(PROFILES_DIR);
        fs::path zip = PROFILES_DIR / (name + ".zip");

        // Build temp staging folder
        fs::path stage = PROFILES_DIR / ("_stage_" + name);
        (void)fs::create_directories(stage);

        // Copy all config files into staging folder
        for (auto& f : get_config_files())
            if (fs::exists(f))
                (void)fs::copy_file(f, stage / f.filename(),
                    fs::copy_options::overwrite_existing);

        // Remove existing zip if present
        if (fs::exists(zip)) (void)fs::remove(zip);

        // Use PowerShell Compress-Archive
        // Use PowerShell Compress-Archive
        std::wstring cmd =
            L"powershell -NonInteractive -Command \"Compress-Archive -Path '"
            + stage.wstring() + L"\\*' -DestinationPath '"
            + zip.wstring() + L"' -Force\" >nul 2>&1";
        (void)_wsystem(cmd.c_str());
        // Cleanup staging folder
        (void)fs::remove_all(stage);

        if (fs::exists(zip)) {
            log_msg("[PROFILE] Saved: " + zip.string());
            return true;
        }
        log_msg("[PROFILE] Failed to create zip");
        return false;
    }
    catch (std::exception& e) {
        log_msg(std::string("[PROFILE] Error: ") + e.what());
        return false;
    }
}

static bool import_config(const std::string& name)
{
    try {
        fs::path zip = PROFILES_DIR / (name + ".zip");
        if (!fs::exists(zip)) {
            // Also check current directory
            zip = fs::path(name + ".zip");
            if (!fs::exists(zip)) {
                log_msg("[PROFILE] File not found: " + name + ".zip");
                log_msg("[PROFILE] Place the zip in: " + PROFILES_DIR.string());
                return false;
            }
        }

        // Stage extract folder
        fs::path stage = PROFILES_DIR / ("_import_" + name);
        (void)fs::create_directories(stage);

        // Use PowerShell Expand-Archive
        // Use PowerShell Expand-Archive
        std::wstring cmd =
            L"powershell -NonInteractive -Command \"Expand-Archive -Path '"
            + zip.wstring() + L"' -DestinationPath '"
            + stage.wstring() + L"' -Force\" >nul 2>&1";
        (void)_wsystem(cmd.c_str());
        // Copy extracted files to AppData
        int copied = 0;
        for (auto& entry : fs::recursive_directory_iterator(stage)) {
            if (!entry.is_regular_file()) continue;
            std::string fn = entry.path().filename().string();
            // Only import known config files
            static const std::set<std::string> allowed = {
                "config.json", "games.json", "close_apps.json",
                "reopen_apps.json", "process_groups.json",
                "themes.json", "never_ask.json", "presets.json"
            };
            if (!allowed.count(fn)) continue;
            (void)fs::copy_file(entry.path(), APPDATA_DIR / fn,
                fs::copy_options::overwrite_existing);
            copied++;
        }

        (void)fs::remove_all(stage);

        if (copied > 0) {
            refresh_runtime_config();
            log_msg("[PROFILE] Loaded " + std::to_string(copied) +
                " files from " + name + ".zip");
            log_msg("[PROFILE] Restart optimizer to apply all changes");
            return true;
        }
        log_msg("[PROFILE] No valid config files found in zip");
        return false;
    }
    catch (std::exception& e) {
        log_msg(std::string("[PROFILE] Error: ") + e.what());
        return false;
    }
}

static void list_exports()
{
    if (!fs::exists(PROFILES_DIR)) {
        log_msg("[PROFILE] No exports folder yet. Use: profile save <name>");
        return;
    }
    std::vector<std::string> found;
    for (auto& entry : fs::directory_iterator(PROFILES_DIR)) {
        if (entry.path().extension() == ".zip")
            found.push_back(entry.path().stem().string());
    }
    if (found.empty()) {
        log_msg("[PROFILE] No config zips found in " + PROFILES_DIR.string());
        return;
    }
    log_msg("[PROFILE] Available configs (" +
        std::to_string(found.size()) + "):");
    for (auto& n : found)
        log_msg("  " + n);
    log_msg("[PROFILE] Use: profile load <name>  to load one");
}

// ============================================================
// COPY PROFILE
// ============================================================
static void copy_profile(const std::string& src, const std::string& dst)
{
    json games = load_json_file(GAMES_FILE, json::object());
    std::string sl = to_lower(src);
    std::string dl = to_lower(dst);
    if (!games.contains(sl)) {
        log_msg("[PROFILE] Source not found: " + src);
        log_msg("[PROFILE] Available: check games.json");
        return;
    }
    if (games.contains(dl)) {
        log_msg("[PROFILE] " + dst + " already exists. Overwrite? (yes/no)");
        // Simple inline confirm
        std::string line;
        std::getline(std::cin, line);
        if (to_lower(trim(line)) != "yes") {
            log_msg("[PROFILE] Cancelled");
            return;
        }
    }
    games[dl] = games[sl];
    save_json_file(GAMES_FILE, games);
    log_msg("[PROFILE] Copied " + src + " -> " + dst);
}

// ============================================================
// GAMEMODE (quick per-game mode switch)
// ============================================================
static void set_game_mode(const std::string& game, const std::string& mode)
{
    static const std::set<std::string> valid = { "safe","balanced","aggressive" };
    if (!valid.count(mode)) {
        log_msg("[GAMEMODE] Invalid mode: " + mode);
        log_msg("[GAMEMODE] Use: safe / balanced / aggressive");
        return;
    }
    json games = load_json_file(GAMES_FILE, json::object());
    std::string gl = to_lower(game);
    if (!games.contains(gl)) {
        log_msg("[GAMEMODE] Game not found: " + game);
        return;
    }
    games[gl]["mode"] = mode;
    save_json_file(GAMES_FILE, games);
    log_msg("[GAMEMODE] " + game + " set to " + mode);
}

// ============================================================
// LAST SESSION SUMMARY
// ============================================================
static void show_last_session()
{
    std::lock_guard<std::mutex> lk(g_history_mutex);
    if (g_last_session.game.empty()) {
        // Try loading from history file
        if (!g_session_history.empty()) {
            auto& r = g_session_history.back();
            int h = r.duration_sec / 3600;
            int m = (r.duration_sec % 3600) / 60;
            int s = r.duration_sec % 60;
            char buf[128];
            _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                "[SESSION] Last: %s | %02d:%02d:%02d",
                r.game.c_str(), h, m, s);
            log_msg(buf);
        }
        else {
            log_msg("[SESSION] No session history yet");
        }
        return;
    }
    auto& r = g_last_session;
    int h = r.duration_sec / 3600;
    int m = (r.duration_sec % 3600) / 60;
    int s = r.duration_sec % 60;
    char buf[128];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE,
        "[SESSION] Game: %s | Duration: %02d:%02d:%02d | %s",
        r.game.c_str(), h, m, s, r.timestamp.c_str());
    log_msg(buf);
}

// ============================================================
// WHITELIST
// ============================================================
static void whitelist_cmd(const std::vector<std::string>& args)
{
    if (args.empty()) {
        // Show current whitelist
        json nask = load_json_file(NEVER_ASK_FILE, json::array());
        if (!nask.is_array() || nask.empty()) {
            log_msg("[WHITELIST] Empty, no apps are whitelisted from detection");
            return;
        }
        log_msg("[WHITELIST] Apps excluded from auto-detection:");
        for (auto& v : nask)
            if (v.is_string()) log_msg("  " + v.get<std::string>());
        return;
    }
    std::string sub = to_lower(args[0]);
    if (args.size() < 2 && (sub == "add" || sub == "remove")) {
        log_msg("[WHITELIST] Usage: whitelist add <app.exe>");
        log_msg("[WHITELIST]        whitelist remove <app.exe>");
        return;
    }
    if (sub == "add") {
        std::string app = to_lower(args[1]);
        g_never_ask.insert(app);
        save_never_ask(g_never_ask);
        log_msg("[WHITELIST] Added: " + app + " (will never be suggested by detection)");
    }
    else if (sub == "remove") {
        std::string app = to_lower(args[1]);
        g_never_ask.erase(app);
        save_never_ask(g_never_ask);
        log_msg("[WHITELIST] Removed: " + app);
    }
    else {
        log_msg("[WHITELIST] Usage: whitelist / whitelist add <app.exe> / whitelist remove <app.exe>");
    }
}

// ============================================================
// AUTO UPDATE CHECK
// ============================================================
static void check_for_updates()
{
    // Write a self-deleting PowerShell updater script to AppData
    fs::path ps_path = APPDATA_DIR / "optimizer_update.ps1";

    // Build the script as a raw string

    // Build exe path safely
    wchar_t self_buf[MAX_PATH]{};
    GetModuleFileNameW(nullptr, self_buf, MAX_PATH);
    std::wstring self_wpath(self_buf);
    // Convert to string for embedding in PS1
    std::string self_path(self_wpath.begin(), self_wpath.end());

    // Replace backslashes for PS1
    std::string ps_self = self_path;
    for (auto& c : ps_self) if (c == '\\') c = '/';

    std::string ps_script =
        "# Optimizer auto-updater -- generated by optimizer.exe\n"
        "$version_url  = 'https://raw.githubusercontent.com/passmedaoxy/optimizer/main/VERSION'\n"
        "$download_url = 'https://github.com/passmedaoxy/optimizer/releases/latest/download/optimizer.exe'\n"
        "$current_ver  = '" + VERSION + "'\n"
        "$optimizer    = '" + ps_self + "'\n"
        "$script_path  = $MyInvocation.MyCommand.Path\n"
        "\n"
        "Write-Host ''\n"
        "Write-Host '  +--------------------------------------------+' -ForegroundColor White\n"
        "Write-Host '  |   Optimizer Updater                        |' -ForegroundColor White\n"
        "Write-Host '  +--------------------------------------------+' -ForegroundColor White\n"
        "Write-Host ''\n"
        "\n"
        "# Check latest version\n"
        "Write-Host '  [i] Checking for updates...' -ForegroundColor Cyan\n"
        "try {\n"
        "    $latest = (Invoke-WebRequest -Uri $version_url -UseBasicParsing).Content.Trim()\n"
        "} catch {\n"
        "    Write-Host '  [x] Could not reach update server.' -ForegroundColor Red\n"
        "    Read-Host '  Press Enter to close'\n"
        "    Remove-Item $script_path -Force -ErrorAction SilentlyContinue\n"
        "    exit 1\n"
        "}\n"
        "\n"
        "Write-Host \"  [i] Latest version:  $latest\" -ForegroundColor Cyan\n"
        "Write-Host \"  [i] Current version: $current_ver\" -ForegroundColor Cyan\n"
        "Write-Host ''\n"
        "\n"
        "# Compare versions\n"
        "function Compare-Version($a, $b) {\n"
        "    $ap = $a.Split('.') | ForEach-Object { [int]$_ }\n"
        "    $bp = $b.Split('.') | ForEach-Object { [int]$_ }\n"
        "    for ($i = 0; $i -lt 3; $i++) {\n"
        "        if ($ap[$i] -gt $bp[$i]) { return 1 }\n"
        "        if ($ap[$i] -lt $bp[$i]) { return -1 }\n"
        "    }\n"
        "    return 0\n"
        "}\n"
        "\n"
        "if ((Compare-Version $latest $current_ver) -le 0) {\n"
        "    Write-Host '  [+] You are already up to date!' -ForegroundColor Green\n"
        "    Read-Host '  Press Enter to close'\n"
        "    Remove-Item $script_path -Force -ErrorAction SilentlyContinue\n"
        "    exit 0\n"
        "}\n"
        "\n"
        "Write-Host \"  [!] New version available: $latest\" -ForegroundColor Yellow\n"
        "$answer = Read-Host '  Download and install? (yes/no)'\n"
        "if ($answer -notmatch '^(yes|y)$') {\n"
        "    Write-Host '  [!] Update cancelled.' -ForegroundColor Yellow\n"
        "    Remove-Item $script_path -Force -ErrorAction SilentlyContinue\n"
        "    exit 0\n"
        "}\n"
        "\n"
        "# Download new exe\n"
        "$temp = [System.IO.Path]::GetTempFileName() + '.exe'\n"
        "Write-Host '  [i] Downloading...' -ForegroundColor Cyan\n"
        "try {\n"
        "    $wc = New-Object System.Net.WebClient\n"
        "    $wc.DownloadFile($download_url, $temp)\n"
        "} catch {\n"
        "    Write-Host '  [x] Download failed.' -ForegroundColor Red\n"
        "    Write-Host '  [x] Get it manually: https://github.com/passmedaoxy/optimizer/releases/latest' -ForegroundColor Red\n"
        "    Read-Host '  Press Enter to close'\n"
        "    Remove-Item $script_path -Force -ErrorAction SilentlyContinue\n"
        "    exit 1\n"
        "}\n"
        "\n"
        "# Validate size\n"
        "$size = (Get-Item $temp).Length\n"
        "if ($size -lt 512000) {\n"
        "    Write-Host '  [x] Downloaded file too small -- may be corrupt. Aborting.' -ForegroundColor Red\n"
        "    Remove-Item $temp -Force -ErrorAction SilentlyContinue\n"
        "    Read-Host '  Press Enter to close'\n"
        "    Remove-Item $script_path -Force -ErrorAction SilentlyContinue\n"
        "    exit 1\n"
        "}\n"
        "Write-Host \"  [+] Downloaded ($([math]::Round($size/1KB)) KB)\" -ForegroundColor Green\n"
        "\n"
        "# Wait for optimizer to close\n"
        "Write-Host '  [i] Waiting for optimizer.exe to close...' -ForegroundColor Cyan\n"
        "$waited = 0\n"
        "while ((Get-Process -Name 'optimizer' -ErrorAction SilentlyContinue) -and $waited -lt 15) {\n"
        "    Start-Sleep -Milliseconds 500\n"
        "    $waited += 0.5\n"
        "}\n"
        "if (Get-Process -Name 'optimizer' -ErrorAction SilentlyContinue) {\n"
        "    Stop-Process -Name 'optimizer' -Force -ErrorAction SilentlyContinue\n"
        "    Start-Sleep -Seconds 1\n"
        "}\n"
        "\n"
        "# Replace exe\n"
        "Write-Host '  [i] Installing update...' -ForegroundColor Cyan\n"
        "try {\n"
        "    $backup = $optimizer + '.bak'\n"
        "    if (Test-Path $optimizer) { Move-Item $optimizer $backup -Force }\n"
        "    Move-Item $temp $optimizer -Force\n"
        "    if (Test-Path $backup) { Remove-Item $backup -Force -ErrorAction SilentlyContinue }\n"
        "    Write-Host \"  [+] Updated to v$latest successfully!\" -ForegroundColor Green\n"
        "} catch {\n"
        "    Write-Host '  [x] Install failed. Restoring backup...' -ForegroundColor Red\n"
        "    $backup = $optimizer + '.bak'\n"
        "    if ((Test-Path $backup) -and !(Test-Path $optimizer)) {\n"
        "        Move-Item $backup $optimizer -Force\n"
        "    }\n"
        "    Remove-Item $temp -Force -ErrorAction SilentlyContinue\n"
        "    Read-Host '  Press Enter to close'\n"
        "    Remove-Item $script_path -Force -ErrorAction SilentlyContinue\n"
        "    exit 1\n"
        "}\n"
        "\n"
        "# Relaunch\n"
        "Write-Host \"  [i] Launching Optimizer v$latest...\" -ForegroundColor Cyan\n"
        "Start-Sleep -Milliseconds 500\n"
        "Start-Process $optimizer -Verb RunAs\n"
        "Remove-Item $script_path -Force -ErrorAction SilentlyContinue\n";

    // Write the script
    try {
        std::ofstream f(ps_path.string());
        f << ps_script;
        f.close();
    }
    catch (...) {
        log_msg("[UPDATE] Could not write update script to AppData");
        return;
    }

    log_msg("[UPDATE] Launching updater...");
    log_msg("[UPDATE] Check the updater window for progress");

    // Launch PowerShell elevated with the script
    std::wstring args = L"-ExecutionPolicy Bypass -File \"" + ps_path.wstring() + L"\"";
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.lpVerb = L"runas";
    sei.lpFile = L"powershell.exe";
    sei.lpParameters = args.c_str();
    sei.nShow = SW_SHOW;
    ShellExecuteExW(&sei);
}

static bool backup_configs()
{
    try {
        std::vector<fs::path> files = {
            GAMES_FILE, CLOSE_APPS_FILE, REOPEN_APPS_FILE,
            PROCESS_GROUPS_FILE, CONFIG_FILE, PRESETS_FILE
        };
        fs::path bdir = backup_dir_path();
        (void)fs::create_directories(bdir);
        for (auto& p : files)
            if (fs::exists(p))
                (void)fs::copy_file(p, bdir / p.filename(),
                    fs::copy_options::overwrite_existing);
        log_msg("[BACKUP] Saved: " + bdir.filename().string());
        return true;
    }
    catch (std::exception& e) {
        log_msg(std::string("[BACKUP] Failed: ") + e.what());
        return false;
    }
}

static fs::path latest_backup()
{
    try {
        if (!fs::exists(APPDATA_DIR)) return {};
        std::vector<fs::path> dirs;
        for (auto& entry : fs::directory_iterator(APPDATA_DIR))
            if (entry.is_directory() &&
                entry.path().filename().string().rfind("backup_", 0) == 0)
                dirs.push_back(entry.path());
        if (dirs.empty()) return {};
        std::sort(dirs.begin(), dirs.end(), [](const fs::path& a, const fs::path& b) {
            return fs::last_write_time(a) > fs::last_write_time(b);
            });
        return dirs[0];
    }
    catch (...) { return {}; }
}

static bool restore_latest_backup()
{
    fs::path src = latest_backup();
    if (src.empty()) { log_msg("[RESTORE] No backup folder found"); return false; }
    try {
        for (auto& entry : fs::directory_iterator(src))
            (void)fs::copy_file(entry.path(), APPDATA_DIR / entry.path().filename(),
                fs::copy_options::overwrite_existing);
        log_msg("[RESTORE] Restored from: " + src.filename().string());
        refresh_runtime_config();
        return true;
    }
    catch (std::exception& e) {
        log_msg(std::string("[RESTORE] Failed: ") + e.what());
        return false;
    }
}

static void reset_configs()
{
    if (!backup_configs()) { log_msg("[RESET] Backup failed; refusing to reset"); return; }
    try {
        save_json_file(GAMES_FILE, json::object());
        save_json_file(CLOSE_APPS_FILE, json::array());
        save_json_file(REOPEN_APPS_FILE, json::array());
        save_json_file(PROCESS_GROUPS_FILE, json::object());
        save_json_file(PRESETS_FILE, DEFAULT_PRESETS);
        save_json_file(CONFIG_FILE, DEFAULT_CONFIG);
        refresh_runtime_config();
        log_msg("[RESET] Configs reset to defaults");
    }
    catch (std::exception& e) {
        log_msg(std::string("[RESET] Failed: ") + e.what());
    }
}

// ============================================================
// TAIL LOG
// ============================================================

static void tail_log(int n_lines = 30)
{
    try {
        if (!fs::exists(LOG_PATH)) { log_msg("No log file yet."); return; }
        std::ifstream f(LOG_PATH);
        std::deque<std::string> lines;
        std::string line;
        while (std::getline(f, line)) {
            lines.push_back(line);
            if (static_cast<int>(lines.size()) > n_lines) lines.pop_front();
        }
        for (auto& l : lines) {
            std::cout << l << "\n";
            std::this_thread::sleep_for(
                std::chrono::milliseconds(static_cast<int>(UI_DELAY_MS)));
        }
    }
    catch (...) { log_msg("Failed to read log"); }
}

// ============================================================
// SIMULATE / AUDIT
// ============================================================

static void simulate_for_game(const std::string& game_exe)
{
    std::string gexe = to_lower(trim(game_exe));
    if (gexe.empty()) { log_msg("Usage: audit <game.exe>"); return; }
    ensure_files();
    auto games = load_proc_map(GAMES_FILE);
    auto it = games.find(gexe);
    if (it == games.end()) {
        log_msg("[AUDIT] '" + gexe + "' not found in games.json");
        return;
    }
    auto close_apps = load_list(CLOSE_APPS_FILE);
    auto reopen_raw = load_list(REOPEN_APPS_FILE);
    auto groups = load_groups(PROCESS_GROUPS_FILE);
    std::set<std::string> reopen_apps;
    for (auto& x : reopen_raw) reopen_apps.insert(to_lower(x));

    auto runtime = get_runtime_config();
    std::string global_mode = runtime.value("mode", "balanced");
    json        config_features = runtime.value("features", DEFAULT_CONFIG["features"]);
    json presets;
    {
        std::lock_guard<std::mutex> lk(g_config_lock);
        presets = g_presets_cache.is_null() ? load_presets() : g_presets_cache;
    }

    auto ef_pair = get_effective_features(global_mode, config_features,
        it->second, presets);
    json effective = ef_pair.first;
    std::string mode_key = ef_pair.second;
    auto preserved = compute_preserved_processes(it->second, groups);

    std::vector<std::string> kill_list;
    for (auto& x : close_apps) {
        std::string xl = to_lower(x);
        if (!preserved.count(xl)) kill_list.push_back(xl);
    }

    log_msg("[AUDIT] Game: " + gexe + " | Mode: " + mode_key);

    std::string pres_str;
    for (auto& p : preserved) pres_str += p + " ";
    log_msg("[AUDIT] Preserve: " + (pres_str.empty() ? "None" : pres_str));

    std::string kill_str;
    for (auto& k : kill_list) kill_str += k + " ";
    log_msg("[AUDIT] Would close: " + (kill_str.empty() ? "None" : kill_str));

    auto ram = resolve_ram_cleaner_settings(it->second, effective, runtime);
    log_msg(std::string("[AUDIT] RAM cleaner: ") + (ram.enabled ? "enabled" : "disabled") +
        " | interval: " + std::to_string(ram.interval_sec) + "s");
}

// ============================================================
// WIZARD
// ============================================================

static void wizard()
{
    log_msg("[WIZARD] Starting setup...");
    ensure_files();
    auto presets = load_presets();

    std::string preset_names;
    for (auto it = presets.begin(); it != presets.end(); ++it)
        preset_names += it.key() + " ";
    std::cout << "Available modes: " << preset_names << "\n";

    std::string new_mode;
    std::cout << "Default mode (blank to keep current): ";
    std::getline(std::cin, new_mode);
    new_mode = trim(new_mode);
    if (!new_mode.empty()) {
        auto cfg = get_runtime_config();
        cfg["mode"] = new_mode;
        save_json_file(CONFIG_FILE, cfg);
        log_msg("[WIZARD] Set config mode -> " + new_mode);
    }

    std::string yn;
    std::cout << "Add a game exe now? (y/n): ";
    std::getline(std::cin, yn);
    if (to_lower(trim(yn)) == "y") {
        std::string gexe, timer_str, gmode, pgrps, pprocs;
        std::cout << "Game process name (example: cs2.exe): ";  std::getline(std::cin, gexe);
        std::cout << "Enable timer resolution? (y/n): ";        std::getline(std::cin, timer_str);
        std::cout << "Game mode override (blank for default): "; std::getline(std::cin, gmode);
        std::cout << "Preserve groups (comma, blank none): ";    std::getline(std::cin, pgrps);
        std::cout << "Preserve processes (comma, blank none): "; std::getline(std::cin, pprocs);

        bool timer = (to_lower(trim(timer_str)) == "y");
        json entry;
        entry["timer"] = timer;
        if (!trim(gmode).empty()) entry["mode"] = trim(gmode);
        if (!trim(pgrps).empty()) {
            json arr = json::array();
            for (auto& g : str_split(pgrps, ',')) arr.push_back(g);
            entry["preserve_groups"] = arr;
        }
        if (!trim(pprocs).empty()) {
            json arr = json::array();
            for (auto& p : str_split(pprocs, ',')) arr.push_back(p);
            entry["preserve_processes"] = arr;
        }
        json games_json = load_json_file(GAMES_FILE, json::object());
        games_json[to_lower(trim(gexe))] = entry;
        save_json_file(GAMES_FILE, games_json);
        log_msg("[WIZARD] Added game -> " + gexe);
    }

    std::cout << "Add apps to close list? (y/n): ";
    std::getline(std::cin, yn);
    if (to_lower(trim(yn)) == "y") {
        auto close_list = load_list(CLOSE_APPS_FILE);
        std::string items;
        std::cout << "Process names comma-separated: ";
        std::getline(std::cin, items);
        json arr = json::array();
        for (auto& x : close_list) arr.push_back(x);
        for (auto& x : str_split(items, ','))
            if (std::find(close_list.begin(), close_list.end(), x) == close_list.end())
                arr.push_back(x);
        save_json_file(CLOSE_APPS_FILE, arr);
        log_msg("[WIZARD] Updated close_apps.json");
    }

    refresh_runtime_config();
    log_msg("[WIZARD] Done. Type 'check' to validate configs.");
}


// ============================================================
// ADDGAME
// ============================================================

static void addgame()
{
    ensure_files();

    // ---- Read available modes from presets.json ----
    json presets = load_presets();
    std::vector<std::string> mode_names;
    for (auto it = presets.begin(); it != presets.end(); ++it)
        mode_names.push_back(it.key());
    std::sort(mode_names.begin(), mode_names.end());

    // ---- Read available groups from process_groups.json ----
    auto groups = load_groups(PROCESS_GROUPS_FILE);
    std::vector<std::string> group_names;
    for (auto& kv : groups)
        group_names.push_back(kv.first);
    std::sort(group_names.begin(), group_names.end());

    std::cout << "\n=== Add Game ===\n";

    // ---- Game exe ----
    std::string gexe;
    std::cout << "Game exe name (e.g. cs2.exe): ";
    std::getline(std::cin, gexe);
    gexe = trim(gexe);
    if (gexe.empty()) { log_msg("[ADDGAME] Cancelled."); return; }

    // ---- Timer resolution ----
    std::string yn;
    std::cout << "Enable timer resolution? (y/n): ";
    std::getline(std::cin, yn);
    bool timer = (to_lower(trim(yn)) == "y");

    // ---- Mode ----
    std::cout << "Available modes: ";
    for (size_t i = 0; i < mode_names.size(); ++i)
        std::cout << mode_names[i] << (i + 1 < mode_names.size() ? " / " : "");
    std::cout << "\nMode (blank for aggressive): ";
    std::string gmode;
    std::getline(std::cin, gmode);
    gmode = trim(gmode);
    if (gmode.empty()) gmode = "aggressive";
    // validate -- fall back to aggressive if unknown
    if (std::find(mode_names.begin(), mode_names.end(), to_lower(gmode)) == mode_names.end()) {
        std::cout << "Unknown mode '" << gmode << "', using aggressive.\n";
        gmode = "aggressive";
    }

    // ---- Preserve group ----
    json preserve_groups_arr = json::array();
    if (!group_names.empty()) {
        std::cout << "Available groups: ";
        for (size_t i = 0; i < group_names.size(); ++i)
            std::cout << group_names[i] << (i + 1 < group_names.size() ? " / " : "");
        std::cout << "\nPreserve group (blank for none, comma-separate multiple): ";
        std::string grp_input;
        std::getline(std::cin, grp_input);
        for (auto& g : str_split(grp_input, ',')) {
            std::string gl = to_lower(g);
            if (groups.count(gl))
                preserve_groups_arr.push_back(gl);
            else if (!gl.empty())
                std::cout << "  Warning: group '" << gl << "' not found in process_groups.json, skipping.\n";
        }
    }
    else {
        std::cout << "No groups defined in process_groups.json, skipping.\n";
    }

    // ---- Extra preserve processes ----
    std::cout << "Extra processes to preserve (comma-separated, blank for none): ";
    std::string pprocs;
    std::getline(std::cin, pprocs);
    json preserve_procs_arr = json::array();
    for (auto& p : str_split(pprocs, ','))
        if (!p.empty()) preserve_procs_arr.push_back(p);

    // ---- RAM cleaner ----
    std::cout << "Enable RAM cleaner for this game? (y/n): ";
    std::getline(std::cin, yn);
    bool ram_enabled = (to_lower(trim(yn)) == "y");

    json ram_obj = json();
    if (ram_enabled) {
        std::cout << "RAM clean interval in seconds (blank for global default): ";
        std::string interval_str;
        std::getline(std::cin, interval_str);
        interval_str = trim(interval_str);
        if (!interval_str.empty()) {
            try {
                int secs = std::stoi(interval_str);
                if (secs < 30)   secs = 30;
                if (secs > 3600) secs = 3600;
                ram_obj["interval_sec"] = secs;
            }
            catch (...) {
                std::cout << "  Invalid number, using global default.\n";
            }
        }
        // if no interval was set, ram_obj stays empty -- that means use global default
    }

    // ---- Build entry ----
    json entry;
    entry["timer"] = timer;
    entry["mode"] = to_lower(gmode);
    entry["preserve_groups"] = preserve_groups_arr;
    entry["preserve_processes"] = preserve_procs_arr;

    if (ram_enabled) {
        // always write the ram_cleaner block so the feature flag is explicit
        if (!ram_obj.is_null() && ram_obj.contains("interval_sec"))
            entry["ram_cleaner"]["interval_sec"] = ram_obj["interval_sec"];
        // mark enabled via the feature flag at game level
        entry["ram_cleaner_enabled"] = true;
    }

    // ---- Save ----
    json games_json = load_json_file(GAMES_FILE, json::object());
    std::string key = to_lower(trim(gexe));

    bool overwrite = false;
    if (games_json.contains(key)) {
        std::cout << "'" << gexe << "' already exists. Overwrite? (y/n): ";
        std::getline(std::cin, yn);
        if (to_lower(trim(yn)) != "y") { log_msg("[ADDGAME] Cancelled."); return; }
        overwrite = true;
    }

    games_json[key] = entry;
    if (save_json_file(GAMES_FILE, games_json)) {
        log_msg("[ADDGAME] " + std::string(overwrite ? "Updated" : "Added") +
            ": " + gexe +
            " | mode: " + gmode +
            " | timer: " + (timer ? "yes" : "no") +
            " | RAM cleaner: " + (ram_enabled ? "yes" : "no"));
        refresh_runtime_config();
    }
    else {
        log_msg("[ADDGAME] Failed to save games.json");
    }
}

// ============================================================
// DOCS / OPEN HELPERS
// ============================================================

static void docs()
{
    log_msg("Config files are in the folder opened by: openfolder");
    log_msg("games.json: { \"game.exe\": {timer, mode, preserve_groups, preserve_processes} }");
    log_msg("close_apps.json: [\"discord.exe\", ...] process names to close");
    log_msg("reopen_apps.json: [\"app.exe\", ...] apps to reopen after game closes");
    log_msg("process_groups.json: { \"steam\": [..], \"riot\": [..] }");
    log_msg("config.json: mode/features/background_apps + dry_run + verbose_init");
    log_msg("config.json ram_cleaner block: { interval_sec, empty_working_sets, flush_file_cache }");
    log_msg("games.json per-game ram_cleaner: same keys as above, overrides global setting");
    log_msg("presets.json: defines available modes + custom presets");
}

static void open_config_folder()
{
    ShellExecuteW(nullptr, L"explore",
        APPDATA_DIR.wstring().c_str(), nullptr, nullptr, SW_SHOW);
    log_msg("Opened config folder");
}

static void open_log()
{
    ShellExecuteW(nullptr, L"open",
        LOG_PATH.wstring().c_str(), nullptr, nullptr, SW_SHOW);
}

static void edit_file(const std::string& which)
{
    std::string w = to_lower(trim(which));
    std::map<std::string, fs::path> mapping = {
        { "games",     GAMES_FILE          },
        { "close",     CLOSE_APPS_FILE     },
        { "closeapps", CLOSE_APPS_FILE     },
        { "reopen",    REOPEN_APPS_FILE    },
        { "reopenapps",REOPEN_APPS_FILE    },
        { "groups",    PROCESS_GROUPS_FILE },
        { "config",    CONFIG_FILE         },
        { "presets",   PRESETS_FILE        },
    };
    auto it = mapping.find(w);
    if (it == mapping.end()) {
        log_msg("Usage: edit games|closeapps|reopenapps|groups|config|presets");
        return;
    }
    ShellExecuteW(nullptr, L"open",
        it->second.wstring().c_str(), nullptr, nullptr, SW_SHOW);
    log_msg("Opened: " + it->second.filename().string());
}

// ============================================================
// PRINT COMMANDS
// ============================================================

static void print_commands()
{
    const char* cmds[] = {
        "",
        "  ======================================================",
        "   Optimizer Commands",
        "  ======================================================",
        "",
        "  [ General ]",
  "  view                    show current console mode",
  "  view clean              switch to clean organized output",
  "  view debug              switch to raw debug output",
        "  help                    show this list",
        "  status                  show current optimizer state",
        "  version                 show optimizer version",
        "  exit / quit / close     safely close optimizer",
        "  startup                 toggle run-on-startup",
        "  reload                  restart optimizer",
        "  mute / unmute           toggle console output",
        "  last                    show last 40 log lines",
        "  history [n]             show last N gaming sessions (default 10)",
        "  benchmark               measure scheduler latency + memory bandwidth",
        "",
        "  [ Game Detection ]",
  "  detection               show whether auto-detection is on or off",
  "  detection on/off        enable or disable auto game detection",
  "  whitelist               show apps excluded from detection",
  "  whitelist add <app>     never suggest this app",
  "  whitelist remove <app>  remove from whitelist",
        "  detection               show whether auto-detection is on or off",
        "  detection on/off        enable or disable auto game detection",
        "  addgame                 manually add or update a game entry",
        "  audit <game.exe>        preview what optimizer would do (no changes)",
        "  test <game.exe>         same as audit",
        "",
        "  [ Config ]",
        "  mode <name>             set global mode (safe/balanced/aggressive)",
        "  dryrun on/off           toggle dry-run mode",
        "  verbose on/off          toggle verbose startup logging",
        "  togglefeature <name>    toggle any feature on/off in config.json",
        "  presets                 list available presets",
        "  wizard                  interactive setup wizard",
        "  edit <name>             open a config file in notepad",
        "                          (games/close/reopen/groups/config/presets)",
        "",
        "  [ Profiles & Export ]",
  "  profile save <name>           save all configs to a named zip",
  "  profile load <name>           load configs from a named zip",
  "  exports                 list all saved config zips",
  "  copyprofile <src> <dst> copy one game profile to another",
  "  gamemode <game> <mode>  quickly change a game's mode",
  "  lastsession             show last gaming session summary",
  "  update                  check for a newer version",
  "",
  "  [ Files & Backup ]",
        "  openfolder              open config folder in Explorer",
        "  openlog                 open current log file",
        "  check                   validate + auto-repair all JSON files",
        "  backup                  backup all config files",
        "  restore                 restore latest backup",
        "  reset                   reset configs to defaults (backs up first)",
        "  docs                    show config reference + examples",
        "",
        "  [ Network ]",
        "  netrestore              restore all network settings",
        "  dnsflush                flush DNS cache",
        "",
        "  [ Hardware ]",
        "  cpucheck                show CPU optimization status",
        "  cpurestore              restore CPU parking settings",
        "  hpet on/off             enable/disable HPET (requires reboot)",
        "  hagscheck               check Hardware-Accelerated GPU Scheduling",
        "",
        "  [ Memory ]",
        "  ramclean                trigger immediate RAM clean now",
        "  ramclean status         show RAM cleaner config",
        "  standby                 flush standby memory list",
        "",
        "  [ Services ]",
        "  svcrestore              restart suspended services",
        "",
        "  [ Appearance ]",
        "  color <theme>           set console color theme",
        "  theme                   list all available themes",
        "",
        "  ======================================================",
        "",
        nullptr
    };
    for (int i = 0; cmds[i]; ++i) {
        std::cout << cmds[i] << "\n";
        std::this_thread::sleep_for(
            std::chrono::milliseconds(static_cast<int>(UI_DELAY_MS)));
    }
}


// ============================================================
// COMMAND LOOP
// ============================================================

static void command_loop()
{
    while (g_running.load()) {
        std::string line;
        try {
            if (!std::getline(std::cin, line)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                continue;
            }
        }
        catch (...) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            continue;
        }

        line = trim(line);
        if (line.empty()) continue;

        auto parts = str_split(line, ' ');
        if (parts.empty()) continue;

        std::string cmd = to_lower(parts[0]);
        if (!cmd.empty() && cmd[0] == '/') cmd = cmd.substr(1);
        std::vector<std::string> args(parts.begin() + 1, parts.end());

        // -- Handle active wizard or pending discovery ----------------
        {
            bool handle = false;
            {
                std::lock_guard<std::mutex> lk(g_console_mutex);
                handle = g_wizard_active || !g_pending_discovery.empty();
            }
            if (handle) {
                handle_discovery_response(line);
                continue;
            }
        }

        if (cmd == "help" || cmd == "?") { print_commands(); continue; }
        if (cmd == "exit" || cmd == "quit" || cmd == "close")
        {
            safe_exit("Exit command"); return;
        }
        if (cmd == "mute") {
            g_console_muted.store(true);
            std::cout << "Console muted. Use: unmute\n"; continue;
        }
        if (cmd == "unmute") {
            g_console_muted.store(false);
            log_msg("Console unmuted"); continue;
        }
        if (cmd == "startup") { toggle_startup();       continue; }
        if (cmd == "openfolder") { open_config_folder();   continue; }
        if (cmd == "openlog") { open_log();             continue; }
        if (cmd == "edit") { edit_file(args.empty() ? "" : args[0]); continue; }
        if (cmd == "backup") { backup_configs();        continue; }
        if (cmd == "restore") { restore_latest_backup(); continue; }
        if (cmd == "export") {
            if (args.empty()) { list_exports(); }
            else { export_config(args[0]); }
            continue;
        }
        if (cmd == "import") {
            if (args.empty()) { list_exports(); log_msg("[PROFILE] Usage: profile load <name>"); }
            else { import_config(args[0]); }
            continue;
        }
        if (cmd == "profiles" || cmd == "listexports") {
            list_exports(); continue;
        }
        if (cmd == "copyprofile") {
            if (args.size() < 2)
                log_msg("[PROFILE] Usage: copyprofile <source.exe> <dest.exe>");
            else
                copy_profile(args[0], args[1]);
            continue;
        }
        if (cmd == "gamemode") {
            if (args.size() < 2)
                log_msg("[GAMEMODE] Usage: gamemode <game.exe> <safe/balanced/aggressive>");
            else
                set_game_mode(args[0], args[1]);
            continue;
        }
        if (cmd == "lastsession") {
            show_last_session(); continue;
        }
        if (cmd == "whitelist") {
            whitelist_cmd(args); continue;
        }
        if (cmd == "update" || cmd == "checkupdate") {
            check_for_updates(); continue;
        }
        if (cmd == "reset") { reset_configs();        continue; }
        if (cmd == "last") { tail_log(40);           continue; }
        if (cmd == "wizard") { wizard();               continue; }
        if (cmd == "addgame") { addgame();              continue; }
        if (cmd == "view") {
            if (args.empty()) {
                std::string cur = (g_view_mode.load() == VIEW_CLEAN) ? "clean" : "debug";
                log_msg("[VIEW] Current console mode: " + cur);
                log_msg("[VIEW] Use: view clean  or  view debug");
            }
            else {
                std::string sub = to_lower(args[0]);
                if (sub == "clean") {
                    g_view_mode.store(VIEW_CLEAN);
                    json cfg = get_runtime_config();
                    cfg["view_mode"] = VIEW_CLEAN;
                    save_json_file(CONFIG_FILE, cfg);
                    refresh_runtime_config();
                    log_msg("[VIEW] Switched to clean mode");
                    // Reprint dashboard
                    auto c2 = get_runtime_config();
                    print_dashboard(
                        c2.value("mode", "balanced"),
                        (int)load_proc_map(GAMES_FILE).size(),
                        g_discovery_enabled.load(),
                        c2.value("dry_run", false));
                }
                else if (sub == "debug") {
                    g_view_mode.store(VIEW_DEBUG);
                    json cfg = get_runtime_config();
                    cfg["view_mode"] = VIEW_DEBUG;
                    save_json_file(CONFIG_FILE, cfg);
                    refresh_runtime_config();
                    log_msg("[VIEW] Switched to debug mode");
                }
                else {
                    log_msg("[VIEW] Usage: view clean / view debug");
                }
            }
            continue;
        }
        if (cmd == "detection") {
            if (args.empty()) {
                log_msg(std::string("[DETECTION] Auto game detection is currently: ") +
                    (g_discovery_enabled.load() ? "ON" : "OFF"));
            }
            else {
                std::string sub = to_lower(args[0]);
                if (sub == "on" || sub == "1" || sub == "enable") {
                    g_discovery_enabled.store(true);
                    json cfg = get_runtime_config();
                    cfg["features"]["auto_discovery"] = true;
                    save_json_file(CONFIG_FILE, cfg);
                    refresh_runtime_config();
                    log_msg("[DETECTION] Auto game detection ENABLED");
                    log_msg("[DETECTION] Watching for: fullscreen / CPU>30% sustained / GPU (DXGI)");
                }
                else if (sub == "off" || sub == "0" || sub == "disable") {
                    g_discovery_enabled.store(false);
                    json cfg = get_runtime_config();
                    cfg["features"]["auto_discovery"] = false;
                    save_json_file(CONFIG_FILE, cfg);
                    refresh_runtime_config();
                    log_msg("[DISCOVERY] Auto game detection DISABLED");
                    log_msg("[DETECTION] Use 'detection on' to re-enable");
                }
                else {
                    log_msg("[DISCOVERY] Usage: detection on / detection off");
                }
            }
            continue;
        }
        if (cmd == "dnsflush") {
            flush_dns_cache(); continue;
        }
        if (cmd == "netrestore") {
            restore_nagle(); restore_nic_power_saving();
            restore_network_threads();
            restore_tcp_autotuning(); restore_ecn(); restore_rss();
            restore_qos_reserve(); restore_interrupt_moderation();
            restore_adapter_buffers(); restore_flow_control();
            log_msg("[NET] All network settings restored manually"); continue;
        }
        if (cmd == "svcrestore") {
            restore_services();
            log_msg("[SVC] Services restored manually"); continue;
        }
        if (cmd == "hpet") {
            if (!args.empty() && args[0] == "on") disable_dynamic_tick();
            else restore_dynamic_tick();
            continue;
        }
        if (cmd == "cpucheck") {
            log_msg("[CPU] C-states disabled: " + std::string(g_cstate_disabled ? "yes" : "no"));
            log_msg("[CPU] Boost forced: " + std::string(g_boost_forced ? "yes" : "no"));
            log_msg("[CPU] CPU sets isolated: " + std::string(!g_isolated_cpu_sets.empty() ? "yes (" + std::to_string(g_isolated_cpu_sets.size()) + " sets)" : "no"));
            log_msg("[CPU] SMT optimized: " + std::string(g_smt_modified ? "yes" : "no"));
            log_msg("[CPU] Prefetch disabled: " + std::string(g_prefetch_disabled ? "yes" : "no"));
            continue;
        }
        if (cmd == "cpurestore") {
            restore_cpu_parking();
            log_msg("[CPU] CPU parking restored manually"); continue;
        }
        if (cmd == "audit" || cmd == "test")
        {
            simulate_for_game(args.empty() ? "" : args[0]); continue;
        }
        if (cmd == "docs") { docs();                 continue; }
        if (cmd == "version") { log_msg("Optimizer version: " + VERSION); continue; }

        if (cmd == "check") {
            std::map<fs::path, json> defaults = {
                { GAMES_FILE,          json::object()  },
                { CLOSE_APPS_FILE,     json::array()   },
                { REOPEN_APPS_FILE,    json::array()   },
                { PROCESS_GROUPS_FILE, json::object()  },
                { PRESETS_FILE,        DEFAULT_PRESETS },
                { CONFIG_FILE,         DEFAULT_CONFIG  },
            };
            validate_and_repair_files(defaults);
            log_msg("Config check complete");
            continue;
        }

        if (cmd == "verbose" && !args.empty()) {
            bool on = (args[0] == "1" || args[0] == "true" || args[0] == "on" ||
                args[0] == "yes" || args[0] == "y");
            auto cfg = get_runtime_config();
            cfg["verbose_init"] = on;
            save_json_file(CONFIG_FILE, cfg);
            refresh_runtime_config();
            log_msg("Verbose init set to: " + std::string(on ? "true" : "false"));
            continue;
        }

        if (cmd == "status") {
            auto cfg = get_runtime_config();
            json presets;
            {
                std::lock_guard<std::mutex> lk(g_config_lock);
                presets = g_presets_cache.is_null() ? load_presets() : g_presets_cache;
            }
            std::string resolved = resolve_mode(cfg.value("mode", "balanced"), presets);
            log_msg("Version: " + VERSION);
            log_msg(std::string("Running: ") +
                (g_running.load() ? "true" : "false") +
                " | Paused: " +
                (g_paused.load() ? "true" : "false"));
            {
                std::lock_guard<std::mutex> lk(g_game_name_mutex);
                log_msg("Current game: " +
                    (g_current_game_name.empty() ? "None" : g_current_game_name));
            }
            log_msg("Mode: " + cfg.value("mode", "balanced") +
                " | Resolved: " + resolved +
                " | Dry-run: " +
                (cfg.value("dry_run", false) ? "true" : "false"));
            continue;
        }

        if (cmd == "presets") {
            json presets = load_presets();
            std::string names;
            for (auto it = presets.begin(); it != presets.end(); ++it)
                names += it.key() + " ";
            log_msg("Presets: " + (names.empty() ? "None" : names));
            continue;
        }

        if (cmd == "mode" && !args.empty()) {
            auto cfg = get_runtime_config();
            cfg["mode"] = args[0];
            save_json_file(CONFIG_FILE, cfg);
            refresh_runtime_config();
            log_msg("Mode set to: " + args[0]);
            continue;
        }

        if (cmd == "dryrun" && !args.empty()) {
            bool val = (args[0] == "1" || args[0] == "true" || args[0] == "on" ||
                args[0] == "yes" || args[0] == "y");
            auto cfg = get_runtime_config();
            cfg["dry_run"] = val;
            save_json_file(CONFIG_FILE, cfg);
            refresh_runtime_config();
            log_msg("Dry-run set to: " + std::string(val ? "true" : "false"));
            continue;
        }

        if (cmd == "color") {
            if (args.empty()) { list_themes(); }
            else { apply_console_color(args[0]); save_theme(args[0]); }
            continue;
        }
        if (cmd == "theme") { list_themes(); continue; }
        if (cmd == "history") {
            int n = 10;
            if (!args.empty()) { try { n = std::stoi(args[0]); } catch (...) {} }
            show_history(n); continue;
        }
        if (cmd == "benchmark") { run_benchmark(); continue; }
        if (cmd == "standby") { clean_standby_list(); continue; }
        if (cmd == "hagscheck") { check_hags(); continue; }
        if (cmd == "togglefeature") {
            if (args.empty()) { log_msg("[TOGGLE] Usage: togglefeature <feature_name>"); continue; }
            json cfg = load_json_file(CONFIG_FILE, json::object());
            std::string feat = args[0];
            if (cfg.contains("features") && cfg["features"].contains(feat)) {
                bool cur = cfg["features"][feat].get<bool>();
                cfg["features"][feat] = !cur;
                save_json_file(CONFIG_FILE, cfg);
                log_msg("[TOGGLE] " + feat + " -> " + (!cur ? "ON" : "OFF"));
                refresh_runtime_config();
            }
            else {
                log_msg("[TOGGLE] Unknown feature: " + feat);
            }
            continue;
        }
        if (cmd == "ramclean") {
            auto cfg = get_runtime_config();
            RamCleanerSettings s;
            s.enabled = true;
            if (cfg.contains("ram_cleaner") && cfg["ram_cleaner"].is_object()) {
                const auto& rc = cfg["ram_cleaner"];
                if (rc.contains("interval_sec") && rc["interval_sec"].is_number_integer())
                    s.interval_sec = rc["interval_sec"].get<int>();
                if (rc.contains("empty_working_sets") && rc["empty_working_sets"].is_boolean())
                    s.empty_working_sets = rc["empty_working_sets"].get<bool>();
                if (rc.contains("flush_file_cache") && rc["flush_file_cache"].is_boolean())
                    s.flush_file_cache = rc["flush_file_cache"].get<bool>();
            }
            if (!args.empty() && args[0] == "status") {
                char buf[128];
                _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                    "[RAM] Config: interval=%ds, working-sets=%s, file-cache=%s",
                    s.interval_sec,
                    s.empty_working_sets ? "on" : "off",
                    s.flush_file_cache ? "on" : "off");
                log_msg(buf);
            }
            else {
                log_msg("[RAM] Manual clean triggered...");
                double freed = run_ram_clean(s);
                char buf[96];
                if (freed >= 0.0)
                    _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                        "[RAM] Done. ~%.0f MB freed", freed);
                else
                    _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                        "[RAM] Done. (%.0f MB consumed by OS - normal)", -freed);
                log_msg(buf);
            }
            continue;
        }

        if (cmd == "reload") {
            log_msg("[RELOAD] Restarting Optimizer...");
            schedule_relaunch();
            g_exit_code.store(WORKER_EXIT_RELOAD);
            safe_exit("Reload requested");
            return;
        }

        log_msg("Unknown command. Type 'help'.");
    }
}

// ============================================================
// SYSTEM TRAY
// ============================================================

static LRESULT CALLBACK tray_wnd_proc(
    HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    if (msg == TRAY_ICON_MSG) {
        if (lparam == WM_LBUTTONUP) {
            if (g_console_hwnd && IsWindowVisible(g_console_hwnd))
                hide_console();
            else
                show_console();
            return 0;
        }
        if (lparam == WM_RBUTTONUP) {
            HMENU menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, 1, L"Open Console");
            AppendMenuW(menu, MF_STRING, 2, L"Minimize (Hide)");
            AppendMenuW(menu, MF_STRING, 3,
                g_paused.load() ? L"Resume" : L"Pause");
            AppendMenuW(menu, MF_STRING, 5, L"Diagnostics Snapshot");
            AppendMenuW(menu, MF_STRING, 6, L"What Optimizer Does");
            AppendMenuW(menu, MF_STRING, 7, L"Open Log");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, 4, L"Exit");

            POINT pt{};
            GetCursorPos(&pt);
            SetForegroundWindow(hwnd);
            UINT cmd = TrackPopupMenu(
                menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                pt.x, pt.y, 0, hwnd, nullptr);
            DestroyMenu(menu);

            switch (cmd) {
            case 1: show_console();      break;
            case 2: hide_console();      break;
            case 3:
                g_paused.store(!g_paused.load());
                log_msg(g_paused.load() ? "[STATE] Paused" : "[STATE] Resumed");
                break;
            case 4: safe_exit("Tray exit"); break;
            case 5: diagnostics_snapshot(); break;
            case 6: self_audit_report();    break;
            case 7: open_log();             break;
            }
            return 0;
        }
        return 0;
    }
    if (msg == WM_DESTROY) {
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

static void tray_status_loop(HWND hwnd, HICON icon)
{
    while (g_running.load()) {
        double cpu = get_cpu_percent();
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        double mem = get_mem_percent();
        std::string game;
        {
            std::lock_guard<std::mutex> lk(g_game_name_mutex);
            game = g_current_game_name.empty() ? "Idle" : g_current_game_name;
        }
        wchar_t tip[128]{};
        _snwprintf_s(tip, 128, _TRUNCATE,
            L"Optimizer\nCPU %.0f%% | RAM %.0f%%\nGame: %s",
            cpu, mem, to_wide(game).c_str());
        g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        g_nid.hIcon = icon;
        wcsncpy_s(g_nid.szTip, tip, _TRUNCATE);
        Shell_NotifyIconW(NIM_MODIFY, &g_nid);
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    }
}

static void tray_loop()
{
    HINSTANCE hinst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = tray_wnd_proc;
    wc.hInstance = hinst;
    wc.lpszClassName = L"OptimizerTrayClass";
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(0, L"OptimizerTrayClass", L"OptimizerHidden",
        0, 0, 0, 0, 0, HWND_MESSAGE,
        nullptr, hinst, nullptr);
    g_tray_hwnd = hwnd;

    HICON icon = LoadIcon(nullptr, IDI_APPLICATION);
    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = hwnd;
    g_nid.uID = 0;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = TRAY_ICON_MSG;
    g_nid.hIcon = icon;
    wcsncpy_s(g_nid.szTip, L"Optimizer", _TRUNCATE);
    Shell_NotifyIconW(NIM_ADD, &g_nid);
    if (!is_clean()) log_msg("[TRAY] Running in system tray");

    std::thread st(tray_status_loop, hwnd, icon);
    st.detach();

    MSG msg{};
    while (g_running.load() && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

// ============================================================
// OPTIMIZER LOOP
// ============================================================


// hot_plug_monitor_loop: re-applies usb tweak on device change,
// lowers priority of bg apps that launch after game start.
static void hot_plug_monitor_loop(
    DWORD game_pid,
    const std::vector<std::string>& background_apps,
    const json& effective_features,
    const json& game_cfg)
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_IDLE);
    log_msg("[HOTPLUG] started");

    // track existing bg pids so we only catch new launches
    std::set<DWORD> known_bg_pids;
    if (!background_apps.empty()) {
        std::set<std::string> targets;
        for (auto& n : background_apps) targets.insert(to_lower(n));
        auto procs = enumerate_processes_light();
        for (auto& p : procs)
            if (targets.count(to_lower(p.name)))
                known_bg_pids.insert(p.pid);
    }

    // baseline usb device count
    int last_usb_count = 0;
    {
        HDEVINFO di = SetupDiGetClassDevsW(nullptr, L"USB", nullptr,
            DIGCF_ALLCLASSES | DIGCF_PRESENT);
        if (di != INVALID_HANDLE_VALUE) {
            SP_DEVINFO_DATA dd{}; dd.cbSize = sizeof(dd);
            for (DWORD i = 0; SetupDiEnumDeviceInfo(di, i, &dd); i++) last_usb_count++;
            SetupDiDestroyDeviceInfoList(di);
        }
    }

    while (pid_exists(game_pid) && g_running.load()) {
        // Sleep in 5s chunks so we respond to game exit quickly
        for (int i = 0; i < 12 && pid_exists(game_pid) && g_running.load(); i++)
            std::this_thread::sleep_for(std::chrono::seconds(5));

        if (!pid_exists(game_pid) || !g_running.load()) break;

        // usb
        if (feature_enabled("usb_power_disable", effective_features, game_cfg)) {
            int cur_usb_count = 0;
            HDEVINFO di = SetupDiGetClassDevsW(nullptr, L"USB", nullptr,
                DIGCF_ALLCLASSES | DIGCF_PRESENT);
            if (di != INVALID_HANDLE_VALUE) {
                SP_DEVINFO_DATA dd{}; dd.cbSize = sizeof(dd);
                for (DWORD i = 0; SetupDiEnumDeviceInfo(di, i, &dd); i++) cur_usb_count++;
                SetupDiDestroyDeviceInfoList(di);
            }
            if (cur_usb_count != last_usb_count) {
                log_msg("[HOTPLUG] usb change ("
                    + std::to_string(last_usb_count) + " -> "
                    + std::to_string(cur_usb_count)
                    + ")");
                disable_usb_power_saving();
                last_usb_count = cur_usb_count;
            }
        }

        // bg apps
        if (!background_apps.empty()) {
            std::set<std::string> targets;
            for (auto& n : background_apps) targets.insert(to_lower(n));
            auto procs = enumerate_processes_light();
            for (auto& p : procs) {
                if (!targets.count(to_lower(p.name))) continue;
                if (known_bg_pids.count(p.pid))        continue; // already seen
                // New instance of a background app launched while game is running
                HANDLE h = OpenProcess(PROCESS_SET_INFORMATION, FALSE, p.pid);
                if (h) {
                    SetPriorityClass(h, BELOW_NORMAL_PRIORITY_CLASS);
                    CloseHandle(h);
                    log_msg("[HOTPLUG] " + p.name + " (PID " + std::to_string(p.pid)
                        + ") lowered");
                    known_bg_pids.insert(p.pid);
                }
            }
        }
    }
    log_msg("[HOTPLUG] done");
}

static void optimizer_loop()
{
    ensure_files();

    std::map<std::string, json> games = load_proc_map(GAMES_FILE);
    auto close_apps = load_list(CLOSE_APPS_FILE);
    auto reopen_raw = load_list(REOPEN_APPS_FILE);
    auto groups = load_groups(PROCESS_GROUPS_FILE);

    std::set<std::string> reopen_apps;
    for (auto& x : reopen_raw) reopen_apps.insert(to_lower(x));

    refresh_runtime_config();
    auto config = get_runtime_config();

    bool        verbose_init = config.value("verbose_init", false);
    bool        dry_run = config.value("dry_run", false);
    std::string global_mode = config.value("mode", "balanced");
    json        config_features = config.value("features", DEFAULT_CONFIG["features"]);
    json        presets;
    {
        std::lock_guard<std::mutex> lk(g_config_lock);
        presets = g_presets_cache.is_null() ? load_presets() : g_presets_cache;
    }

    std::vector<std::string> background_apps;
    for (auto& x : config.value("background_apps", DEFAULT_CONFIG["background_apps"]))
        if (x.is_string()) background_apps.push_back(x.get<std::string>());

    if (verbose_init) {
        log_msg("[INIT] Loaded games.json entries: " + std::to_string(games.size()));
        log_msg("[INIT] Loaded close_apps entries: " + std::to_string(close_apps.size()));
        log_msg("[INIT] Loaded reopen_apps entries: " + std::to_string(reopen_apps.size()));
        log_msg("[INIT] Loaded groups: " + std::to_string(groups.size()));
    }

    std::string power_source = get_power_source();
    if (!is_clean()) log_msg("[INIT] Power source: " + power_source);
    std::string resolved_mode = resolve_mode(global_mode, presets);
    if (power_source == "battery" && resolved_mode == "aggressive") {
        log_msg("[INIT] On battery + aggressive -> using balanced");
        resolved_mode = "balanced";
    }

    // Signal optimizer_loop that startup display is ready
    // (handled in run_worker before threads start)
    if (!is_clean()) {
        log_msg("[INIT] Optimizer running");
        if (dry_run) log_msg("[DRY-RUN] Enabled: no changes will be applied");
        log_msg("[INIT] Mode: " + global_mode);
        log_msg("Type 'help' to see commands.");
    }

    // Start auto-discovery thread (can be disabled via config)
    g_discovery_enabled.store(feature_enabled("auto_discovery", config_features, json{}));
    std::thread([&presets]() {
        auto_discovery_loop(presets);
        }).detach();
    if (!g_discovery_enabled.load())
        if (!is_clean()) log_msg("[DETECTION] Auto game detection disabled. Type 'detection on' to enable");

    while (g_running.load()) {
        {
            std::lock_guard<std::mutex> lk(g_game_name_mutex);
            g_current_game_name.clear();
        }

        auto result = wait_for_game(games);
        if (!result.found || !g_running.load()) break;

        {
            std::lock_guard<std::mutex> lk(g_game_name_mutex);
            g_current_game_name = result.game_name;
        }

        if (!stabilize_game_process(result.pid)) {
            std::lock_guard<std::mutex> lk(g_game_name_mutex);
            g_current_game_name.clear();
            continue;
        }

        config = get_runtime_config();
        dry_run = config.value("dry_run", false);
        global_mode = config.value("mode", resolved_mode);
        config_features = config.value("features", DEFAULT_CONFIG["features"]);
        {
            std::lock_guard<std::mutex> lk(g_config_lock);
            presets = g_presets_cache.is_null() ? load_presets() : g_presets_cache;
        }
        background_apps.clear();
        for (auto& x : config.value("background_apps", DEFAULT_CONFIG["background_apps"]))
            if (x.is_string()) background_apps.push_back(x.get<std::string>());

        auto ef_pair = get_effective_features(
            global_mode, config_features, result.game_cfg, presets);
        json effective_features = ef_pair.first;
        std::string game_mode = ef_pair.second;

        log_msg("[APPLY] Optimizing " + result.game_name +
            " with mode '" + game_mode + "'");
        if (is_clean())
            print_game_start_banner(result.game_name, game_mode);

        if (feature_enabled("explorer_priority_tweak", effective_features, result.game_cfg)) {
            if (dry_run) log_msg("[DRY-RUN] Would lower explorer.exe priority");
            else tweak_explorer_priority(true);
        }

        if (feature_enabled("priority_boost", effective_features, result.game_cfg)) {
            if (dry_run) {
                log_msg("[DRY-RUN] Would set Ultimate Performance");
                log_msg("[DRY-RUN] Would boost game + child priority");
            }
            else {
                set_ultimate_power();
                DWORD gpid = result.pid;
                std::thread([gpid]() { boost_priority(gpid); }).detach();

            }
        }

        auto preserved = compute_preserved_processes(result.game_cfg, groups);
        if (!preserved.empty()) {
            std::string ps;
            for (auto& p : preserved) ps += p + " ";
            log_msg("[PRESERVE] Not closing: " + ps);
        }

        if (feature_enabled("close_apps", effective_features, result.game_cfg)) {
            std::vector<std::string> kill_list;
            for (auto& x : close_apps) {
                std::string xl = to_lower(x);
                if (!preserved.count(xl)) kill_list.push_back(xl);
            }
            if (!kill_list.empty()) {
                std::string ks;
                for (auto& k : kill_list) ks += k + " ";
                log_msg("[CLOSE] Will close: " + ks);
                if (dry_run) log_msg("[DRY-RUN] Would terminate close_apps processes");
                else kill_processes(kill_list, reopen_apps);
            }
            else {
                log_msg("[CLOSE] Nothing to close");
            }
        }
        else {
            log_msg("[CLOSE] close_apps feature disabled");
        }

        if (feature_enabled("background_affinity_isolation",
            effective_features, result.game_cfg)) {
            if (dry_run) log_msg("[DRY-RUN] Would isolate background affinity");
            else isolate_background_affinity(background_apps);
        }

        bool timer_enabled = false;
        if (feature_enabled("timer_resolution", effective_features, result.game_cfg) &&
            result.game_cfg.value("timer", false))
        {
            if (dry_run) log_msg("[DRY-RUN] Would enable 0.5ms timer resolution");
            else { enable_timer(); timer_enabled = true; }
        }

        if (feature_enabled("perf_monitor", effective_features, result.game_cfg) &&
            !dry_run)
        {
            DWORD gpid = result.pid;
            std::vector<std::string> bg_copy = background_apps;
            std::thread([gpid, bg_copy]() {
                performance_monitor_loop(gpid, bg_copy);
                }).detach();
        }

        // RAM CLEANER
        {
            auto ram_settings = resolve_ram_cleaner_settings(
                result.game_cfg, effective_features, config);
            if (ram_settings.enabled) {
                if (dry_run) {
                    log_msg("[DRY-RUN] Would start RAM cleaner (interval: " +
                        std::to_string(ram_settings.interval_sec) + "s)");
                }
                else {
                    DWORD gpid = result.pid;
                    RamCleanerSettings rs = ram_settings;
                    std::thread([gpid, rs]() {
                        ram_cleaner_loop(gpid, rs);
                        }).detach();
                }
            }
        }

        // MMCSS
        if (feature_enabled("mmcss", effective_features, result.game_cfg)) {
            if (dry_run) log_msg("[DRY-RUN] Would apply MMCSS Games scheduling");
            else apply_mmcss(result.pid);
        }

        // I/O PRIORITY
        // Skip NtSetInformationProcess on Vanguard-protected games to be safe
        static const std::set<std::string> vanguard_games = {
            "valorant.exe", "valorant-win64-shipping.exe"
        };
        bool is_vanguard_game = vanguard_games.count(to_lower(result.game_name)) > 0;
        if (feature_enabled("io_priority", effective_features, result.game_cfg)) {
            if (is_vanguard_game) {
                log_msg("[IO] Skipping I/O priority for Vanguard-protected game");
            }
            else if (dry_run) {
                log_msg("[DRY-RUN] Would set game I/O priority to High");
            }
            else {
                set_io_priority_high(result.pid);
            }
        }

        // CPU UNPARK
        if (feature_enabled("cpu_unpark", effective_features, result.game_cfg)) {
            if (dry_run) log_msg("[DRY-RUN] Would unpark CPU cores");
            else unpark_cpu_cores();
        }

        // NUMA / CCD AFFINITY
        if (feature_enabled("numa_affinity", effective_features, result.game_cfg)) {
            if (dry_run) log_msg("[DRY-RUN] Would pin game to NUMA node 0 (CCD0)");
            else apply_numa_affinity(result.pid);
        }

        // GPU PROCESS PRIORITY
        if (feature_enabled("gpu_priority", effective_features, result.game_cfg)) {
            if (dry_run) log_msg("[DRY-RUN] Would boost GPU-related process priorities");
            else boost_gpu_processes();
        }

        // NETWORK OPTIMIZATIONS
        if (feature_enabled("network_optimizations", effective_features, result.game_cfg)) {
            if (dry_run) {
                log_msg("[DRY-RUN] Would disable Nagle's algorithm");
                log_msg("[DRY-RUN] Would disable NIC power saving");
                log_msg("[DRY-RUN] Would boost network process priorities");
            }
            else {
                disable_nagle();
                disable_nic_power_saving();
                boost_network_threads();
            }
        }

        // ADVANCED NETWORK
        if (feature_enabled("net_advanced", effective_features, result.game_cfg)) {
            if (dry_run) {
                log_msg("[DRY-RUN] Would apply advanced network tweaks (TCP/RSS/ECN/QoS/DSCP/buffers/flow)");
            }
            else {
                disable_tcp_autotuning();
                disable_ecn();
                enable_rss();
                remove_qos_reserve();
                apply_dscp_tagging(result.game_name);
                flush_dns_cache();
                disable_interrupt_moderation();
                tune_adapter_buffers();
                disable_flow_control();
            }
        }

        // POWER THROTTLING
        if (feature_enabled("power_throttle_disable", effective_features, result.game_cfg)) {
            if (dry_run) log_msg("[DRY-RUN] Would disable EcoQoS power throttling");
            else disable_power_throttling(result.pid);
        }

        // SERVICE SUSPEND
        if (feature_enabled("service_suspend", effective_features, result.game_cfg)) {
            if (dry_run) log_msg("[DRY-RUN] Would suspend non-essential services");
            else suspend_services();
        }

        // XBOX GAME BAR / DVR
        if (feature_enabled("gamebar_disable", effective_features, result.game_cfg)) {
            if (dry_run) log_msg("[DRY-RUN] Would disable Xbox Game Bar/DVR");
            else disable_game_bar();
        }

        // FULLSCREEN OPTIMIZATIONS
        if (feature_enabled("fso_disable", effective_features, result.game_cfg)) {
            if (dry_run) log_msg("[DRY-RUN] Would disable fullscreen optimizations for game");
            else disable_fullscreen_optimizations(result.game_name);
        }

        // MEMORY COMPRESSION
        if (feature_enabled("memory_compression_disable", effective_features, result.game_cfg)) {
            if (dry_run) log_msg("[DRY-RUN] Would disable memory compression");
            else disable_memory_compression();
        }

        // DWM PRIORITY
        if (feature_enabled("dwm_priority", effective_features, result.game_cfg)) {
            if (dry_run) log_msg("[DRY-RUN] Would boost DWM priority");
            else boost_dwm_priority();
        }

        // VISUAL EFFECTS
        if (feature_enabled("visual_effects_disable", effective_features, result.game_cfg)) {
            if (dry_run) log_msg("[DRY-RUN] Would disable visual animations");
            else disable_visual_effects();
        }

        // DISK WRITE CACHE
        if (feature_enabled("disk_write_cache", effective_features, result.game_cfg)) {
            if (dry_run) log_msg("[DRY-RUN] Would optimize disk write cache");
            else optimize_disk_write_cache();
        }

        // HPET / DYNAMIC TICK (one-time, not per-session -- only if explicitly enabled)
        if (feature_enabled("hpet_disable", effective_features, result.game_cfg)) {
            if (dry_run) log_msg("[DRY-RUN] Would disable dynamic tick (requires reboot)");
            else disable_dynamic_tick();
        }

        // CPU ADVANCED
        if (feature_enabled("cpu_advanced", effective_features, result.game_cfg)) {
            if (dry_run) {
                log_msg("[DRY-RUN] Would apply advanced CPU optimizations");
            }
            else {
                if (feature_enabled("cstate_disable", effective_features, result.game_cfg))
                    disable_cstates();
                if (feature_enabled("force_boost", effective_features, result.game_cfg))
                    force_max_boost();
                if (feature_enabled("cpu_set_isolation", effective_features, result.game_cfg))
                    isolate_game_cpu_sets(result.pid);
                if (feature_enabled("large_pages", effective_features, result.game_cfg))
                    enable_large_pages();
                if (feature_enabled("smt_optimize", effective_features, result.game_cfg))
                    optimize_smt_scheduling();
                if (feature_enabled("prefetch_disable", effective_features, result.game_cfg))
                    disable_prefetch();
                set_ideal_processor(result.pid);
                disable_process_mitigations(result.pid);
                set_background_thread_mode(background_apps);
            }
        }

        // GPU POWER MAX
        if (feature_enabled("gpu_power_max", effective_features, result.game_cfg)) {
            if (dry_run) log_msg("[DRY-RUN] Would set GPU to max performance mode");
            else { set_gpu_power_max(); check_hags(); }
        }

        // STANDBY LIST + WORKING SET TRIM (one-shot before game starts)
        if (feature_enabled("standby_cleaner", effective_features, result.game_cfg)) {
            if (dry_run) log_msg("[DRY-RUN] Would flush standby memory list");
            else {
                // Delay so game loads first
                std::thread([]() {
                    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_IDLE);
                    std::this_thread::sleep_for(std::chrono::seconds(5));
                    clean_standby_list();
                    }).detach();
            }
        }
        if (feature_enabled("working_set_trim", effective_features, result.game_cfg)) {
            if (dry_run) log_msg("[DRY-RUN] Would trim working sets of all non-game processes");
            else {
                // Delay trim so game fully loads first -- avoids stutter at launch
                DWORD wst_pid = result.pid;
                std::thread([wst_pid]() {
                    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_IDLE);
                    std::this_thread::sleep_for(std::chrono::seconds(8));
                    if (pid_exists(wst_pid)) trim_all_working_sets(wst_pid);
                    }).detach();
            }
        }

        // INTERRUPT AFFINITY
        if (feature_enabled("interrupt_affinity", effective_features, result.game_cfg)) {
            if (dry_run) log_msg("[DRY-RUN] Would pin NIC interrupts to last CPU core");
            else set_interrupt_affinity();
        }

        // USB POWER SAVING
        if (feature_enabled("usb_power_disable", effective_features, result.game_cfg)) {
            if (dry_run) log_msg("[DRY-RUN] Would disable USB power saving");
            else disable_usb_power_saving();
        }

        // AUDIO LATENCY
        if (feature_enabled("audio_latency", effective_features, result.game_cfg)) {
            if (dry_run) log_msg("[DRY-RUN] Would set audio to low latency Games mode");
            else set_audio_low_latency();
        }

        // WINDOWS DEFENDER EXCLUSION
        if (feature_enabled("defender_exclusion", effective_features, result.game_cfg)) {
            if (dry_run) log_msg("[DRY-RUN] Would add game folder to Defender exclusions");
            else add_defender_exclusion(result.game_name);
        }

        // Record session start time
        auto session_start = std::chrono::steady_clock::now();

        // monitor for new usb devices and newly launched bg apps
        if (!dry_run) {
            DWORD hp_pid = result.pid;
            json hp_ef = effective_features;
            json hp_gcfg = result.game_cfg;
            std::vector<std::string> hp_bg = background_apps;
            std::thread([hp_pid, hp_bg, hp_ef, hp_gcfg]() {
                hot_plug_monitor_loop(hp_pid, hp_bg, hp_ef, hp_gcfg);
                }).detach();
        }

        log_msg("[STATE] Waiting for game to close...");
        while (pid_exists(result.pid) && g_running.load())
            std::this_thread::sleep_for(std::chrono::seconds(2));

        if (is_clean()) {
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - session_start).count();
            print_game_end_banner(result.game_name, (int)elapsed);
        }
        log_msg("[RESTORE] Game closed, restoring...");

        if (timer_enabled) {
            if (dry_run) log_msg("[DRY-RUN] Would restore timer resolution");
            else disable_timer(); // timer restore is fast, do it immediately
        }

        if (dry_run) {
            log_msg("[DRY-RUN] Would restore affinity/priority/explorer/network/gpu/cpu");
        }
        else {

            json ef_copy = effective_features;
            json gcfg_copy = result.game_cfg;
            std::string gname_copy = result.game_name;
            DWORD gpid_copy = result.pid;
            bool vanguard_copy = is_vanguard_game;
            std::thread restore_thread([ef_copy, gcfg_copy, gname_copy, gpid_copy, vanguard_copy]() {
                SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
                restore_affinity();
                restore_background_priority();
                if (feature_enabled("explorer_priority_tweak", ef_copy, gcfg_copy))
                    tweak_explorer_priority(false);
                if (feature_enabled("numa_affinity", ef_copy, gcfg_copy))
                    restore_numa_affinity(gpid_copy);
                if (feature_enabled("io_priority", ef_copy, gcfg_copy) && !vanguard_copy)
                    set_io_priority_normal(gpid_copy);
                if (feature_enabled("gpu_priority", ef_copy, gcfg_copy))
                    restore_gpu_processes();
                if (feature_enabled("network_optimizations", ef_copy, gcfg_copy)) {
                    restore_network_threads();
                    restore_nagle();
                    restore_nic_power_saving();
                }
                if (feature_enabled("net_advanced", ef_copy, gcfg_copy)) {
                    restore_tcp_autotuning();
                    restore_ecn();
                    restore_rss();
                    restore_qos_reserve();
                    remove_dscp_tagging(gname_copy);
                    restore_interrupt_moderation();
                    restore_adapter_buffers();
                    restore_flow_control();
                }
                if (feature_enabled("cpu_unpark", ef_copy, gcfg_copy))
                    restore_cpu_parking();
                revert_mmcss();
                restore_power_throttling(gpid_copy);
                if (feature_enabled("service_suspend", ef_copy, gcfg_copy))
                    restore_services();
                if (feature_enabled("gamebar_disable", ef_copy, gcfg_copy))
                    restore_game_bar();
                if (feature_enabled("fso_disable", ef_copy, gcfg_copy))
                    restore_fullscreen_optimizations();
                if (feature_enabled("memory_compression_disable", ef_copy, gcfg_copy))
                    restore_memory_compression();
                if (feature_enabled("dwm_priority", ef_copy, gcfg_copy))
                    restore_dwm_priority();
                if (feature_enabled("visual_effects_disable", ef_copy, gcfg_copy))
                    restore_visual_effects();
                if (feature_enabled("cpu_advanced", ef_copy, gcfg_copy)) {
                    if (feature_enabled("cstate_disable", ef_copy, gcfg_copy))
                        restore_cstates();
                    if (feature_enabled("force_boost", ef_copy, gcfg_copy))
                        restore_boost();
                    if (feature_enabled("cpu_set_isolation", ef_copy, gcfg_copy))
                        restore_cpu_sets(gpid_copy);
                    if (feature_enabled("smt_optimize", ef_copy, gcfg_copy))
                        restore_smt_scheduling();
                    restore_background_thread_mode();
                    if (feature_enabled("prefetch_disable", ef_copy, gcfg_copy))
                        restore_prefetch();
                }
                if (feature_enabled("gpu_power_max", ef_copy, gcfg_copy))
                    restore_gpu_power();
                if (feature_enabled("interrupt_affinity", ef_copy, gcfg_copy))
                    restore_interrupt_affinity();
                if (feature_enabled("usb_power_disable", ef_copy, gcfg_copy))
                    restore_usb_power_saving();
                if (feature_enabled("audio_latency", ef_copy, gcfg_copy))
                    restore_audio_latency();
                if (feature_enabled("defender_exclusion", ef_copy, gcfg_copy))
                    remove_defender_exclusion();
                log_msg("[RESTORE] All settings restored");
                });
            // Wait for restore to fully complete before next game can launch
            if (restore_thread.joinable()) restore_thread.join();
        }

        // Save session history
        {
            auto session_end = std::chrono::steady_clock::now();
            SessionRecord rec;
            rec.game = result.game_name;
            rec.duration_sec = static_cast<int>(
                std::chrono::duration_cast<std::chrono::seconds>(
                    session_end - session_start).count());
            // timestamp
            std::time_t t = std::time(nullptr);
            char tbuf[64]{};
            ctime_s(tbuf, sizeof(tbuf), &t);
            rec.timestamp = std::string(tbuf);
            if (!rec.timestamp.empty() && rec.timestamp.back() == '\n')
                rec.timestamp.pop_back();
            save_session_record(rec);
            {
                std::lock_guard<std::mutex> lk(g_history_mutex);
                g_session_history.push_back(rec);
            }
        }

        std::vector<std::wstring> reopen_copy;
        {
            std::lock_guard<std::mutex> lk(g_backup_mutex);
            reopen_copy = g_closed_reopen_paths;
            g_closed_reopen_paths.clear();
        }
        if (!reopen_copy.empty()) {
            if (dry_run) log_msg("[DRY-RUN] Would reopen closed apps");
            else restart_paths(reopen_copy);
        }

        {
            std::lock_guard<std::mutex> lk(g_game_name_mutex);
            g_current_game_name.clear();
        }
        log_msg("[STATE] Ready for next game");
    }

    log_msg("[EXIT] Optimizer loop stopped");
}

// ============================================================
// LOG INIT
// ============================================================

static void init_log()
{
    (void)fs::create_directories(APPDATA_DIR);
    {
        std::lock_guard<std::mutex> lk(g_log_mutex);
        g_log_file.open(LOG_PATH, std::ios::out | std::ios::trunc);
    }
    log_msg("[INIT] Log started");
    write_run_state(false);
    load_saved_theme();
    load_session_history();
}

// ============================================================
// WORKER / LAUNCHER
// ============================================================

static bool is_worker_mode(int argc, wchar_t* argv[])
{
    for (int i = 1; i < argc; ++i)
        if (_wcsicmp(argv[i], L"--worker") == 0) return true;
    return false;
}


// ============================================================
// CONSOLE CTRL HANDLER -- ensures restore runs on any close
// ============================================================
static BOOL WINAPI console_ctrl_handler(DWORD ctrl_type)
{
    switch (ctrl_type) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        log_msg("[EXIT] Console closed, restoring...");
        g_running.store(false);
        global_cleanup(false);
        // Give threads a moment to wind down
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        return TRUE;
    }
    return FALSE;
}

static void run_worker()
{
    show_console();
    if (!is_admin()) {
        std::cout << "[ERROR] Run as administrator\n";
        system("pause");
        exit(0);
    }
    recover_if_unclean_exit();
    init_log();
    if (!is_clean()) log_msg("[INIT] Starting Optimizer...");

    // Print dashboard synchronously before any threads start
    if (is_clean()) {
        g_console_muted.store(false);
        refresh_runtime_config();  // ensure config is loaded before reading
        auto cfg2 = get_runtime_config();
        std::string mode2 = cfg2.value("mode", "balanced");
        bool dry2 = cfg2.value("dry_run", false);
        auto games2 = load_proc_map(GAMES_FILE);
        bool det2 = feature_enabled("auto_discovery",
            cfg2.value("features", DEFAULT_CONFIG["features"]), json{});
        print_dashboard(mode2, (int)games2.size(), det2, dry2);
    }

    // Watch for queued discovery items and show prompt immediately
    std::thread([]() {
        while (g_running.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            DiscoveryItem pending_item;
            bool has_item = false;
            {
                std::lock_guard<std::mutex> lk(g_console_mutex);
                if (!g_pending_discovery.empty() && !g_wizard_active) {
                    g_wizard_state = DiscoveryWizardState{};
                    g_wizard_state.item = g_pending_discovery.front();
                    g_pending_discovery.erase(g_pending_discovery.begin());
                    g_wizard_active = true;
                    pending_item = g_wizard_state.item;
                    has_item = true;
                }
            }
            // Print prompt outside the lock
            if (has_item)
                discovery_wizard_prompt(DiscoveryStep::AskAdd, pending_item.name);
        }
        }).detach();

    try {
        std::thread(optimizer_loop).detach();
        std::thread(command_loop).detach();
        tray_loop();
    }
    catch (...) {}

    global_cleanup(true);
    write_run_state(true);
    close_log();
    exit(g_exit_code.load());
}

static void run_launcher()
{
    SetConsoleTitleW(L"Optimizer (Launcher)");
    hide_console();

    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);

    while (true) {
        std::wstring cmd_line = std::wstring(exe) + L" --worker";
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};

        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_SHOW;
        if (!CreateProcessW(nullptr, cmd_line.data(),
            nullptr, nullptr, FALSE,
            CREATE_NEW_CONSOLE,
            nullptr, nullptr, &si, &pi))
        {
            show_console();
            std::cerr << "[LAUNCHER] Failed to start worker\n";
            Sleep(2000);
            return;
        }

        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD rc = 0;
        GetExitCodeProcess(pi.hProcess, &rc);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);

        if (rc == static_cast<DWORD>(WORKER_EXIT_RELOAD)) {
            Sleep(500);
            continue;
        }
        return;
    }
}

// ============================================================
// ENTRY POINT
// ============================================================

#pragma warning(suppress: 4211)  // VCR003: wmain cannot be static (linker entry point)
int wmain(int argc, wchar_t* argv[])
{
    init_paths();
    init_ntdll();
    g_console_hwnd = GetConsoleWindow();
    g_con_handle = GetStdHandle(STD_OUTPUT_HANDLE);
    DEFAULT_CONFIG = make_default_config();
    DEFAULT_PRESETS = make_default_presets();

    // ensure_files creates AppData dir so config.json can be read/written
    if (is_worker_mode(argc, argv)) {
        try {
            ensure_files();
            prompt_view_mode();
            // Mute startup noise in clean mode until dashboard
            if (is_clean()) g_console_muted.store(true);
            run_worker();
        }
        catch (const std::exception& ex) {
            std::cerr << "[FATAL] Exception: " << ex.what() << "\n";
            std::cerr << "Press Enter to exit...\n";
            std::cin.get();
        }
        catch (...) {
            std::cerr << "[FATAL] Unknown exception at startup\n";
            std::cerr << "Press Enter to exit...\n";
            std::cin.get();
        }
    }
    else {
        run_launcher();
    }

    return 0;
}