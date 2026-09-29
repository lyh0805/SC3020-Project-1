// bptree.cpp
// Disk based B+ tree. Every node read or write goes through Disk, so
// all accesses are counted.

#include "bptree.h"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <stdexcept>

// ======================================================================
// Setup and node I/O
// ======================================================================

size_t BPlusTree::n_for_block(size_t block_size) {
    // 8 byte header + 8n bytes of keys + 4(n + 1) bytes of child pointers
    if (block_size < 12) return 0;
    size_t n = (block_size - 12) / 12;
    return std::min<size_t>(n, 65535);  // key count is stored in 16 bits
}

BPlusTree::BPlusTree(const std::string& path, size_t block_size, size_t max_n)
    : disk_(path, block_size, true) {
    n_ = n_for_block(block_size);
    if (max_n > 0 && max_n < n_) n_ = max_n;
    if (n_ < 3) throw std::runtime_error("block size too small: n must be at least 3");
    min_leaf_ = (n_ + 1) / 2;  // leaf: at least floor((n+1)/2) keys
    min_internal_ = n_ / 2;    // internal: at least floor(n/2) keys
    save_meta();
}

void BPlusTree::save_meta() {
    // Block 0: magic, root, next free block, levels, n.
    // Written uncounted so it does not affect the access statistics.
    std::vector<char> buf(20, 0);
    uint32_t vals[5] = {0x31545042u, root_, next_free_, static_cast<uint32_t>(levels_),
                        static_cast<uint32_t>(n_)};
    std::memcpy(buf.data(), vals, sizeof(vals));
    disk_.write_block(0, buf, false);
}

uint32_t BPlusTree::allocate_node() { return next_free_++; }

Node BPlusTree::read_node(uint32_t id) {
    std::vector<char> b = disk_.read_block(id);
    Node node;
    node.leaf = b[0] != 0;
    uint16_t k;
    std::memcpy(&k, b.data() + 2, sizeof(k));
    std::memcpy(&node.next, b.data() + 4, sizeof(node.next));

    size_t off = 8;
    node.keys.resize(k);
    for (uint16_t i = 0; i < k; ++i) {
        std::memcpy(&node.keys[i].value, b.data() + off, 4);
        std::memcpy(&node.keys[i].record_id, b.data() + off + 4, 4);
        off += 8;
    }
    if (!node.leaf) {
        node.children.resize(static_cast<size_t>(k) + 1);
        std::memcpy(node.children.data(), b.data() + off, 4 * node.children.size());
    }
    return node;
}

void BPlusTree::write_node(uint32_t id, const Node& node) {
    if (node.keys.size() > n_) throw std::logic_error("node overflow on write");
    if (!node.leaf && node.children.size() != node.keys.size() + 1)
        throw std::logic_error("internal node has wrong number of children");

    std::vector<char> b(disk_.block_size(), 0);
    b[0] = node.leaf ? 1 : 0;
    uint16_t k = static_cast<uint16_t>(node.keys.size());
    std::memcpy(b.data() + 2, &k, sizeof(k));
    uint32_t next = node.leaf ? node.next : NIL;
    std::memcpy(b.data() + 4, &next, sizeof(next));

    size_t off = 8;
    for (const Key& key : node.keys) {
        std::memcpy(b.data() + off, &key.value, 4);
        std::memcpy(b.data() + off + 4, &key.record_id, 4);
        off += 8;
    }
    if (!node.leaf) std::memcpy(b.data() + off, node.children.data(), 4 * node.children.size());
    disk_.write_block(id, b);
}

// ======================================================================
// Bulk loading
// ======================================================================

// Splits `total` items into groups of at most `cap`. If the last group
// ends up below `min_size`, the last two groups are evened out.
static std::vector<size_t> group_sizes(size_t total, size_t cap, size_t min_size) {
    std::vector<size_t> sizes;
    for (size_t left = total; left > 0;) {
        size_t s = std::min(cap, left);
        sizes.push_back(s);
        left -= s;
    }
    if (sizes.size() >= 2 && sizes.back() < min_size) {
        size_t both = sizes[sizes.size() - 2] + sizes.back();
        sizes[sizes.size() - 2] = (both + 1) / 2;
        sizes.back() = both / 2;
    }
    return sizes;
}

void BPlusTree::bulk_load(std::vector<Key> entries) {
    std::sort(entries.begin(), entries.end());
    root_ = NIL;
    next_free_ = 1;
    levels_ = 0;
    if (entries.empty()) {
        save_meta();
        return;
    }

    struct Built {
        uint32_t id;
        Key      min;  // smallest key in this subtree
    };
    std::vector<Built> level;

    // Level 1: leaves, filled left to right and chained together.
    std::vector<size_t> sizes = group_sizes(entries.size(), n_, min_leaf_);
    std::vector<uint32_t> ids(sizes.size());
    for (auto& id : ids) id = allocate_node();

    size_t pos = 0;
    for (size_t i = 0; i < sizes.size(); ++i) {
        Node leaf;
        leaf.leaf = true;
        leaf.keys.assign(entries.begin() + pos, entries.begin() + pos + sizes[i]);
        leaf.next = (i + 1 < ids.size()) ? ids[i + 1] : NIL;
        write_node(ids[i], leaf);
        level.push_back({ids[i], leaf.keys.front()});
        pos += sizes[i];
    }
    levels_ = 1;

    // Upper levels: group children under parents until one root remains.
    // A parent's key i is the smallest key under child i+1.
    while (level.size() > 1) {
        std::vector<size_t> groups = group_sizes(level.size(), n_ + 1, min_internal_ + 1);
        std::vector<Built> parents;
        size_t p = 0;
        for (size_t g : groups) {
            Node node;
            node.leaf = false;
            for (size_t j = 0; j < g; ++j) {
                node.children.push_back(level[p + j].id);
                if (j > 0) node.keys.push_back(level[p + j].min);
            }
            uint32_t id = allocate_node();
            write_node(id, node);
            parents.push_back({id, level[p].min});
            p += g;
        }
        level.swap(parents);
        ++levels_;
    }
    root_ = level[0].id;
    save_meta();
}

// ======================================================================
// Range search
// ======================================================================

std::vector<Key> BPlusTree::range_greater_than(float v) {
    std::vector<Key> out;
    if (root_ == NIL) return out;

    // Every entry with value > v is greater than (v, NIL), and every entry
    // with value == v is smaller, since NIL is never a real record id.
    const Key target{v, NIL};

    Node node = read_node(root_);
    while (!node.leaf) {
        size_t i = std::upper_bound(node.keys.begin(), node.keys.end(), target) - node.keys.begin();
        node = read_node(node.children[i]);
    }
    auto it = std::upper_bound(node.keys.begin(), node.keys.end(), target);
    out.insert(out.end(), it, node.keys.end());
    while (node.next != NIL) {
        node = read_node(node.next);
        out.insert(out.end(), node.keys.begin(), node.keys.end());
    }
    return out;
}

// ======================================================================
// Deletion
// ======================================================================

bool BPlusTree::remove(const Key& k) {
    if (root_ == NIL) return false;
    RemoveResult r = remove_from(root_, k);
    if (!r.found) return false;

    if (!r.leaf && r.key_count == 0) {  // root has one child left: shrink the tree
        root_ = r.first_child;
        --levels_;
        save_meta();
    } else if (r.leaf && r.key_count == 0) {  // tree is now empty
        root_ = NIL;
        levels_ = 0;
        save_meta();
    }
    return true;
}

BPlusTree::RemoveResult BPlusTree::remove_from(uint32_t id, const Key& k) {
    Node node = read_node(id);
    RemoveResult r;
    r.leaf = node.leaf;

    if (node.leaf) {
        auto it = std::lower_bound(node.keys.begin(), node.keys.end(), k);
        if (it == node.keys.end() || !(*it == k)) return r;
        size_t pos = it - node.keys.begin();
        node.keys.erase(it);
        write_node(id, node);

        r.found = true;
        if (pos == 0 && !node.keys.empty()) {
            r.min_changed = true;
            r.new_min = node.keys.front();
        }
        r.key_count = node.keys.size();
        r.underflow = (id != root_) && node.keys.size() < min_leaf_;
        return r;
    }

    size_t i = std::upper_bound(node.keys.begin(), node.keys.end(), k) - node.keys.begin();
    RemoveResult c = remove_from(node.children[i], k);
    if (!c.found) return r;
    r.found = true;

    bool dirty = false;
    if (c.min_changed) {
        if (i > 0) {  // the separator for this child is the one to update
            node.keys[i - 1] = c.new_min;
            dirty = true;
        } else {      // leftmost child: the separator lives further up
            r.min_changed = true;
            r.new_min = c.new_min;
        }
    }
    if (c.underflow) {
        fix_underflow(node, i);
        dirty = true;
    }
    if (dirty) write_node(id, node);

    r.key_count = node.keys.size();
    r.first_child = node.children.front();
    r.underflow = (id != root_) && node.keys.size() < min_internal_;
    return r;
}

// Child i of `parent` has too few keys. Following the lecture: try to
// borrow from the left sibling, then the right sibling; otherwise merge.
// `parent` is modified in memory; the caller writes it back.
void BPlusTree::fix_underflow(Node& parent, size_t i) {
    uint32_t cid = parent.children[i];
    Node child = read_node(cid);
    size_t min_keys = child.leaf ? min_leaf_ : min_internal_;
    bool has_left = i > 0;
    bool has_right = i + 1 < parent.children.size();

    uint32_t lid = NIL, rid = NIL;
    Node left, right;

    // Case 1: borrow from the left sibling.
    if (has_left) {
        lid = parent.children[i - 1];
        left = read_node(lid);
        if (left.keys.size() > min_keys) {
            if (child.leaf) {
                child.keys.insert(child.keys.begin(), left.keys.back());
                left.keys.pop_back();
                parent.keys[i - 1] = child.keys.front();
            } else {
                child.keys.insert(child.keys.begin(), parent.keys[i - 1]);
                child.children.insert(child.children.begin(), left.children.back());
                parent.keys[i - 1] = left.keys.back();
                left.keys.pop_back();
                left.children.pop_back();
            }
            write_node(lid, left);
            write_node(cid, child);
            return;
        }
    }

    // Case 2: borrow from the right sibling.
    if (has_right) {
        rid = parent.children[i + 1];
        right = read_node(rid);
        if (right.keys.size() > min_keys) {
            if (child.leaf) {
                child.keys.push_back(right.keys.front());
                right.keys.erase(right.keys.begin());
                parent.keys[i] = right.keys.front();
            } else {
                child.keys.push_back(parent.keys[i]);
                child.children.push_back(right.children.front());
                parent.keys[i] = right.keys.front();
                right.keys.erase(right.keys.begin());
                right.children.erase(right.children.begin());
            }
            write_node(rid, right);
            write_node(cid, child);
            return;
        }
    }

    // Case 3: merge. Prefer merging into the left sibling.
    if (has_left) {
        if (child.leaf) {
            left.keys.insert(left.keys.end(), child.keys.begin(), child.keys.end());
            left.next = child.next;
        } else {
            left.keys.push_back(parent.keys[i - 1]);
            left.keys.insert(left.keys.end(), child.keys.begin(), child.keys.end());
            left.children.insert(left.children.end(), child.children.begin(), child.children.end());
        }
        parent.keys.erase(parent.keys.begin() + static_cast<long>(i - 1));
        parent.children.erase(parent.children.begin() + static_cast<long>(i));
        write_node(lid, left);
    } else {
        if (child.leaf) {
            child.keys.insert(child.keys.end(), right.keys.begin(), right.keys.end());
            child.next = right.next;
        } else {
            child.keys.push_back(parent.keys[i]);
            child.keys.insert(child.keys.end(), right.keys.begin(), right.keys.end());
            child.children.insert(child.children.end(), right.children.begin(), right.children.end());
        }
        parent.keys.erase(parent.keys.begin() + static_cast<long>(i));
        parent.children.erase(parent.children.begin() + static_cast<long>(i + 1));
        write_node(cid, child);
    }
    // The merged away block is simply no longer referenced.
}

// ======================================================================
// Statistics, validation, printing
// ======================================================================

TreeStats BPlusTree::stats() {
    TreeStats s;
    if (root_ == NIL) return s;
    std::vector<uint32_t> level{root_};
    while (!level.empty()) {
        ++s.levels;
        std::vector<uint32_t> next;
        for (uint32_t id : level) {
            Node nd = read_node(id);
            ++s.nodes;
            if (nd.leaf) {
                ++s.leaves;
                s.entries += nd.keys.size();
            } else {
                next.insert(next.end(), nd.children.begin(), nd.children.end());
            }
        }
        level.swap(next);
    }
    return s;
}

Node BPlusTree::root() {
    if (root_ == NIL) return Node{};
    return read_node(root_);
}

bool BPlusTree::check_subtree(uint32_t id, const Key* low, const Key* high, size_t depth,
                              size_t& leaf_depth, size_t& entries, std::string& error) {
    Node nd = read_node(id);
    size_t k = nd.keys.size();
    std::ostringstream where;
    where << "node " << id << ": ";

    size_t minimum = (id == root_) ? (nd.leaf ? 0 : 1) : (nd.leaf ? min_leaf_ : min_internal_);
    if (k > n_ || k < minimum) {
        error = where.str() + "key count " + std::to_string(k) + " outside allowed range";
        return false;
    }
    for (size_t j = 0; j < k; ++j) {
        if (j > 0 && !(nd.keys[j - 1] < nd.keys[j])) {
            error = where.str() + "keys not sorted";
            return false;
        }
        if ((low && nd.keys[j] < *low) || (high && !(nd.keys[j] < *high))) {
            error = where.str() + "key outside the range set by its parent";
            return false;
        }
    }
    if (nd.leaf) {
        if (leaf_depth == 0) leaf_depth = depth;
        if (depth != leaf_depth) {
            error = where.str() + "leaves at different depths";
            return false;
        }
        entries += k;
        return true;
    }
    if (nd.children.size() != k + 1) {
        error = where.str() + "wrong number of children";
        return false;
    }
    for (size_t j = 0; j <= k; ++j) {
        const Key* lo = (j == 0) ? low : &nd.keys[j - 1];
        const Key* hi = (j == k) ? high : &nd.keys[j];
        if (!check_subtree(nd.children[j], lo, hi, depth + 1, leaf_depth, entries, error))
            return false;
    }
    return true;
}

bool BPlusTree::validate(std::string& error) {
    if (root_ == NIL) return true;
    size_t leaf_depth = 0, entries = 0;
    if (!check_subtree(root_, nullptr, nullptr, 1, leaf_depth, entries, error)) return false;
    if (leaf_depth != levels_) {
        error = "stored level count does not match the tree";
        return false;
    }

    // The leaf chain must visit every entry once, in sorted order.
    Node node = read_node(root_);
    while (!node.leaf) node = read_node(node.children.front());
    size_t chained = 0;
    bool have_prev = false;
    Key prev{};
    for (;;) {
        for (const Key& key : node.keys) {
            if (have_prev && !(prev < key)) {
                error = "leaf chain out of order";
                return false;
            }
            prev = key;
            have_prev = true;
            ++chained;
        }
        if (node.next == NIL) break;
        node = read_node(node.next);
    }
    if (chained != entries) {
        error = "leaf chain does not cover every entry";
        return false;
    }
    return true;
}

void BPlusTree::print(std::ostream& out) {
    if (root_ == NIL) {
        out << "(empty tree)\n";
        return;
    }
    std::vector<uint32_t> level{root_};
    size_t depth = levels_;
    out << std::fixed << std::setprecision(3);
    while (!level.empty()) {
        out << "Level " << depth-- << ": ";
        std::vector<uint32_t> next;
        for (uint32_t id : level) {
            Node nd = read_node(id);
            out << "[";
            for (size_t j = 0; j < nd.keys.size(); ++j)
                out << (j ? " " : "") << nd.keys[j].value;
            out << "] ";
            if (!nd.leaf) next.insert(next.end(), nd.children.begin(), nd.children.end());
        }
        out << "\n";
        level.swap(next);
    }
}
