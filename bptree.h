// bptree.h
// Indexing component (Task 2 and 3): a disk based B+ tree on FG_PCT_home.
//
// Every node is stored in exactly one block of its own file (index.disk).
// Block 0 holds metadata (root, levels, next free block); nodes start at 1.
//
// DUPLICATE KEYS
// FG_PCT_home has many repeated values. Each index entry is ordered by
// (fg_pct_home, record_id), which makes every entry unique. The standard
// search, split, borrow and merge rules then work without special cases.
//
// NODE LAYOUT (one block)
//   offset 0  uint8   is_leaf
//   offset 1  uint8   unused
//   offset 2  uint16  number of keys k
//   offset 4  uint32  next leaf block (leaves only, NIL otherwise)
//   offset 8  k keys, 8B each (float fg_pct_home + uint32 record_id)
//   then      k+1 child block numbers, 4B each (internal nodes only)
//
// In a leaf, the record_id inside each key is the pointer to the record.
// The parameter n is the largest k such that an internal node fits:
//   8 + 8n + 4(n + 1) <= block size

#ifndef BPTREE_H
#define BPTREE_H

#include <cstdint>
#include <ostream>
#include <string>
#include <vector>

#include "storage.h"

constexpr uint32_t NIL = 0xFFFFFFFFu;

struct Key {
    float    value;      // fg_pct_home
    uint32_t record_id;  // tie breaker, and the record pointer in leaves
};

inline bool operator<(const Key& a, const Key& b) {
    if (a.value != b.value) return a.value < b.value;
    return a.record_id < b.record_id;
}
inline bool operator==(const Key& a, const Key& b) {
    return a.value == b.value && a.record_id == b.record_id;
}
inline bool operator<=(const Key& a, const Key& b) { return !(b < a); }

struct Node {
    bool                  leaf = true;
    uint32_t              next = NIL;  // next leaf in the chain
    std::vector<Key>      keys;
    std::vector<uint32_t> children;    // internal nodes only, size = keys + 1
};

struct TreeStats {
    size_t nodes   = 0;
    size_t levels  = 0;
    size_t leaves  = 0;
    size_t entries = 0;
};

class BPlusTree {
public:
    // max_n (optional) caps n below what the block allows; useful for testing
    // with small nodes. 0 means use the full block.
    BPlusTree(const std::string& path, size_t block_size, size_t max_n = 0);

    static size_t n_for_block(size_t block_size);

    size_t n() const { return n_; }
    size_t min_leaf_keys() const { return min_leaf_; }
    size_t min_internal_keys() const { return min_internal_; }

    // Builds the tree bottom up from all entries (they are sorted first).
    void bulk_load(std::vector<Key> entries);

    // All entries with value strictly greater than v, in key order.
    std::vector<Key> range_greater_than(float v);

    // Deletes one entry. Returns false if it was not found.
    bool remove(const Key& k);

    TreeStats stats();
    Node      root();
    bool      validate(std::string& error);
    void      print(std::ostream& out);

    Disk& disk() { return disk_; }

private:
    struct RemoveResult {
        bool     found = false;
        bool     underflow = false;
        bool     min_changed = false;  // smallest key in this subtree changed
        Key      new_min{};
        bool     leaf = true;           // info about this node for root collapse
        size_t   key_count = 0;
        uint32_t first_child = NIL;
    };

    Node     read_node(uint32_t id);
    void     write_node(uint32_t id, const Node& node);
    uint32_t allocate_node();
    void     save_meta();

    RemoveResult remove_from(uint32_t id, const Key& k);
    void         fix_underflow(Node& parent, size_t child_index);

    bool check_subtree(uint32_t id, const Key* low, const Key* high, size_t depth,
                       size_t& leaf_depth, size_t& entries, std::string& error);

    Disk     disk_;
    size_t   n_;
    size_t   min_leaf_;
    size_t   min_internal_;
    uint32_t root_ = NIL;
    uint32_t next_free_ = 1;
    size_t   levels_ = 0;
};

#endif
