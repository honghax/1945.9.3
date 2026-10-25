#include "narration.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "events.h"

namespace {

std::vector<std::string> SplitCsv(const std::string& line)
{
    std::vector<std::string> out;
    std::string cur;
    bool quoted = false;
    for (char c : line) {
        if (quoted) {
            if (c == '"') quoted = false;
            else cur.push_back(c);
        } else if (c == '"') {
            quoted = true;
        } else if (c == ',') {
            out.push_back(cur);
            cur.clear();
        } else if (c != '\r') {
            cur.push_back(c);
        }
    }
    out.push_back(cur);
    return out;
}

std::string Trim(const std::string& s)
{
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t')) --e;
    return s.substr(b, e - b);
}

}  // namespace

bool Narration::Load(const std::string& path)
{
    lines_.clear();

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::printf("[narration] 打不开解说表：%s\n", path.c_str());
        return false;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    std::string text = ss.str();
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF
        && static_cast<unsigned char>(text[1]) == 0xBB
        && static_cast<unsigned char>(text[2]) == 0xBF) {
        text.erase(0, 3);
    }

    std::istringstream lines(text);
    std::string line;
    bool header = false;
    while (std::getline(lines, line)) {
        if (line.empty()) continue;
        const std::vector<std::string> f = SplitCsv(line);
        if (f.size() < 3) continue;
        if (!header) {
            header = true;
            if (Trim(f[0]) == "start_date") continue;
        }
        NarrationLine n;
        n.startDay = DateToDays(std::atoi(Trim(f[0]).c_str()));
        n.endDay = DateToDays(std::atoi(Trim(f[1]).c_str()));
        n.text = Trim(f[2]);
        if (n.text.size() >= 2 && n.text.front() == '"' && n.text.back() == '"') {
            n.text = n.text.substr(1, n.text.size() - 2);
        }
        lines_.push_back(n);
    }

    std::printf("[narration] 已加载 %zu 段解说\n", lines_.size());
    return !lines_.empty();
}

const std::string* Narration::TextFor(int day) const
{
    for (const NarrationLine& n : lines_) {
        if (day >= n.startDay && day <= n.endDay) return &n.text;
    }
    return nullptr;
}
