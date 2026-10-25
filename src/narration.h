// 解说词：按当前日期匹配一段旁白
#pragma once

#include <string>
#include <vector>

struct NarrationLine {
    int startDay = 0;
    int endDay = 0;
    std::string text;
};

class Narration {
public:
    bool Load(const std::string& path);
    const std::string* TextFor(int day) const;
    const std::vector<NarrationLine>& All() const { return lines_; }
    size_t Count() const { return lines_.size(); }

private:
    std::vector<NarrationLine> lines_;
};
