/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "wahe_backend.h"
#include "prefs.h"
#include <sstream>
#include <stdexcept>
#include <cmath>

static std::string rom_path = "ROM", preferences_path;
static std::vector<std::string> disk_paths;
static int ram_megabytes = 64;
static constexpr char monitor_gamma_command[] = "SheepShaver monitor gamma ";
static constexpr char gamma_table_command[] = "SheepShaver gamma table ";
std::string wahe_nvram_path = "SheepShaver_nvram.dat";
double wahe_monitor_gamma = 1.8;
bool wahe_force_identity_gamma = false;

prefs_desc platform_prefs_items[] = {
    {"enableextfs", TYPE_BOOLEAN, false, "external filesystem unavailable in WAHE"},
    {"extdrives", TYPE_STRING, false, "external filesystem unavailable in WAHE"},
    {"ether", TYPE_STRING, false, "networking unavailable in WAHE"},
    {"idlewait", TYPE_BOOLEAN, false, "wait when the guest is idle"},
    {nullptr, TYPE_END, false, nullptr}
};

const char *wahe_configure(const std::string &line)
{
    // Apply configuration only while no CPU or native worker can observe partially changed settings
    if (wahe_running || wahe_stopping) return "Configure the machine before Init";
    int value = 0, width = 0, height = 0;
    unsigned long long epoch = 0;
    if (line.compare(0, 21, "SheepShaver ROM path ") == 0) rom_path = line.substr(21);
    else if (line.compare(0, 22, "SheepShaver disk path ") == 0) disk_paths.push_back(line.substr(22));
    else if (line.compare(0, 23, "SheepShaver NVRAM path ") == 0) wahe_nvram_path = line.substr(23);
    else if (line.compare(0, 29, "SheepShaver preferences path ") == 0) preferences_path = line.substr(29);
    else if (line.compare(0, sizeof(gamma_table_command) - 1, gamma_table_command) == 0) {
        // Choose whether the guest may install display correction tables for this machine
        std::istringstream input(line.substr(sizeof(gamma_table_command) - 1));
        std::string mode;
        if (!(input >> mode) || !(input >> std::ws).eof() || (mode != "guest" && mode != "identity"))
            return "Gamma table must be guest or identity";
        wahe_force_identity_gamma = mode == "identity";
    }
    else if (line.compare(0, sizeof(monitor_gamma_command) - 1, monitor_gamma_command) == 0) {
        // Define the virtual display response independently of the guest's correction table
        std::istringstream input(line.substr(sizeof(monitor_gamma_command) - 1));
        double gamma = 0;
        if (!(input >> gamma) || !std::isfinite(gamma) || gamma < 1.0 || gamma > 3.0 ||
            !(input >> std::ws).eof()) return "Monitor gamma must be a number from 1.0 to 3.0";
        wahe_monitor_gamma = gamma;
    }
    else if (sscanf(line.c_str(), "SheepShaver RAM %d", &value) == 1 && value >= 16 && value <= 512) ram_megabytes = value;
    else if (sscanf(line.c_str(), "SheepShaver resolution %dx%d", &width, &height) == 2 &&
        width >= 320 && height >= 200 && width <= 1920 && height <= 1200) { wahe_width = width; wahe_height = height; }
    else if (sscanf(line.c_str(), "SheepShaver sample rate %d", &value) == 1 && value >= 8000 && value <= 48000) wahe_sample_rate = value;
    else if (sscanf(line.c_str(), "SheepShaver clock unix %llu", &epoch) == 1 && epoch <= 0xffffffffULL) wahe_epoch = epoch;
    else return "Unknown or invalid SheepShaver configuration command";
    // Indicate that the complete configuration command was accepted
    return nullptr;
}

void AddPlatformPrefsDefaults()
{
    // Default to explicitly configured image files and callback-owned devices
    PrefsAddBool("idlewait", true);
    PrefsAddBool("enableextfs", false);
    PrefsAddBool("nonet", true);
    PrefsAddBool("nogui", true);
}

void LoadPrefs(const char *)
{
    // Read an optional preferences document through the WAHE file provider
    if (!preferences_path.empty()) {
        FILE *file = wahe_fopen(preferences_path.c_str(), "rb");
        if (!file) {
            // Record provider failures without requiring Wasm exception support
#ifdef __wasm32__
            wahe_fail("Cannot load preferences through the host");
            return;
#else
            throw std::runtime_error("Cannot load preferences through the host");
#endif
        }
        LoadPrefsFromStream(file);
        wahe_fclose(file);
    }

    // Apply explicit module settings and prevent legacy native-device backends from activating
    PrefsReplaceString("rom", rom_path.c_str());
    PrefsReplaceInt32("ramsize", ram_megabytes * 1024 * 1024);
    for (const auto &path : disk_paths) PrefsAddString("disk", path.c_str());
    PrefsReplaceBool("enableextfs", false);
    PrefsReplaceBool("nonet", true);
    PrefsReplaceBool("nogui", true);
    PrefsReplaceBool("jit", false);
    PrefsReplaceBool("jit68k", false);
    PrefsReplaceBool("gfxaccel", false);
    PrefsReplaceBool("ignoresegv", false);
    PrefsRemoveItem("ether");
}

void SavePrefs() { /* The host owns persistent configuration */ }
void prefs_init() { /* Initialization is callback-driven */ }
void prefs_exit() { /* The host owns configuration lifetime */ }
