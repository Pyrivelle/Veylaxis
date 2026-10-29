// 【阅读导航】内容身份工具：用系统密码库计算 SHA-256，作为数据版本和缓存完整性依据。
#include "veylaxis/vgds/Service.hpp"
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#include <array>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace veylaxis::vgds
{
namespace
{
class Hash
{
    BCRYPT_ALG_HANDLE algorithm_{};
    BCRYPT_HASH_HANDLE hash_{};

  public:
    Hash()
    {
        if (BCryptOpenAlgorithmProvider(&algorithm_, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
            throw std::runtime_error("SHA256 unavailable");
        if (BCryptCreateHash(algorithm_, &hash_, nullptr, 0, nullptr, 0, 0) < 0)
        {
            BCryptCloseAlgorithmProvider(algorithm_, 0);
            throw std::runtime_error("Cannot create SHA256");
        }
    }

    ~Hash()
    {
        if (hash_)
            BCryptDestroyHash(hash_);
        if (algorithm_)
            BCryptCloseAlgorithmProvider(algorithm_, 0);
    }

    void add(const char* bytes, std::size_t n)
    {
        if (n > ULONG_MAX || BCryptHashData(hash_, reinterpret_cast<PUCHAR>(const_cast<char*>(bytes)),
                                            static_cast<ULONG>(n), 0) < 0)
            throw std::runtime_error("Cannot hash data");
    }

    std::string finish()
    {
        std::array<unsigned char, 32> data{};
        if (BCryptFinishHash(hash_, data.data(), 32, 0) < 0)
            throw std::runtime_error("Cannot finish SHA256");
        std::ostringstream out;
        for (auto b : data)
            out << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(b);
        return out.str();
    }
};
}

std::string contentHash(const std::string& bytes)
{
    Hash hash;
    hash.add(bytes.data(), bytes.size());
    return hash.finish();
}

std::string fileHash(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw std::runtime_error("Cannot hash file: " + path);
    Hash h;
    std::array<char, 65536> b;
    while (in)
    {
        in.read(b.data(), b.size());
        h.add(b.data(), static_cast<std::size_t>(in.gcount()));
    }
    if (!in.eof())
        throw std::runtime_error("Cannot read hash input");
    return h.finish();
}
}
