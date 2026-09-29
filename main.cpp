// main.cpp
// Runs Task 1 (storage), Task 2 (B+ tree build) and Task 3 (range delete)
// and prints the statistics as tables.
//
// Usage:
//   ./dbms <games.txt> [block_size] [max_n]
//     block_size  bytes per block, default 4096
//     max_n       optional cap on n for testing with small nodes

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "bptree.h"
#include "storage.h"

static const char* DATA_FILE  = "data.disk";
static const char* INDEX_FILE = "index.disk";
static const float THRESHOLD  = 0.5f;
static const int   REPEATS    = 10;  // timed runs are repeated and averaged

using Clock = std::chrono::steady_clock;
using Rows  = std::vector<std::pair<std::string, std::string>>;

// ======================================================================
// Output helpers
// ======================================================================

static std::string num(size_t v) { return std::to_string(v); }

static std::string fixed(double v, int prec) {
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(prec) << v;
    return ss.str();
}

static double ms_since(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

static void print_table(const std::string& title, const Rows& rows) {
    size_t w1 = 0, w2 = 0;
    for (const auto& r : rows) {
        w1 = std::max(w1, r.first.size());
        w2 = std::max(w2, r.second.size());
    }
    std::string line(w1 + w2 + 7, '=');
    std::cout << "\n" << title << "\n" << line << "\n";
    for (const auto& r : rows)
        std::cout << "| " << std::left << std::setw(static_cast<int>(w1)) << r.first << " | "
                  << std::right << std::setw(static_cast<int>(w2)) << r.second << " |\n";
    std::cout << line << "\n";
}

static void print_root_keys(const Node& root) {
    std::cout << "Root node keys (" << root.keys.size() << " keys, "
              << (root.leaf ? "leaf" : "internal") << " node):\n";
    for (size_t i = 0; i < root.keys.size(); ++i) {
        std::cout << (i % 10 == 0 ? "  " : " ") << fixed(root.keys[i].value, 3);
        if (i % 10 == 9 || i + 1 == root.keys.size()) std::cout << "\n";
    }

    // Keys are ordered by (FG_PCT_home, record id), so the same value can
    // appear more than once. Show the full keys for those so it is clear.
    std::vector<size_t> dup;
    for (size_t i = 0; i < root.keys.size(); ++i) {
        bool same_prev = i > 0 && root.keys[i].value == root.keys[i - 1].value;
        bool same_next = i + 1 < root.keys.size() && root.keys[i].value == root.keys[i + 1].value;
        if (same_prev || same_next) dup.push_back(i);
    }
    if (!dup.empty()) {
        std::cout << "  Note: repeated values separate entries with the same FG_PCT_home;\n"
                  << "  the full keys (FG_PCT_home, record id) are:";
        for (size_t j = 0; j < dup.size(); ++j) {
            const Key& k = root.keys[dup[j]];
            std::cout << (j % 5 == 0 ? "\n    " : "  ") << "(" << fixed(k.value, 3) << ", "
                      << k.record_id << ")";
        }
        std::cout << "\n";
    }
}

static void print_tree_stats(const std::string& title, BPlusTree& tree) {
    TreeStats s = tree.stats();
    std::string err;
    bool ok = tree.validate(err);
    print_table(title, {
        {"Parameter n", num(tree.n())},
        {"Number of nodes", num(s.nodes)},
        {"Number of levels", num(s.levels)},
        {"Number of leaf nodes", num(s.leaves)},
        {"Number of index entries", num(s.entries)},
        {"Structure check", ok ? "passed" : "FAILED: " + err},
    });
    print_root_keys(tree.root());
    if (s.nodes <= 40) {
        std::cout << "\nTree (small enough to print):\n";
        tree.print(std::cout);
    }
}

// ======================================================================
// Task 3 building blocks
// ======================================================================

struct ScanResult {
    size_t blocks = 0;
    size_t count = 0;
    double average = 0;
};

// Brute force: read every data block and check every record.
static ScanResult linear_scan(DataFile& data, float threshold) {
    data.disk().reset_stats();
    ScanResult r;
    double sum = 0;
    data.scan([&](uint32_t, const Record& rec) {
        if (rec.fg_pct_home > threshold) {
            sum += rec.fg_pct_home;
            ++r.count;
        }
    });
    r.blocks = data.disk().distinct_blocks_accessed();
    r.average = r.count ? sum / static_cast<double>(r.count) : 0;
    return r;
}

struct IndexResult {
    std::vector<Key> keys;
    size_t index_nodes = 0;
    size_t data_blocks = 0;
    size_t count = 0;
    double average = 0;
};

// Uses the B+ tree to find matching entries, then reads each needed data
// block once to fetch the records.
static IndexResult index_retrieve(BPlusTree& tree, DataFile& data, float threshold) {
    tree.disk().reset_stats();
    data.disk().reset_stats();
    IndexResult r;
    r.keys = tree.range_greater_than(threshold);

    std::vector<uint32_t> rids;
    rids.reserve(r.keys.size());
    for (const Key& k : r.keys) rids.push_back(k.record_id);
    std::sort(rids.begin(), rids.end());

    double sum = 0;
    const size_t rpb = data.records_per_block();
    for (size_t i = 0; i < rids.size();) {
        uint32_t block = static_cast<uint32_t>(rids[i] / rpb);
        std::vector<char> blk = data.disk().read_block(block);
        for (; i < rids.size() && rids[i] / rpb == block; ++i) {
            Record rec = DataFile::record_at(blk, rids[i] % rpb);
            if (rec.valid && rec.fg_pct_home > threshold) {
                sum += rec.fg_pct_home;
                ++r.count;
            }
        }
    }
    r.index_nodes = tree.disk().distinct_blocks_accessed();
    r.data_blocks = data.disk().distinct_blocks_accessed();
    r.average = r.count ? sum / static_cast<double>(r.count) : 0;
    return r;
}

// ======================================================================
// Main
// ======================================================================

int main(int argc, char** argv) {
    if (argc < 2 || argc > 4) {
        std::cerr << "Usage: " << argv[0] << " <games.txt> [block_size] [max_n]\n";
        return 1;
    }
    const std::string input = argv[1];
    const size_t block_size = (argc >= 3) ? std::stoul(argv[2]) : 4096;
    const size_t max_n = (argc >= 4) ? std::stoul(argv[3]) : 0;

    try {
        // ==============================================================
        // TASK 1: storage
        // ==============================================================
        DataFile data(DATA_FILE, block_size, true);
        LoadStats load = data.load_from_text(input);

        std::cout << "######## TASK 1: STORAGE ########\n";
        print_table("Storage statistics", {
            {"Block size (bytes)", num(block_size)},
            {"Record size (bytes)", num(RECORD_SIZE)},
            {"Number of records stored", num(load.records)},
            {"Rows skipped (missing fields)", num(load.skipped)},
            {"Records per block", num(data.records_per_block())},
            {"Number of blocks", num(load.blocks)},
            {"Database file size (bytes)", num(load.blocks * block_size)},
        });

        // ==============================================================
        // TASK 2: build the B+ tree on FG_PCT_home
        // ==============================================================
        std::vector<Key> entries;
        entries.reserve(load.records);
        data.scan([&](uint32_t rid, const Record& rec) { entries.push_back({rec.fg_pct_home, rid}); });

        BPlusTree tree(INDEX_FILE, block_size, max_n);
        auto t_build = Clock::now();
        tree.bulk_load(entries);
        double build_ms = ms_since(t_build);

        std::cout << "\n######## TASK 2: B+ TREE ########\n";
        print_tree_stats("B+ tree statistics (bulk loaded in " + fixed(build_ms, 3) + " ms)", tree);

        // ==============================================================
        // TASK 3: delete games with FG_PCT_home > 0.5
        // ==============================================================
        std::cout << "\n######## TASK 3: DELETE FG_PCT_home > 0.5 ########\n";

        // Read only runs first, on the same data, repeated for stable timings.
        ScanResult scan;
        auto t_scan = Clock::now();
        for (int i = 0; i < REPEATS; ++i) scan = linear_scan(data, THRESHOLD);
        double scan_ms = ms_since(t_scan) / REPEATS;

        IndexResult idx;
        auto t_idx = Clock::now();
        for (int i = 0; i < REPEATS; ++i) idx = index_retrieve(tree, data, THRESHOLD);
        double idx_ms = ms_since(t_idx) / REPEATS;

        // The actual deletion (done once).
        tree.disk().reset_stats();
        data.disk().reset_stats();
        auto t_del = Clock::now();

        std::vector<Key> victims = tree.range_greater_than(THRESHOLD);
        size_t removed = 0;
        for (const Key& k : victims)
            if (tree.remove(k)) ++removed;

        std::vector<uint32_t> rids;
        for (const Key& k : victims) rids.push_back(k.record_id);
        std::sort(rids.begin(), rids.end());
        const size_t rpb = data.records_per_block();
        for (size_t i = 0; i < rids.size();) {
            uint32_t block = static_cast<uint32_t>(rids[i] / rpb);
            std::vector<char> blk = data.disk().read_block(block);
            for (; i < rids.size() && rids[i] / rpb == block; ++i)
                DataFile::mark_deleted(blk, rids[i] % rpb);
            data.disk().write_block(block, blk);
        }
        double del_ms = ms_since(t_del);

        print_table("Retrieval using the B+ tree (averaged over " + num(REPEATS) + " runs)", {
            {"Index nodes accessed", num(idx.index_nodes)},
            {"Data blocks accessed", num(idx.data_blocks)},
            {"Number of games deleted", num(removed)},
            {"Average FG_PCT_home of returned records", fixed(idx.average, 6)},
            {"Running time of retrieval (ms)", fixed(idx_ms, 4)},
        });

        print_table("Brute force linear scan (averaged over " + num(REPEATS) + " runs)", {
            {"Data blocks accessed", num(scan.blocks)},
            {"Matching records", num(scan.count)},
            {"Average FG_PCT_home", fixed(scan.average, 6)},
            {"Running time (ms)", fixed(scan_ms, 4)},
        });

        print_table("Full deletion process (search, tree updates, data blocks)", {
            {"Index node reads", num(tree.disk().reads())},
            {"Index node writes", num(tree.disk().writes())},
            {"Distinct index nodes accessed", num(tree.disk().distinct_blocks_accessed())},
            {"Data block reads", num(data.disk().reads())},
            {"Data block writes", num(data.disk().writes())},
            {"Distinct data blocks accessed", num(data.disk().distinct_blocks_accessed())},
            {"Running time of deletion (ms)", fixed(del_ms, 4)},
        });

        std::cout << "\n######## UPDATED B+ TREE ########\n";
        print_tree_stats("B+ tree statistics after deletion", tree);

        // Sanity checks.
        ScanResult after = linear_scan(data, THRESHOLD);
        size_t left_in_tree = tree.range_greater_than(THRESHOLD).size();
        TreeStats s = tree.stats();
        bool ok = removed == scan.count && idx.count == scan.count && after.count == 0 &&
                  left_in_tree == 0 && s.entries == load.records - removed;
        print_table("Consistency checks", {
            {"Deleted count matches linear scan", removed == scan.count ? "yes" : "NO"},
            {"Records above 0.5 left in data file", num(after.count)},
            {"Entries above 0.5 left in tree", num(left_in_tree)},
            {"Tree entries equal remaining records", s.entries == load.records - removed ? "yes" : "NO"},
        });
        return ok ? 0 : 2;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}
