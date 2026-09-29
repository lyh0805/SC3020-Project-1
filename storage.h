// storage.h
// Storage component (Task 1).
//
// The "disk" is a single binary file split into fixed size blocks.
// All access goes through Disk::read_block / Disk::write_block, which
// also count block accesses for the Task 3 statistics.

#ifndef STORAGE_H
#define STORAGE_H

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <unordered_set>
#include <vector>

// RECORD LAYOUT (28 bytes, no padding)
//
//   game_date_est   uint32  4B  date stored as YYYYMMDD, e.g. 22/12/2022 becomes 20221222
//   team_id_home    uint32  4B  team ids are 10 digits but fit in 32 bits
//   pts_home        uint16  2B
//   fg_pct_home     float   4B  the B+ tree key
//   ft_pct_home     float   4B
//   fg3_pct_home    float   4B
//   ast_home        uint16  2B
//   reb_home        uint16  2B
//   home_team_wins  uint8   1B  0 or 1
//   valid           uint8   1B  1 = live, 0 = deleted (not from the dataset;
//                               lets us delete without moving records, so a
//                               record's address never changes)
//
// pragma pack(1) removes compiler padding, so fields can start at any byte.
#pragma pack(push, 1)
struct Record {
    uint32_t game_date_est;
    uint32_t team_id_home;
    uint16_t pts_home;
    float    fg_pct_home;
    float    ft_pct_home;
    float    fg3_pct_home;
    uint16_t ast_home;
    uint16_t reb_home;
    uint8_t  home_team_wins;
    uint8_t  valid;
};
#pragma pack(pop)

static_assert(sizeof(Record) == 28, "Record must be exactly 28 bytes");

constexpr size_t RECORD_SIZE       = sizeof(Record);
constexpr size_t BLOCK_HEADER_SIZE = sizeof(uint32_t);  // number of used slots
constexpr size_t VALID_OFFSET      = offsetof(Record, valid);

// Simulated disk: one binary file of fixed size blocks.
class Disk {
public:
    // truncate = true starts from an empty file (used when building).
    Disk(const std::string& path, size_t block_size, bool truncate);

    std::vector<char> read_block(uint32_t block_id);
    // counted = false is only used for bookkeeping writes (index metadata)
    // that should not show up in the access statistics.
    void write_block(uint32_t block_id, const std::vector<char>& data, bool counted = true);

    uint32_t num_blocks();
    size_t block_size() const { return block_size_; }

    // Access statistics.
    void   reset_stats();
    size_t reads() const { return reads_; }
    size_t writes() const { return writes_; }
    size_t distinct_blocks_accessed() const { return touched_.size(); }

private:
    std::string  path_;
    size_t       block_size_;
    std::fstream file_;
    size_t       reads_  = 0;
    size_t       writes_ = 0;
    std::unordered_set<uint32_t> touched_;
};

struct LoadStats {
    size_t records = 0;
    size_t skipped = 0;
    size_t blocks  = 0;
};

// The database file holding the NBA records.
//
// BLOCK LAYOUT
//   [ used slot count (4B) | record 0 | record 1 | ... | record k | unused bytes ]
//
// Records are unspanned and fixed length, so record id r is always at
// block r / records_per_block, slot r % records_per_block.
class DataFile {
public:
    DataFile(const std::string& path, size_t block_size, bool truncate);

    LoadStats load_from_text(const std::string& txt_path);

    size_t   block_size() const { return disk_.block_size(); }
    size_t   records_per_block() const { return rpb_; }
    uint32_t num_blocks() { return disk_.num_blocks(); }
    Disk&    disk() { return disk_; }

    void     locate(uint32_t record_id, uint32_t& block, size_t& slot) const;
    uint32_t record_id(uint32_t block, size_t slot) const;

    // Helpers for working on a block that has already been read.
    static uint32_t used_slots(const std::vector<char>& blk);
    static Record   record_at(const std::vector<char>& blk, size_t slot);
    static void     mark_deleted(std::vector<char>& blk, size_t slot);

    // Calls fn(record_id, record) for every live record, block by block.
    template <typename Fn>
    void scan(Fn&& fn) {
        uint32_t nb = num_blocks();
        for (uint32_t b = 0; b < nb; ++b) {
            std::vector<char> blk = disk_.read_block(b);
            uint32_t used = used_slots(blk);
            for (size_t s = 0; s < used; ++s) {
                Record r = record_at(blk, s);
                if (r.valid) fn(record_id(b, s), r);
            }
        }
    }

private:
    Disk   disk_;
    size_t rpb_;
};

#endif
