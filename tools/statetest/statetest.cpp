// Offline test of the D5 saved dirt (src/dirt_state): the file format, saving
// and loading, damaged files, and who is saved or starts clean when trailers
// are swapped, on company jobs and quick jobs.
// Usage: statetest [dir]   files go to <dir>/statetest_files (default: the
//                          system temp folder), removed and created again

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "dirt_state.h"
#include "file_writer.h"

namespace fs = std::filesystem;
using dirt_state::Kind;
using dirt_state::Layers;
using dirt_state::Parse;
using dirt_state::ParseReport;
using dirt_state::Store;

namespace {

int g_failures = 0;

void Check(bool ok, const char* what, const std::string& detail = std::string()) {
    printf("%s %-62s %s\n", ok ? "ok  " : "FAIL", what, detail.c_str());
    if (!ok) ++g_failures;
}

const char* ParseName(Parse p) {
    switch (p) {
        case Parse::kOk: return "ok";
        case Parse::kDamaged: return "damaged";
        case Parse::kUnreadable: return "unreadable";
        case Parse::kNewer: return "newer";
    }
    return "?";
}

std::string Report(const ParseReport& r) {
    std::string s = std::string(ParseName(r.result)) + ", " + std::to_string(r.entries) + " units, " +
                    std::to_string(r.skipped) + " skipped";
    if (!r.detail.empty()) s += " (" + r.detail + ")";
    return s;
}

std::string Num(float v) {
    char b[32];
    snprintf(b, sizeof(b), "%.4f", v);
    return b;
}

float Dust(const Store& s, const std::string& key) {
    const Layers* l = s.Find(key);
    if (!l) return -1.f;  // not in the store
    const auto it = l->find("dust");
    return it == l->end() ? 0.f : it->second;
}

std::string ReadFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

void WriteFile(const fs::path& p, const std::string& text) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << text;
}

// The plugin's side: one dust level per unit, synced on every rig change.
struct Game {
    explicit Game(Store* s) : store(s), tracker(s) {}
    Store* store;
    dirt_state::Tracker tracker;
    dirt_state::Rig rig;
    float dust[dirt_state::kUnits] = {};

    void Current(Layers cur[dirt_state::kUnits]) const {
        for (int u = 0; u < dirt_state::kUnits; ++u) cur[u]["dust"] = dust[u];
    }
    void Frame() {
        Layers cur[dirt_state::kUnits], start[dirt_state::kUnits];
        Current(cur);
        const unsigned changed = tracker.Sync(rig, cur, start);
        for (int u = 0; u < dirt_state::kUnits; ++u) {
            if (!(changed & (1u << u))) continue;
            const auto it = start[u].find("dust");
            dust[u] = it == start[u].end() ? 0.f : it->second;
        }
    }
    void Commit() {
        Layers cur[dirt_state::kUnits];
        Current(cur);
        tracker.Commit(cur);
    }
    void Truck(const char* id, const char* plate) { rig.truck_id = id, rig.truck_plate = plate; }
    void Trailer(const char* id, const char* plate) { rig.trailer_id = id, rig.trailer_plate = plate; }
};

const std::string kTruck = dirt_state::TruckKey("scania.s_2016", "AB 123 CD");
const std::string kTrailerA = dirt_state::TrailerKey("trailer.own_a", "XY 1");
const std::string kTrailerB = dirt_state::TrailerKey("trailer.own_b", "XY 2");

void FormatTests() {
    printf("-- format\n");
    {
        const std::string k = dirt_state::TruckKey("volvo.fh16_2012", "AB 1|2=3%");
        Check(k == "truck|volvo.fh16_2012|AB%201%7C2%3D3%25", "key: spaces and separators escaped", k);
        const std::string t = dirt_state::TrailerKey("trailer.x", "\xC3\x96Z 9");  // UTF-8 plate
        Check(t == "trailer|trailer.x|%C3%96Z%209", "key: UTF-8 plate escaped byte by byte", t);
        Check(dirt_state::TruckKey("", "AB").empty() && dirt_state::TrailerKey("", "").empty(),
              "key: no id = no unit");
    }
    {
        Store s;
        s.Put(kTruck, {{"dust", 0.41237f}});
        s.Put(kTrailerA, {{"dust", 0.3f}, {"mud", 0.05f}});
        s.Put(kTrailerB, {{"dust", 2.f}});
        const std::string text = s.Serialize();
        printf("%s", text.c_str());
        Store r;
        const ParseReport rep = r.Deserialize(text);
        Check(rep.result == Parse::kOk && r.Entries() == s.Entries() && !r.Dirty(), "round trip: same units, order",
              Report(rep));
        Check(Dust(r, kTruck) == 0.4124f, "levels rounded to 4 decimals", Num(Dust(r, kTruck)));
        Check(Dust(r, kTrailerB) == 1.f, "levels clamped to 1", Num(Dust(r, kTrailerB)));
        Check(r.Entries().front().first == kTrailerB, "most recently used first");
    }
    {
        Store s;
        s.Put(kTrailerA, {{"dust", 0.3f}, {"mud", 0.2f}});
        s.Put(kTrailerA, {{"dust", 0.1f}});
        const Layers* l = s.Find(kTrailerA);
        Check(l && l->at("dust") == 0.1f && l->at("mud") == 0.2f, "a layer not given is kept (older plugin, D4 layers)");
        s.Put(kTrailerA, {{"dust", 0.f}, {"mud", 0.f}});
        Check(!s.Find(kTrailerA) && s.Size() == 0, "all layers 0: the unit is not written (missing = clean)");
        s.Put(kTrailerA, {{"dust", 0.3f}, {"wet_mud", 0.4f}, {"wet_mud_front", 0.f}});
        Store r;
        r.Deserialize(s.Serialize());
        r.Put(kTrailerA, {{"wet_mud", 0.f}});  // the mud dried away after the load
        const std::string text = r.Serialize();
        Check(text.find("wet_mud") == std::string::npos && r.Find(kTrailerA)->at("wet_mud") == 0.f &&
                  Dust(r, kTrailerA) == 0.3f,
              "layers at 0 not written; a saved layer can go back to 0");
        s.Erase(kTrailerA);
        s.Put(kTrailerA, {{"Dust!", 0.5f}});
        Check(s.Size() == 0, "bad layer names ignored");
        s.Put("lorry|x|y", {{"dust", 0.5f}});
        Check(s.Size() == 0, "bad keys ignored");
    }
    {
        Store s;
        for (int i = 0; i < 300; ++i) s.Put(dirt_state::TrailerKey("t" + std::to_string(i), ""), {{"dust", 0.5f}});
        Check(s.Size() == dirt_state::kMaxEntries && s.Entries().front().first == "trailer|t299|" &&
                  !s.Find("trailer|t43|") && s.Find("trailer|t44|"),
              "at most 256 units: the least recently used are dropped", std::to_string(s.Size()));
    }
}

void FileTests(const fs::path& dir) {
    printf("-- files\n");
    const fs::path file = dir / "dirt_state.txt";
    fs::path bad = file, tmp = file;
    bad += ".bad";
    tmp += ".tmp";
    {
        Store s;
        const ParseReport r = s.Load(file);
        Check(r.result == Parse::kOk && s.Size() == 0 && !s.Dirty(), "missing file: everything clean", Report(r));
    }
    {
        Store s;
        s.Put(kTruck, {{"dust", 0.25f}});
        s.Put(kTrailerA, {{"dust", 0.5f}});
        std::string err;
        const bool ok = s.Save(file, &err);
        Check(ok && fs::exists(file) && !fs::exists(tmp) && !s.Dirty(), "save: file written, no .tmp left", err);
        Store l;
        const ParseReport r = l.Load(file);
        Check(r.result == Parse::kOk && l.Entries() == s.Entries(), "load: the same units", Report(r));
        s.Put(kTruck, {{"dust", 0.75f}});
        const bool ok2 = s.Save(file, &err);
        Store l2;
        l2.Load(file);
        Check(ok2 && Dust(l2, kTruck) == 0.75f && l2.Size() == 2, "save over an existing file (rename replaces it)",
              Num(Dust(l2, kTruck)));
    }
    const std::string good = ReadFile(file);
    {
        WriteFile(file, good.substr(0, good.rfind("trailer|")));  // cut inside: last unit and end line lost
        Store s;
        const ParseReport r = s.Load(file);
        Check(r.result == Parse::kDamaged && s.Size() == 1 && Dust(s, kTruck) == 0.75f && s.Dirty(),
              "cut file: the good units are kept", Report(r));
        Check(fs::exists(bad) && ReadFile(bad) == ReadFile(file), "cut file: copied to .bad");
        std::string err;
        s.Save(file, &err);
        Store l;
        Check(l.Load(file).result == Parse::kOk && l.Size() == 1, "cut file: the next save writes a good file");
    }
    {
        std::string text = "# hand-edited\r\ntruckdirt_state 1\r\n";
        text += kTruck + " dust=0.5\r\n";
        text += kTrailerA + " dust=nan\r\n";                  // not a number
        text += kTrailerB + " dust=1.5\r\n";                  // above 1
        text += "lorry|x|y dust=0.5\r\n";                     // unknown unit type
        text += "trailer|t%2f|x dust=0.5\r\n";                // escape not as written (lower case, '/' needs none)
        text += "trailer|t1|x dust=0.5 dust=0.6\r\n";         // layer twice
        text += kTruck + " dust=0.1\r\n";                     // unit twice
        text += "trailer|t2|x\tdust=0.20 mud=1\r\n";          // tab, a layer this version does not use
        text += "end 8\r\n";
        WriteFile(file, text);
        Store s;
        const ParseReport r = s.Load(file);
        Check(r.result == Parse::kDamaged && r.skipped == 6 && s.Size() == 2 && Dust(s, kTruck) == 0.5f &&
                  Dust(s, "trailer|t2|x") == 0.2f && s.Find("trailer|t2|x")->at("mud") == 1.f,
              "bad lines skipped, CRLF and the rest kept", Report(r));
    }
    {
        WriteFile(file, std::string("\x89PNG\r\n\x1a\n\0\0\0\rIHDR", 16));
        Store s;
        const ParseReport r = s.Load(file);
        Check(r.result == Parse::kUnreadable && s.Size() == 0 && s.Dirty(), "garbage: unreadable, everything clean",
              Report(r));
        Check(ReadFile(bad).size() == 16, "garbage: copied to .bad");
        std::string err;
        Check(s.Save(file, &err) && Store().Load(file).result == Parse::kOk, "garbage: the next save writes a good file");
    }
    {
        WriteFile(file, "");
        Store s;
        const ParseReport r = s.Load(file);
        Check(r.result == Parse::kUnreadable && s.Size() == 0, "empty file: unreadable", Report(r));
    }
    {
        const std::string newer = "truckdirt_state 2\n" + kTruck + " dust=0.5 shine=0.3\nend 1\n";
        WriteFile(file, newer);
        Store s;
        const ParseReport r = s.Load(file);
        std::string err;
        s.Put(kTruck, {{"dust", 0.9f}});
        const bool saved = s.Save(file, &err);
        Check(r.result == Parse::kNewer && s.ReadOnly() && !saved && ReadFile(file) == newer,
              "newer format: not loaded, never overwritten", Report(r));
    }
    {
        WriteFile(file, good);
        Store s;
        s.Load(file);
        WriteFile(file, good + "end 2\n");  // text after the end line
        const ParseReport r = s.Load(file);
        Check(r.result == Parse::kDamaged && s.Size() == 2, "text after the end line: damaged, units kept", Report(r));
    }
}

void SessionTests(const fs::path& dir) {
    printf("-- sessions\n");
    const fs::path file = dir / "dirt_state.txt";
    fs::remove(file);
    std::string err;
    {
        // Session 1: own truck, then the own trailer A on a cargo_market job.
        Store store;
        store.Load(file);
        Game g(&store);
        g.Truck("scania.s_2016", "AB 123 CD");
        g.Frame();
        Check(g.tracker.KindOf(0) == Kind::kPlayer && g.dust[0] == 0.f, "new truck: player's, clean");
        g.dust[0] = 0.4f;
        g.Trailer("trailer.own_a", "XY 1");
        g.Frame();
        Check(g.tracker.KindOf(1) == Kind::kPlayer && g.dust[1] == 0.f, "own trailer, no job: player's, clean");
        g.dust[1] = 0.3f;
        g.rig.job_market = "cargo_market";
        g.Frame();
        Check(g.tracker.KindOf(1) == Kind::kPlayer && g.dust[1] == 0.3f, "cargo_market job: still the player's");
        // Trailer swap: detach A, hook own trailer B (new, clean), then A again.
        g.rig.job_market.clear();
        g.Trailer("", "");
        g.Frame();
        Check(g.tracker.KindOf(1) == Kind::kNone && g.dust[1] == 0.f, "trailer detached: no unit");
        g.Trailer("trailer.own_b", "XY 2");
        g.Frame();
        Check(g.dust[1] == 0.f, "swap to own trailer B: B starts clean", Num(g.dust[1]));
        g.dust[1] = 0.2f;
        g.Trailer("trailer.own_a", "XY 1");
        g.Frame();
        Check(g.dust[1] == 0.3f, "back to A: A has its dirt again", Num(g.dust[1]));
        g.Commit();
        Check(store.Save(file, &err) && Dust(store, kTruck) == 0.4f && Dust(store, kTrailerA) == 0.3f &&
                  Dust(store, kTrailerB) == 0.2f,
              "save: truck, A and B", err);
    }
    {
        // Session 2: the same rig loads its dirt; the wash cleans A.
        Store store;
        store.Load(file);
        Game g(&store);
        g.Truck("scania.s_2016", "AB 123 CD");
        g.Trailer("trailer.own_a", "XY 1");
        g.Frame();
        Check(g.dust[0] == 0.4f && g.dust[1] == 0.3f, "next session: truck and A as saved",
              Num(g.dust[0]) + " " + Num(g.dust[1]));
        g.dust[1] = 0.f;  // washed
        g.Commit();
        Check(store.Save(file, &err) && !store.Find(kTrailerA) && Dust(store, kTrailerB) == 0.2f,
              "washed A: removed from the file, B kept", err);
    }
    {
        // Session 3: company trailer C (freight_market) and a quick job.
        Store store;
        store.Load(file);
        Game g(&store);
        g.Truck("scania.s_2016", "AB 123 CD");
        g.Frame();
        g.rig.job_market = "freight_market";
        g.Trailer("trailer.company_c", "CO 1");
        g.Frame();
        Check(g.tracker.KindOf(1) == Kind::kCompany && g.dust[1] == 0.f, "company trailer: clean");
        g.dust[0] = 0.5f, g.dust[1] = 0.5f;
        g.Commit();
        const std::string c = dirt_state::TrailerKey("trailer.company_c", "CO 1");
        Check(!store.Find(c) && Dust(store, kTruck) == 0.5f, "company trailer: not saved, the truck is");
        g.Trailer("", "");
        g.Frame();
        g.Trailer("trailer.company_c", "CO 1");
        g.Frame();
        Check(g.dust[1] == 0.5f, "same company trailer again: keeps its dirt this session", Num(g.dust[1]));
        g.rig.job_market.clear();
        g.Trailer("", "");
        g.Frame();
        // Quick job: the company's truck and trailer, in one frame.
        g.rig.job_market = "quick_job";
        g.Truck("daf.xf", "QJ 1");
        g.Trailer("trailer.quick", "QJ 2");
        g.Frame();
        Check(g.tracker.KindOf(0) == Kind::kCompany && g.tracker.KindOf(1) == Kind::kCompany && g.dust[0] == 0.f &&
                  g.dust[1] == 0.f,
              "quick job: company truck and trailer, clean");
        g.dust[0] = 0.6f, g.dust[1] = 0.6f;
        g.Commit();
        g.rig.job_market.clear();
        g.Truck("scania.s_2016", "AB 123 CD");
        g.Trailer("", "");
        g.Frame();
        Check(g.dust[0] == 0.5f, "after the quick job: own truck with its dirt", Num(g.dust[0]));
        g.Commit();
        store.Save(file, &err);
        Check(store.Size() == 2 && !store.Find(dirt_state::TruckKey("daf.xf", "QJ 1")) &&
                  !store.Find(dirt_state::TrailerKey("trailer.quick", "QJ 2")),
              "quick job: nothing of it saved", std::to_string(store.Size()) + " units");
    }
    {
        // Config order: a company trailer seen one frame before its job, a quick
        // job seen one frame before its truck, and its end before the own truck.
        Store store;
        store.Load(file);
        Game g(&store);
        g.Truck("scania.s_2016", "AB 123 CD");
        g.Trailer("trailer.company_d", "CO 2");
        g.Frame();
        Check(g.tracker.KindOf(1) == Kind::kPlayer, "trailer before its job: taken as the player's for a frame");
        g.rig.job_market = "external_contracts";
        g.Frame();
        Check(g.tracker.KindOf(1) == Kind::kCompany && g.dust[1] == 0.f, "then the job: the company's, clean");
        g.dust[1] = 0.7f;
        g.Commit();
        g.rig.job_market.clear();
        g.Frame();  // the job ends a frame before the trailer goes
        g.Commit();
        g.Trailer("", "");
        g.Frame();
        Check(!store.Find(dirt_state::TrailerKey("trailer.company_d", "CO 2")),
              "company trailer seen as the player's for a frame: never saved");
        g.dust[0] = 0.55f;
        g.rig.job_market = "quick_job";
        g.Frame();  // own truck still there
        Check(Dust(store, kTruck) == 0.55f, "quick job before its truck: own truck's dirt saved first",
              Num(Dust(store, kTruck)));
        g.Truck("daf.xf", "QJ 1");
        g.Frame();
        g.dust[0] = 0.9f;
        g.rig.job_market.clear();
        g.Frame();  // quick truck still there
        g.Commit();
        g.Truck("scania.s_2016", "AB 123 CD");
        g.Frame();
        Check(g.dust[0] == 0.55f && !store.Find(dirt_state::TruckKey("daf.xf", "QJ 1")),
              "quick job ends before its truck goes: own truck back, quick truck not saved", Num(g.dust[0]));
    }
}

// D11n: the saves on a writer thread (plugin.cpp): the store's text taken and the store clean at once, the file written
// by WriteText; the writer runs its jobs in order and Stop runs the ones left.
void WriterTests(const fs::path& dir) {
    printf("-- writer thread (D11n)\n");
    const fs::path file = dir / "writer_state.txt";
    dirt_state::Store store;
    dirt_state::Layers l;
    l["dust"] = 0.25f;
    store.Put(dirt_state::TruckKey("volvo.fh", "W 1"), l);
    const std::string text = store.Serialize();
    store.MarkClean();
    std::string error;
    const bool wrote = dirt_state::Store::WriteText(file, text, &error);
    dirt_state::Store back;
    back.Load(file);
    const dirt_state::Layers* got = back.Find(dirt_state::TruckKey("volvo.fh", "W 1"));
    Check(wrote && !store.Dirty() && got && got->count("dust") && got->at("dust") == 0.25f,
          "WriteText writes what Save would; the store clean after MarkClean", error);
    store.MarkDirty();
    Check(store.Dirty(), "MarkDirty: a failed write is saved again later", "");
    std::vector<int> order;
    {
        file_writer::Writer w;
        w.Start();
        for (int i = 0; i < 1000; ++i) w.Post([&order, i] { order.push_back(i); });
        w.Stop();  // every job left runs before it returns
    }
    bool in_order = order.size() == 1000;
    for (size_t i = 0; in_order && i < order.size(); ++i) in_order = order[i] == int(i);
    Check(in_order, "the writer runs 1000 jobs in order, all before Stop returns", std::to_string(order.size()));
    file_writer::Writer idle;  // never started: a job runs at once
    bool ran = false;
    idle.Post([&ran] { ran = true; });
    Check(ran, "a writer not started runs a job at once", "");
}

}  // namespace

int main(int argc, char** argv) {
    const fs::path dir = (argc > 1 ? fs::path(argv[1]) : fs::temp_directory_path()) / "statetest_files";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    if (ec) {
        printf("FAIL cannot create the test folder: %s\n", ec.message().c_str());
        return 1;
    }
    FormatTests();
    FileTests(dir);
    SessionTests(dir);
    WriterTests(dir);
    printf("%s\n", g_failures ? "FAILED" : "all passed");
    return g_failures ? 1 : 0;
}
