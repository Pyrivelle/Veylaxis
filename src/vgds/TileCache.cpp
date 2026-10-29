// 【阅读导航】Catalog 磁盘缓存：提交标记和内容哈希用于识别完整、未损坏的缓存，不只检查文件存在。
#include "TileCache.hpp"
#include "veylaxis/vgds/Codec.hpp"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <stdexcept>
#define NOMINMAX
#include <windows.h>

namespace veylaxis::vgds
{
namespace
{
namespace fs = std::filesystem;
using J = nlohmann::json;

std::string read(const fs::path& path)
{
    if (fs::file_size(path) > 16 * 1024 * 1024)
        throw std::runtime_error("Cache file exceeds limit");
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw std::runtime_error("Cache read failed");
    return {std::istreambuf_iterator<char>(in), {}};
}

void write(const fs::path& path, const std::string& text)
{
    auto temp = path;
    temp += ".tmp";
    {
        std::ofstream out(temp, std::ios::binary);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        out.close();
        if (!out)
            throw std::runtime_error("Cache write failed");
    }
    if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("Cache commit failed");
}

void cleanupUncommitted(const fs::path& dir)
{
    if (!fs::exists(dir) || fs::exists(dir / "commit.json"))
        return;
    for (const char* file :
         {"tile.json", "tile.osgb", "tile.json.tmp", "commit.json.tmp", "tile.osgb.tmp.osgb"})
        fs::remove(dir / file);
    if (fs::is_empty(dir))
        fs::remove(dir);
}
}

TileCache::TileCache(std::string root, std::size_t entries, std::uint64_t bytes)
    : root_(fs::absolute(root).string()), entries_(entries), bytes_(bytes)
{
    if (root.empty() || entries < 1 || bytes < 1024 * 1024)
        throw std::invalid_argument("Invalid cache limits");
    fs::create_directories(root_);
}
// 缓存命中也要验证内容和文件；生成失败的半成品不作为成功结果返回。
TileResult TileCache::fetch(const std::string& key, const TileRequest& r,
                            const std::function<TileResult(const std::string&)>& generate)
try
{
    requestCheckpoint();

    struct Lock
    {
        HANDLE handle;

        explicit Lock(const fs::path& root)
            : handle(CreateFileW((root / "writer.lock").c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                 OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr))
        {
            if (handle == INVALID_HANDLE_VALUE)
                throw std::runtime_error("Cache is in use by another process");
        }

        ~Lock()
        {
            CloseHandle(handle);
        }
    } lock{fs::path(root_)};

    const fs::path root(root_), dir = root / key;
    fs::create_directories(dir);
    if (fs::exists(dir / "commit.json"))
        try
        {
            auto text = read(dir / "tile.json");
            auto commit = J::parse(read(dir / "commit.json"));
            if (commit.at("key") != key || commit.at("hash") != contentHash(text))
                throw std::runtime_error("Cache hash mismatch");
            auto decoded = decodeJson(text);
            auto* p = std::get_if<TilePackage>(&decoded);
            if (!p)
                throw std::runtime_error("Cache decode failed");
            for (const auto& a : p->artifacts)
                if (fs::path(a.path) != dir / "tile.osgb" || fs::file_size(a.path) != a.bytes ||
                    fileHash(a.path) != a.contentHash)
                    throw std::runtime_error("Cache artifact mismatch");
            if (!(p->metadata.tile == r.tile))
                throw std::runtime_error("Wrong cached tile");
            p->metadata.requestId = r.requestId;
            fs::last_write_time(dir / "commit.json", fs::file_time_type::clock::now());
            ++hits;
            return *p;
        }
        catch (const std::exception&)
        {
            ++repairs;
        }
    auto result = generate((dir / "tile.osgb").string());
    if (auto e = std::get_if<TileError>(&result))
    {
        cleanupUncommitted(dir);
        return *e;
    }
    requestCheckpoint();
    auto p = std::get<TilePackage>(std::move(result));
    p.metadata.requestId.clear();
    auto encoded = encodeJson(p);
    if (auto e = std::get_if<TileError>(&encoded))
    {
        cleanupUncommitted(dir);
        return *e;
    }
    std::uint64_t entryBytes = std::get<std::string>(encoded).size() + 256;
    for (const auto& artifact : p.artifacts)
        entryBytes += artifact.bytes;
    if (entryBytes > bytes_)
    {
        cleanupUncommitted(dir);
        return TileError{ErrorCode::GenerationFailure, "Single tile exceeds configured cache byte budget"};
    }
    write(dir / "tile.json", std::get<std::string>(encoded));
    write(dir / "commit.json", J{{"key", key}, {"hash", contentHash(std::get<std::string>(encoded))}}.dump());
    ++generations;
    prune(key);
    p.metadata.requestId = r.requestId;
    return p;
}
catch (const RequestCancelled& e)
{
    try
    {
        cleanupUncommitted(fs::path(root_) / key);
    }
    catch (...)
    {
    }
    return TileError{ErrorCode::Cancelled, e.what()};
}
catch (const std::exception& e)
{
    try
    {
        cleanupUncommitted(fs::path(root_) / key);
    }
    catch (...)
    {
    }
    return TileError{ErrorCode::IoFailure, e.what()};
}

void TileCache::prune(const std::string& keep)
{
    struct Item
    {
        fs::path path;
        std::uint64_t bytes;
        fs::file_time_type time;
    };

    std::vector<Item> items;
    std::uint64_t total = 0;
    for (const auto& entry : fs::directory_iterator(root_))
    {
        auto name = entry.path().filename().string();
        if (!entry.is_directory() || name.size() != 64 ||
            !std::all_of(name.begin(), name.end(),
                         [](char c)
                         {
                             return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
                         }) ||
            !fs::exists(entry.path() / "commit.json"))
            continue;
        std::uint64_t size = 0;
        for (const char* f : {"tile.json", "tile.osgb", "commit.json"})
            if (fs::exists(entry.path() / f))
                size += fs::file_size(entry.path() / f);
        items.push_back({entry.path(), size, fs::last_write_time(entry.path() / "commit.json")});
        total += size;
    }
    std::sort(items.begin(), items.end(),
              [](const auto& a, const auto& b)
              {
                  return a.time < b.time;
              });
    std::size_t count = items.size();
    for (const auto& item : items)
    {
        if (count <= entries_ && total <= bytes_)
            break;
        if (item.path.filename() == keep)
            continue;
        // Only exact files created by this cache are removed. Never recursively erase a directory.
        for (const char* file : {"tile.json", "tile.osgb", "commit.json", "tile.json.tmp", "commit.json.tmp",
                                 "tile.osgb.tmp.osgb"})
            fs::remove(item.path / file);
        if (fs::is_empty(item.path))
            fs::remove(item.path);
        total -= item.bytes;
        --count;
        ++evictions;
    }
}
}
