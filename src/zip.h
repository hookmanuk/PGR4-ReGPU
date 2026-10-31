#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

// Calls fn(name, data) for each member whose lowercase name contains `filter` ("" for all).
void ForEachZipMember(const std::vector<uint8_t>& zip, const char* filter,
    const std::function<void(const std::string&, const std::vector<uint8_t>&)>& fn);
// One member by path (case-insensitive); false if missing or unreadable.
bool ReadZipMember(const std::filesystem::path& zipPath, const std::string& member, std::vector<uint8_t>& out);
// Raw deflate (RFC 1951) data, appended to out; false if it is corrupt.
bool Inflate(const uint8_t* data, size_t size, std::vector<uint8_t>& out);
