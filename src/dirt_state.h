// D5: the saved dirt of the player's truck and own trailer between sessions
// (Documents\<game>\TruckDirt\dirt_state.txt, outside the game's save).
//
// Pure bookkeeping and std::filesystem, no game, Windows or D3D calls
// (tools/statetest runs it offline):
// - Store: the saved units, one line each, keyed "truck|<id>|<plate>" or
//   "trailer|<id>|<plate>" (the SDK's truck/trailer config id and licence
//   plate; the own trailer's id is stable between sessions [verified: V1]).
//   Each unit has named layer levels in 0..1 ("dust" now; the D4 layers
//   later). Layers this version does not know are kept as they are. A layer
//   at 0 is not written, nor a unit whose layers are all 0: missing = clean. At most
//   kMaxEntries units, the least recently used are dropped;
// - Load/Save: text file with a format version and an end line with the
//   count (a cut file shows). Saving writes <file>.tmp and renames it over
//   the file, so a crash leaves the old or the new file, never half of one.
//   A damaged file is copied to <file>.bad and its good lines are kept; a
//   file of a newer format is left alone and never overwritten;
// - Tracker: who is saved and who starts clean, from job.market [verified:
//   V1]: the truck is the player's except on a quick_job (the company's truck
//   and trailer); the trailer is the player's own with no job or on a
//   cargo_market job, the company's on any other market. The player's units
//   start with their saved dirt; the company's start clean and live only in
//   this session (detaching and hooking the same one again keeps its dirt).
//   A unit whose kind changes without its key changing (a config that came a
//   frame before the other) keeps its dirt; a key seen as the company's in
//   this session is never added to the file.
#pragma once

#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace dirt_state {

constexpr int kFormatVersion = 1;
constexpr int kMaxEntries = 256;
constexpr int kUnits = 2;  // 0 = truck, 1 = trailer.0 (as dirt::kUnits)

// Layer name ("dust", lower case, digits and '_') -> level 0..1.
using Layers = std::map<std::string, float>;

// The keys of the two units ("" without an id: no unit).
std::string TruckKey(const std::string& id, const std::string& plate);
std::string TrailerKey(const std::string& id, const std::string& plate);

enum class Parse { kOk, kDamaged, kUnreadable, kNewer };

struct ParseReport {
    Parse result = Parse::kOk;
    int entries = 0;     // units loaded
    int skipped = 0;     // bad or duplicate lines
    std::string detail;  // first problem, for the log
};

class Store {
public:
    const Layers* Find(const std::string& key) const;
    // Sets the given layers of the unit (others it had are kept) and makes it
    // the most recently used. Levels are clamped to 0..1 and rounded to the
    // file's 4 decimals; all 0 removes the unit.
    void Put(const std::string& key, const Layers& layers);
    void Erase(const std::string& key);
    int Size() const { return static_cast<int>(entries_.size()); }
    const std::vector<std::pair<std::string, Layers>>& Entries() const { return entries_; }

    // Changed since the last load or save.
    bool Dirty() const { return dirty_; }
    // A file of a newer format was found: it must not be overwritten.
    bool ReadOnly() const { return read_only_; }

    std::string Serialize() const;
    // Replaces the content with the text's (nothing on kUnreadable/kNewer).
    ParseReport Deserialize(const std::string& text);

    // The file: a missing file is an empty store (kOk, 0 entries). A damaged
    // or unreadable file is copied to <file>.bad first.
    ParseReport Load(const std::filesystem::path& file);
    bool Save(const std::filesystem::path& file, std::string* error);
    // D11n: Save in two steps, for a writer thread: the text taken (and the store clean) here, the file written
    // there by WriteText (Save's own <file>.tmp and rename); MarkDirty again when that failed.
    void MarkClean() { dirty_ = false; }
    void MarkDirty() { dirty_ = true; }
    static bool WriteText(const std::filesystem::path& file, const std::string& text, std::string* error);

private:
    std::vector<std::pair<std::string, Layers>> entries_;  // most recently used first
    bool dirty_ = false;
    bool read_only_ = false;
};

// Telemetry configuration of the rig (strings as the SDK sends them).
struct Rig {
    std::string truck_id, truck_plate;
    std::string trailer_id, trailer_plate;  // empty id = no trailer
    std::string job_market;                 // empty = no job
};

enum class Kind { kNone, kPlayer, kCompany };

Kind TruckKind(const Rig& rig);
Kind TrailerKind(const Rig& rig);

class Tracker {
public:
    explicit Tracker(Store* store) : store_(store) {}
    // Follows a rig change. `current` holds the units' levels now (to keep
    // what the outgoing units had); returns a mask (bit 1 << unit) of the
    // units whose levels must be replaced by start[unit] (empty = clean).
    unsigned Sync(const Rig& rig, const Layers current[kUnits], Layers start[kUnits]);
    // Writes the player's units into the store (before Store::Save).
    void Commit(const Layers current[kUnits]);

    Kind KindOf(int unit) const { return kind_[unit]; }
    const std::string& Key(int unit) const { return key_[unit]; }

private:
    void Keep(int unit, const Layers& current);
    void Put(const std::string& key, const Layers& layers);

    Store* store_;
    std::string key_[kUnits];
    Kind kind_[kUnits] = {Kind::kNone, Kind::kNone};
    std::map<std::string, Layers> session_;  // the company's units seen this session
    std::set<std::string> company_;          // keys seen as the company's this session
};

}  // namespace dirt_state
