#include "events.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace {

bool IsLeap(int y)
{
    return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

std::vector<std::string> SplitCsvLine(const std::string& line)
{
    std::vector<std::string> out;
    std::string cur;
    bool inQuotes = false;
    for (size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (inQuotes) {
            if (c == '"') {
                if (i + 1 < line.size() && line[i + 1] == '"') {
                    cur.push_back('"');
                    ++i;
                } else {
                    inQuotes = false;
                }
            } else {
                cur.push_back(c);
            }
        } else if (c == '"') {
            inQuotes = true;
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

int DateToDays(int yyyymmdd)
{
    const int y = yyyymmdd / 10000;
    const int m = (yyyymmdd / 100) % 100;
    const int d = yyyymmdd % 100;
    static const int mdays[13] = { 0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

    int total = 0;
    for (int yy = 1939; yy < y; ++yy) total += IsLeap(yy) ? 366 : 365;
    for (int mm = 1; mm < m; ++mm) total += (mm == 2 && IsLeap(y)) ? 29 : mdays[mm];
    total += d - 1;
    return total;
}

std::string DaysToDateString(int days)
{
    int y = 1939, m = 1, d = 1;
    static const int mdays[13] = { 0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    int remain = days;
    while (true) {
        const int ylen = IsLeap(y) ? 366 : 365;
        if (remain < ylen) break;
        remain -= ylen;
        ++y;
    }
    while (true) {
        const int mlen = (m == 2 && IsLeap(y)) ? 29 : mdays[m];
        if (remain < mlen) break;
        remain -= mlen;
        ++m;
    }
    d += remain;

    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", y, m, d);
    return std::string(buf);
}

bool EventTable::Load(const std::string& path)
{
    events_.clear();

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::printf("[events] 打不开事件表:%s\n", path.c_str());
        return false;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    std::string text = ss.str();
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF
        && static_cast<unsigned char>(text[1]) == 0xBB
        && static_cast<unsigned char>(text[2]) == 0xBF) {
        text.erase(0, 3);   // UTF-8 BOM
    }

    std::istringstream lines(text);
    std::string line;
    bool skippedHeader = false;
    while (std::getline(lines, line)) {
        if (line.empty()) continue;
        const std::vector<std::string> f = SplitCsvLine(line);
        if (f.empty()) continue;
        if (!skippedHeader) {
            skippedHeader = true;
            const std::string first = Trim(f[0]);
            if (first == "start_date" || first == "开始日期") continue;
        }
        if (f.size() < 8) {
            std::printf("[events] 字段不足，跳过：%s\n", line.c_str());
            continue;
        }

        WarEvent ev;
        ev.startDate = std::atoi(Trim(f[0]).c_str());
        ev.endDate = std::atoi(Trim(f[1]).c_str());
        if (ev.endDate == 0) ev.endDate = ev.startDate;
        ev.name = Trim(f[2]);
        ev.category = Trim(f[3]);
        ev.place = Trim(f[4]);
        ev.lon = static_cast<float>(std::atof(Trim(f[5]).c_str()));
        ev.lat = static_cast<float>(std::atof(Trim(f[6]).c_str()));
        ev.side = Trim(f[7]);
        if (f.size() > 8) ev.summary = Trim(f[8]);
        if (f.size() > 9) ev.source = Trim(f[9]);
        if (f.size() > 10) ev.confidence = Trim(f[10]);

        ev.hasCoord = (ev.lon > 0.f && ev.lat > 0.f);
        ev.startDay = DateToDays(ev.startDate);
        ev.endDay = DateToDays(ev.endDate);
        events_.push_back(ev);
    }

    std::printf("[events] 已加载 %zu 条事件\n", events_.size());
    return !events_.empty();
}
