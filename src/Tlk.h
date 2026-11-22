#pragma once

#include <windows.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct TlkLocation
{
    std::wstring tlkPath;
    std::wstring applyPath;
};

struct TlkBytes
{
    const uint8_t* data = nullptr;
    size_t size = 0;
    virtual ~TlkBytes();
};

std::wstring MatchTlkPath(const std::vector<TlkLocation>& locations, const std::wstring& itemPath);
bool ReadTlkString(const uint8_t* data, size_t size, uint32_t strref, std::wstring& text);
std::shared_ptr<TlkBytes> OpenTlkForItem(const std::wstring& itemPath);
