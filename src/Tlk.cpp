#include "Tlk.h"

#include <cwctype>
#include <map>
#include <string.h>

namespace
{
    constexpr uint64_t kMaxTlkBytes = 256ull * 1024ull * 1024ull;
    constexpr uint32_t kMaxStringBytes = 1024u * 1024u;

    struct MappedTlk : TlkBytes
    {
        HANDLE file = INVALID_HANDLE_VALUE;
        HANDLE mapping = nullptr;

        ~MappedTlk() override
        {
            if (data != nullptr)
            {
                UnmapViewOfFile(data);
                data = nullptr;
            }
            if (mapping != nullptr)
            {
                CloseHandle(mapping);
            }
            if (file != INVALID_HANDLE_VALUE)
            {
                CloseHandle(file);
            }
        }
    };

    struct CachedTlk
    {
        std::shared_ptr<TlkBytes> bytes;
        FILETIME written = {};
    };

    std::wstring ReadRegString(HKEY root, const wchar_t* subkey, const wchar_t* name)
    {
        DWORD bytes = 0;
        if (RegGetValueW(root, subkey, name, RRF_RT_REG_SZ, nullptr, nullptr, &bytes) != ERROR_SUCCESS || bytes < sizeof(wchar_t))
        {
            return {};
        }
        std::wstring value(bytes / sizeof(wchar_t), L'\0');
        if (RegGetValueW(root, subkey, name, RRF_RT_REG_SZ, nullptr, value.data(), &bytes) != ERROR_SUCCESS)
        {
            return {};
        }
        value.resize(wcsnlen(value.c_str(), value.size()));
        return value;
    }

    std::wstring Expand(const std::wstring& value)
    {
        if (value.empty())
        {
            return {};
        }
        DWORD count = ExpandEnvironmentStringsW(value.c_str(), nullptr, 0);
        if (count == 0)
        {
            return value;
        }
        std::wstring expanded(count, L'\0');
        if (ExpandEnvironmentStringsW(value.c_str(), expanded.data(), count) == 0)
        {
            return value;
        }
        expanded.resize(wcsnlen(expanded.c_str(), expanded.size()));
        return expanded;
    }

    std::wstring IeShellExRoot()
    {
        std::wstring root = ReadRegString(HKEY_LOCAL_MACHINE,
            L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment", L"ieshellex");
        if (root.empty())
        {
            root = ReadRegString(HKEY_CURRENT_USER, L"Environment", L"ieshellex");
        }
        if (root.empty())
        {
            DWORD count = GetEnvironmentVariableW(L"ieshellex", nullptr, 0);
            if (count > 1)
            {
                root.assign(count, L'\0');
                GetEnvironmentVariableW(L"ieshellex", root.data(), count);
                root.resize(wcsnlen(root.c_str(), root.size()));
            }
        }
        return Expand(root);
    }

    std::wstring Lower(std::wstring value)
    {
        if (!value.empty())
        {
            CharLowerBuffW(value.data(), static_cast<DWORD>(value.size()));
        }
        return value;
    }

    std::wstring DirectoryOf(const std::wstring& path)
    {
        const size_t slash = path.find_last_of(L"\\/");
        if (slash == std::wstring::npos)
        {
            return {};
        }
        return path.substr(0, slash);
    }

    bool ContainsInsensitive(const std::wstring& haystack, const std::wstring& needle)
    {
        if (needle.empty() || haystack.size() < needle.size())
        {
            return false;
        }
        return Lower(haystack).find(Lower(needle)) != std::wstring::npos;
    }

    std::wstring UnescapeXml(const std::wstring& text)
    {
        std::wstring out;
        out.reserve(text.size());
        for (size_t i = 0; i < text.size();)
        {
            if (text[i] != L'&')
            {
                out.push_back(text[i]);
                ++i;
                continue;
            }
            const size_t end = text.find(L';', i + 1);
            if (end == std::wstring::npos)
            {
                out.push_back(text[i]);
                ++i;
                continue;
            }
            const std::wstring token = text.substr(i + 1, end - (i + 1));
            if (token == L"amp") out += L'&';
            else if (token == L"lt") out += L'<';
            else if (token == L"gt") out += L'>';
            else if (token == L"quot") out += L'"';
            else if (token == L"apos") out += L'\'';
            else
            {
                out.append(text, i, end - i + 1);
            }
            i = end + 1;
        }
        return out;
    }

    std::wstring Trim(const std::wstring& text)
    {
        size_t begin = 0;
        while (begin < text.size() && iswspace(text[begin]))
        {
            ++begin;
        }
        size_t end = text.size();
        while (end > begin && iswspace(text[end - 1]))
        {
            --end;
        }
        return text.substr(begin, end - begin);
    }

    std::wstring ElementText(const std::wstring& block, const wchar_t* tag)
    {
        const std::wstring open = std::wstring(L"<") + tag;
        const size_t start = block.find(open);
        if (start == std::wstring::npos)
        {
            return {};
        }
        const size_t gt = block.find(L'>', start);
        if (gt == std::wstring::npos || (gt > start && block[gt - 1] == L'/'))
        {
            return {};
        }
        const std::wstring close = std::wstring(L"</") + tag + L">";
        const size_t end = block.find(close, gt + 1);
        if (end == std::wstring::npos)
        {
            return {};
        }
        return Trim(UnescapeXml(block.substr(gt + 1, end - (gt + 1))));
    }

    std::wstring DecodeBytes(const char* bytes, int length, UINT codePage)
    {
        if (bytes == nullptr || length <= 0)
        {
            return {};
        }
        const DWORD flags = codePage == CP_UTF8 ? MB_ERR_INVALID_CHARS : 0;
        const int count = MultiByteToWideChar(codePage, flags, bytes, length, nullptr, 0);
        if (count <= 0)
        {
            return {};
        }
        std::wstring wide(static_cast<size_t>(count), L'\0');
        if (MultiByteToWideChar(codePage, flags, bytes, length, wide.data(), count) <= 0)
        {
            return {};
        }
        return wide;
    }

    std::wstring DecodeXmlFile(const std::string& bytes)
    {
        if (bytes.size() >= 2
            && static_cast<unsigned char>(bytes[0]) == 0xFF
            && static_cast<unsigned char>(bytes[1]) == 0xFE)
        {
            const wchar_t* wide = reinterpret_cast<const wchar_t*>(bytes.data() + 2);
            const size_t chars = (bytes.size() - 2) / sizeof(wchar_t);
            return std::wstring(wide, wide + chars);
        }

        size_t start = 0;
        if (bytes.size() >= 3
            && static_cast<unsigned char>(bytes[0]) == 0xEF
            && static_cast<unsigned char>(bytes[1]) == 0xBB
            && static_cast<unsigned char>(bytes[2]) == 0xBF)
        {
            start = 3;
        }

        const std::string header = bytes.substr(0, bytes.size() < 200 ? bytes.size() : 200);
        const bool ibm850 = header.find("ibm850") != std::string::npos || header.find("cp850") != std::string::npos;
        if (ibm850)
        {
            return DecodeBytes(bytes.data() + start, static_cast<int>(bytes.size() - start), 850);
        }
        std::wstring utf8 = DecodeBytes(bytes.data() + start, static_cast<int>(bytes.size() - start), CP_UTF8);
        if (!utf8.empty() || bytes.size() == start)
        {
            return utf8;
        }
        return DecodeBytes(bytes.data() + start, static_cast<int>(bytes.size() - start), 1252);
    }

    std::vector<TlkLocation> ParseLocations(const std::wstring& xml)
    {
        std::vector<TlkLocation> locations;
        size_t pos = 0;
        while (pos < xml.size())
        {
            const size_t start = xml.find(L"<TlkLocationData", pos);
            if (start == std::wstring::npos)
            {
                break;
            }
            const size_t end = xml.find(L"</TlkLocationData>", start);
            if (end == std::wstring::npos)
            {
                break;
            }
            const std::wstring block = xml.substr(start, end - start);
            TlkLocation location;
            location.tlkPath = ElementText(block, L"TlkPath");
            location.applyPath = ElementText(block, L"ApplyPath");
            if (!location.tlkPath.empty() && !location.applyPath.empty())
            {
                locations.push_back(std::move(location));
            }
            pos = end + 1;
        }
        return locations;
    }

    bool LoadLocations(std::vector<TlkLocation>& locations)
    {
        static SRWLOCK lock = SRWLOCK_INIT;
        static std::vector<TlkLocation> cached;
        static std::wstring cachedPath;
        static FILETIME cachedWrite = {};
        static bool loaded = false;

        const std::wstring root = IeShellExRoot();
        if (root.empty())
        {
            return false;
        }
        std::wstring path = root;
        if (path.back() != L'\\' && path.back() != L'/')
        {
            path += L'\\';
        }
        path += L"config\\tlkLocations.xml";

        WIN32_FILE_ATTRIBUTE_DATA info = {};
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info))
        {
            return false;
        }

        AcquireSRWLockShared(&lock);
        if (loaded && cachedPath == path && CompareFileTime(&cachedWrite, &info.ftLastWriteTime) == 0)
        {
            locations = cached;
            ReleaseSRWLockShared(&lock);
            return true;
        }
        ReleaseSRWLockShared(&lock);

        const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            return false;
        }
        LARGE_INTEGER size = {};
        if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > 1024 * 1024)
        {
            CloseHandle(file);
            return false;
        }
        std::string bytes(static_cast<size_t>(size.QuadPart), '\0');
        DWORD read = 0;
        const BOOL ok = ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr);
        CloseHandle(file);
        if (!ok || read != bytes.size())
        {
            return false;
        }

        std::vector<TlkLocation> fresh = ParseLocations(DecodeXmlFile(bytes));
        AcquireSRWLockExclusive(&lock);
        cached = fresh;
        cachedPath = path;
        cachedWrite = info.ftLastWriteTime;
        loaded = true;
        locations = cached;
        ReleaseSRWLockExclusive(&lock);
        return true;
    }

    std::shared_ptr<TlkBytes> MapTlk(const std::wstring& path)
    {
        static SRWLOCK lock = SRWLOCK_INIT;
        static std::map<std::wstring, CachedTlk> cache;

        WIN32_FILE_ATTRIBUTE_DATA info = {};
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info))
        {
            return nullptr;
        }
        const uint64_t fileSize = (static_cast<uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
        if (fileSize < 18 || fileSize > kMaxTlkBytes)
        {
            return nullptr;
        }

        const std::wstring key = Lower(path);
        {
            AcquireSRWLockShared(&lock);
            const auto it = cache.find(key);
            if (it != cache.end() && CompareFileTime(&it->second.written, &info.ftLastWriteTime) == 0)
            {
                std::shared_ptr<TlkBytes> existing = it->second.bytes;
                ReleaseSRWLockShared(&lock);
                return existing;
            }
            ReleaseSRWLockShared(&lock);
        }

        auto mapped = std::make_shared<MappedTlk>();
        mapped->file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (mapped->file == INVALID_HANDLE_VALUE)
        {
            return nullptr;
        }
        mapped->mapping = CreateFileMappingW(mapped->file, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (mapped->mapping == nullptr)
        {
            return nullptr;
        }
        void* view = MapViewOfFile(mapped->mapping, FILE_MAP_READ, 0, 0, 0);
        if (view == nullptr)
        {
            return nullptr;
        }
        mapped->data = static_cast<const uint8_t*>(view);
        mapped->size = static_cast<size_t>(fileSize);

        AcquireSRWLockExclusive(&lock);
        cache[key] = CachedTlk{ mapped, info.ftLastWriteTime };
        ReleaseSRWLockExclusive(&lock);
        return mapped;
    }

    std::wstring DecodeTlkText(const uint8_t* bytes, uint32_t length)
    {
        while (length > 0 && bytes[length - 1] == 0)
        {
            --length;
        }
        if (length == 0)
        {
            return {};
        }
        std::wstring text = DecodeBytes(reinterpret_cast<const char*>(bytes), static_cast<int>(length), CP_UTF8);
        if (!text.empty())
        {
            return text;
        }
        return DecodeBytes(reinterpret_cast<const char*>(bytes), static_cast<int>(length), 1252);
    }
}

TlkBytes::~TlkBytes() = default;

std::wstring MatchTlkPath(const std::vector<TlkLocation>& locations, const std::wstring& itemPath)
{
    const std::wstring directory = DirectoryOf(itemPath);
    size_t bestLength = 0;
    std::wstring best;
    for (const TlkLocation& location : locations)
    {
        if (location.applyPath.empty() || location.tlkPath.empty())
        {
            continue;
        }
        if (location.applyPath.size() > bestLength && ContainsInsensitive(directory, location.applyPath))
        {
            bestLength = location.applyPath.size();
            best = location.tlkPath;
        }
    }
    return best;
}

bool ReadTlkString(const uint8_t* data, size_t size, uint32_t strref, std::wstring& text)
{
    text.clear();
    if (data == nullptr || size < 18 || memcmp(data, "TLK ", 4) != 0 || data[4] != 'V')
    {
        return false;
    }

    uint32_t count = 0;
    uint32_t dataOffset = 0;
    memcpy(&count, data + 0x0A, sizeof(count));
    memcpy(&dataOffset, data + 0x0E, sizeof(dataOffset));
    if (strref >= count)
    {
        return false;
    }

    const uint64_t entry = 18ull + static_cast<uint64_t>(strref) * 26ull;
    if (entry + 26 > size)
    {
        return false;
    }

    uint32_t index = 0;
    uint32_t length = 0;
    memcpy(&index, data + static_cast<size_t>(entry) + 0x12, sizeof(index));
    memcpy(&length, data + static_cast<size_t>(entry) + 0x16, sizeof(length));
    if (length == 0)
    {
        return true;
    }
    if (length > kMaxStringBytes)
    {
        return false;
    }

    const uint64_t start = static_cast<uint64_t>(dataOffset) + index;
    if (start > size || length > size - static_cast<size_t>(start))
    {
        return false;
    }

    text = DecodeTlkText(data + static_cast<size_t>(start), length);
    return true;
}

std::shared_ptr<TlkBytes> OpenTlkForItem(const std::wstring& itemPath)
{
    if (itemPath.empty())
    {
        return nullptr;
    }
    std::vector<TlkLocation> locations;
    if (!LoadLocations(locations))
    {
        return nullptr;
    }
    const std::wstring tlkPath = MatchTlkPath(locations, itemPath);
    if (tlkPath.empty())
    {
        return nullptr;
    }
    return MapTlk(tlkPath);
}
