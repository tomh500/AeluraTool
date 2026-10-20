#include "Recording.h"
#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <winrt/base.h>
#include "../ScreenShotPlus/include/nlohmann/json.hpp"
namespace aelura::recording {
using json = nlohmann::json;
void validate(Options const& o, RECT r) {
    const auto w = r.right - r.left, h = r.bottom - r.top;
    if (w < 2 || h < 2 || w > 16384 || h > 16384 || o.fps < 1 || o.fps > 120 ||
        o.bitrate < 100000 || o.bitrate > 200000000 || o.scale < 10 || o.scale > 100 ||
        o.replaySeconds < 10 || o.replaySeconds > 600 || static_cast<unsigned>(o.codec) > 1 ||
        static_cast<unsigned>(o.encoder) > 1 || static_cast<unsigned>(o.scaling) > 3 ||
        static_cast<unsigned>(o.graphics)>1 || o.directory.empty() || !o.directory.is_absolute() || w * uint64_t(h) * o.scale * o.scale / 10000 > 33177600)
        throw winrt::hresult_invalid_argument();
}
Options load(std::filesystem::path const& file) {
    Options o;
    try {
        std::ifstream stream(file); auto j = json::parse(stream);
        o.fps = j.at("fps"); o.bitrate = j.at("bitrate"); o.scale = j.at("scale");
        o.replaySeconds = j.at("replaySeconds"); o.codec = static_cast<Codec>(j.at("codec").get<int>());
        o.encoder = static_cast<Encoder>(j.at("encoder").get<int>()); o.scaling = static_cast<Scaling>(j.at("scaling").get<int>());
        o.microphone = j.at("microphone"); o.cursor = j.at("cursor"); o.performanceBeta=j.value("performanceBeta",false);
        o.replayEnabled=j.value("replayEnabled",false);
        o.graphics=static_cast<Graphics>(j.value("graphics",0));
        o.directory = winrt::to_hstring(j.at("directory").get<std::string>()).c_str();
        validate(o, {0,0,1920,1080}); return o;
    } catch (...) { return Options{}; }
}
void save(std::filesystem::path const& file, Options const& o) {
    validate(o, {0,0,1920,1080});
    json j{{"fps",o.fps},{"bitrate",o.bitrate},{"scale",o.scale},{"replaySeconds",o.replaySeconds},
        {"codec",static_cast<int>(o.codec)},{"encoder",static_cast<int>(o.encoder)},
        {"scaling",static_cast<int>(o.scaling)},{"microphone",o.microphone},{"cursor",o.cursor},{"performanceBeta",o.performanceBeta},
        {"directory",winrt::to_string(o.directory.wstring())},{"replayEnabled",o.replayEnabled},{"graphics",int(o.graphics)}};
    std::filesystem::create_directories(file.parent_path()); auto temp = file; temp += L".tmp";
    { std::ofstream s(temp, std::ios::binary | std::ios::trunc); s.exceptions(std::ios::badbit | std::ios::failbit); s << j.dump(2); s.flush(); }
    if (!MoveFileExW(temp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) winrt::throw_last_error();
}
RECT selectionRect(POINT a, POINT b, RECT d) {
    a.x = std::clamp(a.x,d.left,d.right); b.x = std::clamp(b.x,d.left,d.right);
    a.y = std::clamp(a.y,d.top,d.bottom); b.y = std::clamp(b.y,d.top,d.bottom);
    return {std::min(a.x,b.x),std::min(a.y,b.y),std::max(a.x,b.x),std::max(a.y,b.y)};
}
unsigned outputExtent(unsigned pixels, unsigned scale) {
    // NV12 requires even dimensions. Round up so the last source row/column
    // survives, including a full monitor whose last mouse coordinate is 2559.
    return std::max(2u, (pixels * scale + 99) / 100 + 1) & ~1u;
}
size_t replayFirst(std::vector<double> const& d, double limit) {
    double total = 0; size_t first = d.size();
    while (first && total + d[first - 1] <= limit + 0.001) total += d[--first];
    return first;
}
}
