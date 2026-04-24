#include "range_map.h"

#include <algorithm>
#include <cassert>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

// Bespoke parser for our fixed-schema topology.yaml.
// Handles exactly the format written by src/common/topology.yaml (no anchors,
// no multi-document, no complex types). Zero external dependencies.

namespace plin {

namespace {

// Trim leading/trailing whitespace and strip inline comments starting with '#'.
std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return {};
    size_t end = s.find_last_not_of(" \t\r\n");
    std::string t = s.substr(start, end - start + 1);
    // strip inline comment
    auto c = t.find(" #");
    if (c != std::string::npos) t = t.substr(0, c);
    return t;
}

// Parse "key: value" → {key, value}. Returns false if no colon found.
bool split_kv(const std::string& line, std::string& key, std::string& val) {
    auto pos = line.find(':');
    if (pos == std::string::npos) return false;
    key = trim(line.substr(0, pos));
    val = trim(line.substr(pos + 1));
    return true;
}

// Parse an integer from s, stripping surrounding brackets/quotes.
template<typename T>
T parse_int(const std::string& s) {
    std::string t = s;
    for (char ch : {'{', '}', '[', ']', '\''}) {
        t.erase(std::remove(t.begin(), t.end(), ch), t.end());
    }
    return static_cast<T>(std::stoll(trim(t)));
}

// Parse "host: X, port: Y, ..." from a brace-style inline map or subsequent lines.
// We look for "host" and "port" tokens anywhere in the line.
bool extract_host_port(const std::string& s, std::string& host, uint16_t& port) {
    // Try simple token search: "host: 127.0.0.1, port: 7000"
    auto find_val = [&](const std::string& key) -> std::string {
        auto pos = s.find(key + ":");
        if (pos == std::string::npos) return {};
        auto after = s.find_first_not_of(" \t", pos + key.size() + 1);
        if (after == std::string::npos) return {};
        auto end = s.find_first_of(",}", after);
        return trim(s.substr(after, end == std::string::npos ? std::string::npos : end - after));
    };
    std::string h = find_val("host");
    std::string p = find_val("port");
    if (h.empty() || p.empty()) return false;
    host = h;
    port = static_cast<uint16_t>(std::stoi(p));
    return true;
}

// Parse list of ints from "[1, 2, 3]" or "1, 2, 3".
std::vector<int> parse_int_list(const std::string& s) {
    std::string t = s;
    for (char ch : {'[', ']', '{'}) {
        t.erase(std::remove(t.begin(), t.end(), ch), t.end());
    }
    std::vector<int> result;
    std::istringstream ss(t);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        std::string tr = trim(tok);
        if (!tr.empty()) result.push_back(std::stoi(tr));
    }
    return result;
}

}  // namespace

bool RangeMap::load(const std::string& yaml_path) {
    std::ifstream f(yaml_path);
    if (!f) {
        std::cerr << "[RangeMap] cannot open " << yaml_path << "\n";
        return false;
    }

    enum class Section { NONE, CLOUD, EDGES, ENDS } section = Section::NONE;
    EdgeInfo cur_edge{};
    EndInfo  cur_end{};
    bool in_edge = false;
    bool in_end  = false;

    auto flush_edge = [&] {
        if (in_edge) { edges_.push_back(cur_edge); cur_edge = {}; in_edge = false; }
    };
    auto flush_end = [&] {
        if (in_end) { ends_.push_back(cur_end); cur_end = {}; in_end = false; }
    };

    std::string raw;
    while (std::getline(f, raw)) {
        std::string line = trim(raw);
        if (line.empty() || line[0] == '#') continue;

        // Detect section headers
        if (line.rfind("cloud:", 0) == 0) {
            flush_edge(); flush_end();
            section = Section::CLOUD;
            // The value part may be on the same line or on following lines
            std::string val = trim(line.substr(6));
            if (!val.empty() && val[0] == '{') {
                extract_host_port(val, cloud_.host, cloud_.port);
            }
            continue;
        }
        if (line == "edges:") {
            flush_edge(); flush_end();
            section = Section::EDGES;
            continue;
        }
        if (line == "ends:") {
            flush_edge(); flush_end();
            section = Section::ENDS;
            continue;
        }

        if (section == Section::CLOUD) {
            std::string k, v;
            if (split_kv(line, k, v)) {
                if (k == "host") cloud_.host = v;
                else if (k == "port") cloud_.port = static_cast<uint16_t>(std::stoi(v));
            }
        }

        if (section == Section::EDGES) {
            // A new list item "- { ... }"
            if (line[0] == '-') {
                flush_edge();
                in_edge = true;
                std::string rest = trim(line.substr(1));
                if (!rest.empty() && rest[0] == '{') {
                    // Inline: - { id: 1, host: ..., port: ..., ends: [...] }
                    auto id_pos = rest.find("id:");
                    if (id_pos != std::string::npos) {
                        auto after = rest.find_first_not_of(" \t", id_pos + 3);
                        auto comma = rest.find(',', after);
                        cur_edge.id = std::stoi(rest.substr(after, comma - after));
                    }
                    extract_host_port(rest, cur_edge.host, cur_edge.port);
                    auto ends_pos = rest.find("ends:");
                    if (ends_pos != std::string::npos) {
                        cur_edge.end_ids = parse_int_list(rest.substr(ends_pos + 5));
                    }
                }
            } else if (in_edge) {
                std::string k, v;
                if (split_kv(line, k, v)) {
                    if (k == "id") cur_edge.id = std::stoi(v);
                    else if (k == "host") cur_edge.host = v;
                    else if (k == "port") cur_edge.port = static_cast<uint16_t>(std::stoi(v));
                    else if (k == "ends") cur_edge.end_ids = parse_int_list(v);
                }
            }
        }

        if (section == Section::ENDS) {
            if (line[0] == '-') {
                flush_end();
                in_end = true;
                std::string rest = trim(line.substr(1));
                if (!rest.empty() && rest[0] == '{') {
                    // Inline: - { id: X, edge: Y, host: Z, port: P, key_range: [lo, hi] }
                    auto parse_field = [&](const std::string& key) -> std::string {
                        auto pos = rest.find(key + ":");
                        if (pos == std::string::npos) return {};
                        auto after = rest.find_first_not_of(" \t", pos + key.size() + 1);
                        auto end = rest.find_first_of(",}", after);
                        return trim(rest.substr(after, end == std::string::npos ? std::string::npos : end - after));
                    };
                    std::string id_s = parse_field("id");
                    std::string edge_s = parse_field("edge");
                    if (!id_s.empty()) cur_end.id = std::stoi(id_s);
                    if (!edge_s.empty()) cur_end.edge_id = std::stoi(edge_s);
                    extract_host_port(rest, cur_end.host, cur_end.port);
                    // key_range: [lo, hi]
                    auto kr = rest.find("key_range:");
                    if (kr != std::string::npos) {
                        auto lb = rest.find('[', kr);
                        auto rb = rest.find(']', lb);
                        if (lb != std::string::npos && rb != std::string::npos) {
                            auto inner = rest.substr(lb + 1, rb - lb - 1);
                            auto comma = inner.find(',');
                            cur_end.key_lo = std::stod(trim(inner.substr(0, comma)));
                            cur_end.key_hi = std::stod(trim(inner.substr(comma + 1)));
                        }
                    }
                }
            } else if (in_end) {
                std::string k, v;
                if (split_kv(line, k, v)) {
                    if (k == "id") cur_end.id = std::stoi(v);
                    else if (k == "edge") cur_end.edge_id = std::stoi(v);
                    else if (k == "host") cur_end.host = v;
                    else if (k == "port") cur_end.port = static_cast<uint16_t>(std::stoi(v));
                    else if (k == "key_lo") cur_end.key_lo = std::stod(v);
                    else if (k == "key_hi") cur_end.key_hi = std::stod(v);
                }
            }
        }
    }
    flush_edge();
    flush_end();

    if (cloud_.host.empty() || cloud_.port == 0) {
        std::cerr << "[RangeMap] cloud block missing or incomplete\n";
        return false;
    }
    if (edges_.empty() || ends_.empty()) {
        std::cerr << "[RangeMap] edges or ends list is empty\n";
        return false;
    }
    // Sort ends by key_lo for binary search
    std::sort(ends_.begin(), ends_.end(),
              [](const EndInfo& a, const EndInfo& b){ return a.key_lo < b.key_lo; });
    return true;
}

int RangeMap::locate_end(key_t k) const {
    // Binary search over sorted ends_
    int lo = 0, hi = static_cast<int>(ends_.size()) - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (k < ends_[mid].key_lo) hi = mid - 1;
        else if (k > ends_[mid].key_hi) lo = mid + 1;
        else return ends_[mid].id;
    }
    return -1;
}

int RangeMap::edge_of(int end_id) const {
    for (const auto& e : ends_)
        if (e.id == end_id) return e.edge_id;
    return -1;
}

std::vector<int> RangeMap::siblings_of(int end_id) const {
    int my_edge = edge_of(end_id);
    if (my_edge < 0) return {};
    std::vector<int> result;
    for (const auto& e : ends_)
        if (e.edge_id == my_edge && e.id != end_id)
            result.push_back(e.id);
    return result;
}

bool RangeMap::same_edge(int a, int b) const {
    return edge_of(a) == edge_of(b) && edge_of(a) >= 0;
}

}  // namespace plin
