// 战史事件表：CSV 读取 + 按日期筛选
#pragma once

#include <string>
#include <vector>

struct WarEvent {
    int startDate = 0;   // YYYYMMDD
    int endDate = 0;     // YYYYMMDD
    int startDay = 0;    // 自 1939-01-01 起的天数
    int endDay = 0;
    std::string name;
    std::string category;    // 战斗/失守/收复/登陆/调动/背景
    std::string place;
    std::string side;        // 中/日/双方
    std::string summary;
    std::string source;      // 来源标识，对应 data/sources.csv
    std::string confidence;  // 高/中/存疑
    float lon = 0.f;
    float lat = 0.f;
    bool hasCoord = false;
};

int DateToDays(int yyyymmdd);   // 自 1939-01-01 起的天数
std::string DaysToDateString(int days);

class EventTable {
public:
    bool Load(const std::string& path);
    const std::vector<WarEvent>& All() const { return events_; }
    size_t Count() const { return events_.size(); }

private:
    std::vector<WarEvent> events_;
};
