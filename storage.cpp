// storage.cpp
// Implementation of the simulated disk and the data file.

#include "storage.h"

#include <cctype>
#include <optional>
#include <sstream>
#include <stdexcept>

// ======================================================================
// Disk
// ======================================================================

Disk::Disk(const std::string& path, size_t block_size, bool truncate)
    : path_(path), block_size_(block_size) {
    if (truncate) {
        std::ofstream f(path_, std::ios::out | std::ios::trunc | std::ios::binary);
    } else {
        std::ofstream f(path_, std::ios::app | std::ios::binary);  // create if missing
    }
    file_.open(path_, std::ios::in | std::ios::out | std::ios::binary);
    if (!file_) throw std::runtime_error("could not open disk file: " + path_);
}

std::vector<char> Disk::read_block(uint32_t block_id) {
    std::vector<char> buf(block_size_);
    file_.clear();
    file_.seekg(static_cast<std::streamoff>(block_id) * static_cast<std::streamoff>(block_size_));
    file_.read(buf.data(), static_cast<std::streamsize>(block_size_));
    if (static_cast<size_t>(file_.gcount()) != block_size_) {
        file_.clear();
        throw std::out_of_range("block " + std::to_string(block_id) + " does not exist");
    }
    ++reads_;
    touched_.insert(block_id);
    return buf;
}

void Disk::write_block(uint32_t block_id, const std::vector<char>& data, bool counted) {
    if (data.size() > block_size_) throw std::runtime_error("data larger than block size");
    std::vector<char> buf(block_size_, 0);
    std::memcpy(buf.data(), data.data(), data.size());
    file_.clear();
    file_.seekp(static_cast<std::streamoff>(block_id) * static_cast<std::streamoff>(block_size_));
    file_.write(buf.data(), static_cast<std::streamsize>(block_size_));
    if (!file_) throw std::runtime_error("write_block failed");
    if (counted) {
        ++writes_;
        touched_.insert(block_id);
    }
}

uint32_t Disk::num_blocks() {
    file_.clear();
    file_.seekg(0, std::ios::end);
    return static_cast<uint32_t>(static_cast<size_t>(file_.tellg()) / block_size_);
}

void Disk::reset_stats() {
    reads_ = 0;
    writes_ = 0;
    touched_.clear();
}

// ======================================================================
// Parsing one line of games.txt into a Record
// ======================================================================

static std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

// Strips trailing whitespace, including the '\r' left by Windows line endings.
static std::string rstrip(const std::string& s) {
    size_t end = s.size();
    while (end > 0 && std::isspace(static_cast<unsigned char>(s[end - 1]))) --end;
    return s.substr(0, end);
}

static std::vector<std::string> split_fields(const std::string& raw_line) {
    std::string line = rstrip(raw_line);
    std::vector<std::string> out;
    std::stringstream ss(line);
    std::string field;
    while (std::getline(ss, field, '\t')) out.push_back(field);
    // A trailing empty field is dropped by getline; restore it.
    if (!line.empty() && line.back() == '\t') out.push_back("");
    if (out.size() != 9) {  // fall back to whitespace separated input
        out.clear();
        std::stringstream ws(line);
        while (ws >> field) out.push_back(field);
    }
    return out;
}

// Accepts DD/MM/YYYY (the dataset's format) or YYYY/MM/DD style ISO dates.
static uint32_t encode_date(const std::string& raw) {
    std::string s = trim(raw);
    char sep = (s.find('/') != std::string::npos) ? '/' : '-';
    std::vector<std::string> parts;
    std::stringstream ss(s);
    std::string part;
    while (std::getline(ss, part, sep)) parts.push_back(part);
    if (parts.size() != 3) throw std::invalid_argument("bad date: " + raw);

    int year, month, day;
    if (sep == '/') {  // DD/MM/YYYY
        day = std::stoi(parts[0]);
        month = std::stoi(parts[1]);
        year = std::stoi(parts[2]);
    } else {           // ISO order
        year = std::stoi(parts[0]);
        month = std::stoi(parts[1]);
        day = std::stoi(parts[2]);
    }
    if (month < 1 || month > 12 || day < 1 || day > 31)
        throw std::invalid_argument("date out of range: " + raw);
    return static_cast<uint32_t>(year) * 10000u + static_cast<uint32_t>(month) * 100u +
           static_cast<uint32_t>(day);
}

// Returns nothing if the row has missing or invalid fields (row is skipped).
static std::optional<Record> parse_row(const std::vector<std::string>& f) {
    if (f.size() != 9) return std::nullopt;
    for (const auto& s : f)
        if (trim(s).empty()) return std::nullopt;
    try {
        Record r{};
        r.game_date_est  = encode_date(f[0]);
        r.team_id_home   = static_cast<uint32_t>(std::stod(f[1]));
        r.pts_home       = static_cast<uint16_t>(std::stod(f[2]));
        r.fg_pct_home    = std::stof(f[3]);
        r.ft_pct_home    = std::stof(f[4]);
        r.fg3_pct_home   = std::stof(f[5]);
        r.ast_home       = static_cast<uint16_t>(std::stod(f[6]));
        r.reb_home       = static_cast<uint16_t>(std::stod(f[7]));
        r.home_team_wins = static_cast<uint8_t>(std::stod(f[8]));
        r.valid          = 1;
        return r;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

// ======================================================================
// DataFile
// ======================================================================

DataFile::DataFile(const std::string& path, size_t block_size, bool truncate)
    : disk_(path, block_size, truncate) {
    if (block_size < BLOCK_HEADER_SIZE + RECORD_SIZE)
        throw std::runtime_error("block size too small to hold a single record");
    rpb_ = (block_size - BLOCK_HEADER_SIZE) / RECORD_SIZE;
}

void DataFile::locate(uint32_t record_id, uint32_t& block, size_t& slot) const {
    block = static_cast<uint32_t>(record_id / rpb_);
    slot = record_id % rpb_;
}

uint32_t DataFile::record_id(uint32_t block, size_t slot) const {
    return static_cast<uint32_t>(block * rpb_ + slot);
}

uint32_t DataFile::used_slots(const std::vector<char>& blk) {
    uint32_t n;
    std::memcpy(&n, blk.data(), BLOCK_HEADER_SIZE);
    return n;
}

Record DataFile::record_at(const std::vector<char>& blk, size_t slot) {
    Record r;
    std::memcpy(&r, blk.data() + BLOCK_HEADER_SIZE + slot * RECORD_SIZE, RECORD_SIZE);
    return r;
}

void DataFile::mark_deleted(std::vector<char>& blk, size_t slot) {
    blk[BLOCK_HEADER_SIZE + slot * RECORD_SIZE + VALID_OFFSET] = 0;
}

LoadStats DataFile::load_from_text(const std::string& txt_path) {
    std::ifstream in(txt_path, std::ios::binary);
    if (!in) throw std::runtime_error("could not open input file: " + txt_path);

    LoadStats stats;
    std::vector<Record> pending;
    pending.reserve(rpb_);
    uint32_t block_id = 0;

    auto flush = [&]() {
        std::vector<char> buf(BLOCK_HEADER_SIZE + pending.size() * RECORD_SIZE);
        uint32_t used = static_cast<uint32_t>(pending.size());
        std::memcpy(buf.data(), &used, BLOCK_HEADER_SIZE);
        std::memcpy(buf.data() + BLOCK_HEADER_SIZE, pending.data(), pending.size() * RECORD_SIZE);
        disk_.write_block(block_id++, buf);
        pending.clear();
    };

    std::string line;
    std::getline(in, line);  // header row
    while (std::getline(in, line)) {
        if (rstrip(line).empty()) continue;
        auto rec = parse_row(split_fields(line));
        if (!rec) {
            ++stats.skipped;
            continue;
        }
        pending.push_back(*rec);
        ++stats.records;
        if (pending.size() == rpb_) flush();
    }
    if (!pending.empty()) flush();

    stats.blocks = block_id;
    return stats;
}
