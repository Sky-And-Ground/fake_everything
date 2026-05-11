#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <shellapi.h>
#include <winioctl.h>

#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <cstdint>

#include <string>
#include <algorithm>
#include <system_error>
#include <vector>
#include <memory>

struct PathInfo {
    DWORDLONG frn;   // file reference number.
    DWORDLONG pfrn;  // parent file reference number.
    std::wstring name;

    PathInfo() = default;

    PathInfo(DWORDLONG _frn, DWORDLONG _pfrn, const std::wstring& _name)
        : frn{ _frn }, pfrn{ _pfrn }, name{ _name }
    {}
};

class WinHandle {
    HANDLE hnd;
public:
    WinHandle() : hnd{ INVALID_HANDLE_VALUE } {}

    ~WinHandle() {
        if (hnd != INVALID_HANDLE_VALUE) {
            CloseHandle(hnd);
        }
    }

    WinHandle(const WinHandle&) = delete;
    WinHandle& operator=(const WinHandle&) = delete;

    WinHandle(WinHandle&& other) noexcept : hnd{ other.hnd } {
        other.hnd = INVALID_HANDLE_VALUE;
    }

    WinHandle& operator=(WinHandle&& other) noexcept {
        if (this != &other) {
            close();

            hnd = other.hnd;
            other.hnd = INVALID_HANDLE_VALUE;
        }

        return *this;
    }

    HANDLE get() const noexcept {
        return hnd;
    }

    void set(HANDLE _hnd) {
        close();
        hnd = _hnd;
    }

    void close() {
        if (hnd != INVALID_HANDLE_VALUE) {
            CloseHandle(hnd);
        }
    }
};

class VolumeData {
    WinHandle handle;
    std::wstring volume;
    std::vector<PathInfo> database;   // for this case, a vector takes less memory, and much faster than map.
    bool _valid;

    std::error_code get_last_system_error_code() {
        return std::error_code{ (int)GetLastError(), std::system_category() };
    }

    HANDLE open_volume() {
        std::wstring temp = L"\\\\.\\" + volume;
        HANDLE hnd = CreateFileW(temp.c_str(),
                                GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr,
                                OPEN_EXISTING,
                                0,
                                nullptr);

        if (hnd == INVALID_HANDLE_VALUE) {
            auto ec = get_last_system_error_code();
            fprintf(stderr, "%d: sys call failed, %d, %s\n", __LINE__, ec.value(), ec.message().c_str());
        }

        return hnd;
    }

    bool create_usn_journal() {
        CREATE_USN_JOURNAL_DATA cujd{};
        DWORD bytes_returned;

        bool result = DeviceIoControl(handle.get(),
            FSCTL_CREATE_USN_JOURNAL,
            &cujd,
            sizeof(cujd),
            nullptr,
            0,
            &bytes_returned,
            nullptr);

        if (!result) {
            auto ec = get_last_system_error_code();
            fprintf(stderr, "%d: sys call failed, %d, %s\n", __LINE__, ec.value(), ec.message().c_str());
        }

        return result;
    }

    bool query_usn_journal(USN_JOURNAL_DATA& ujd) {
        DWORD bytes_returned = 0;
        bool result = DeviceIoControl(handle.get(),
            FSCTL_QUERY_USN_JOURNAL,
            nullptr,
            0,
            &ujd,
            sizeof(ujd),
            &bytes_returned,
            nullptr);

        if (!result) {
            auto ec = get_last_system_error_code();
            fprintf(stderr, "%d: sys call failed, %d, %s\n", __LINE__, ec.value(), ec.message().c_str());
        }

        return result;
    }

    bool enum_usn_data(USN_JOURNAL_DATA& ujd) {
        const uint32_t buf_size = 1024 * 1024;
        auto buf = std::make_unique<char[]>(buf_size);

#ifdef _MSC_VER
        MFT_ENUM_DATA_V0 med = { 0, 0, ujd.NextUsn };
#else
        MFT_ENUM_DATA med = { 0, 0, ujd.NextUsn };
#endif

        DWORD bytes_returned = 0;

        while (true) {
            bool result = DeviceIoControl(handle.get(),
                                        FSCTL_ENUM_USN_DATA,
                                        (LPVOID)&med,
                                        (DWORD)sizeof(med),
                                        (LPVOID)buf.get(),
                                        buf_size,
                                        (LPDWORD)&bytes_returned,
                                        nullptr);

            if (!result) {
                DWORD err = GetLastError();

                if (err == ERROR_HANDLE_EOF) {
                    break;
                }
                else {
                    auto ec = get_last_system_error_code();
                    fprintf(stderr, "%d: sys call failed, %d, %s\n", __LINE__, ec.value(), ec.message().c_str());
                    return false;
                }
            }

            bytes_returned -= sizeof(USN);
            PUSN_RECORD rec = (PUSN_RECORD)((PCHAR)buf.get() + sizeof(USN));

            while (bytes_returned > 0) {
                std::wstring name(rec->FileName, rec->FileNameLength / 2);
                database.emplace_back(rec->FileReferenceNumber, rec->ParentFileReferenceNumber, name);

                bytes_returned -= rec->RecordLength;
                rec = (PUSN_RECORD)((PCHAR)rec + rec->RecordLength);
            }

            med.StartFileReferenceNumber = *(USN*)(&buf[0]);
        }

        // after the collection, just sort them, then we could use binary search.
        std::sort(database.begin(), database.end(), [](const PathInfo& left, const PathInfo& right) -> bool {
            return left.frn < right.frn;
        });

        return true;
    }

    std::vector<PathInfo>::iterator database_binary_search(DWORDLONG target) {
        auto iter = std::lower_bound(database.begin(), database.end(), target, [](const PathInfo& info, DWORDLONG frn) {
            return info.frn < frn;
        });

        if (iter != database.end() && iter->frn == target) {
            return iter;
        }
        else {
            return database.end();
        }
    }

    std::wstring get_absolute_name_by_reference_number(DWORDLONG number) {
        auto iter = database_binary_search(number);

        if (iter == database.end()) {
            return L"";
        }
        else {
            DWORDLONG parentNumber = iter->pfrn;
            std::wstring temp = iter->name;

            // concatenate the complete path.
            while ((iter = database_binary_search(parentNumber)) != database.end()) {
                temp = iter->name + L"\\" + temp;
                parentNumber = iter->pfrn;
            }

            return volume + L"\\" + temp;
        }
    }
public:
    VolumeData() : volume{ L"" }, database{}, _valid{ false } {
        database.reserve(65536);
    }

    bool build_databse(const std::wstring& _volume) {
        volume = _volume;

        HANDLE hnd = open_volume();
        if (hnd == INVALID_HANDLE_VALUE) {
            return false;
        }

        handle.set(hnd);

        if (!create_usn_journal()) {
            return false;
        }

        USN_JOURNAL_DATA ujd;

        if (!query_usn_journal(ujd)) {
            return false;
        }

        if (!enum_usn_data(ujd)) {
            return false;
        }

        handle.close();

        _valid = true;
        return _valid;
    }

    bool is_valid() const noexcept {
        return _valid;
    }

    std::vector<std::wstring> query(const std::wstring& pattern) noexcept {
        std::vector<std::wstring> result;

        if (!is_valid()) {
            return result;
        }

        for (const auto& entry : database) {
            if (entry.name.find(pattern) != std::wstring::npos) {
                result.emplace_back(get_absolute_name_by_reference_number(entry.frn));
            }
        }

        return result;
    }

    std::vector<std::wstring> query_ignore_case(const std::wstring& pattern) noexcept {
        std::vector<std::wstring> result;

        if (!is_valid()) {
            return result;
        }

        auto ignoreCaseLambda = [](wchar_t a, wchar_t b) -> bool {
                return std::tolower(a) == std::tolower(b);
                };

        for (const auto& entry : database) {
            const auto& name = entry.name;
            auto searchRet = std::search(name.cbegin(), name.cend(), pattern.cbegin(), pattern.cend(), ignoreCaseLambda);

            if (searchRet != name.cend()) {
                result.emplace_back(get_absolute_name_by_reference_number(entry.frn));
            }
        }

        return result;
    }
};

bool is_ntfs(const std::wstring& path) {
    wchar_t buf[MAX_PATH];
    std::wstring temp = path + L"\\";

    bool result = GetVolumeInformationW(temp.c_str(), nullptr, 0, nullptr, nullptr, nullptr, buf, MAX_PATH);
    return result && wcscmp(buf, L"NTFS") == 0;
}

bool is_running_as_admin() {
    BOOL isAdmin = false;
    PSID adminGroup = nullptr;

    SID_IDENTIFIER_AUTHORITY authority = SECURITY_NT_AUTHORITY;
    if (!AllocateAndInitializeSid(&authority, 2, 
                                    SECURITY_BUILTIN_DOMAIN_RID, 
                                    DOMAIN_ALIAS_RID_ADMINS,
                                    0, 0, 0, 0, 0, 0,
                                    &adminGroup)) 
    {
        return false;
    }

    if (!CheckTokenMembership(nullptr, adminGroup, &isAdmin)) {
        isAdmin = false;
    }

    if (adminGroup) {
        FreeSid(adminGroup);
    }

    return isAdmin;
}

bool start_as_admin() {
    wchar_t modulePath[MAX_PATH];
    
    if (GetModuleFileNameW(nullptr, modulePath, MAX_PATH) == 0) {
        return false;
    }
    
    SHELLEXECUTEINFOW shellInfo = { 0 };
    shellInfo.cbSize = sizeof(SHELLEXECUTEINFOW);
    shellInfo.lpVerb = L"runas";
    shellInfo.lpFile = modulePath;
    shellInfo.nShow = SW_SHOWNORMAL;
    shellInfo.fMask = SEE_MASK_NOCLOSEPROCESS;
    
    if (ShellExecuteExW(&shellInfo)) {
        CloseHandle(shellInfo.hProcess);
        return true;
    }

    return false;
}

std::string unicode_to_ascii(const std::wstring& wstr) {
    int len = WideCharToMultiByte(CP_ACP, 0, wstr.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (len == 0) {
        return "";
    }

    auto buf = std::make_unique<char[]>(len);
    WideCharToMultiByte(CP_ACP, 0, wstr.c_str(), -1, buf.get(), len, nullptr, nullptr);
    return std::string(buf.get());
}

void search_one_volume(const std::wstring& volume, const std::wstring& pattern, bool ignoreCase) {
    if (!is_ntfs(volume)) {
        fprintf(stderr, "volume: %s is not NTFS\n", unicode_to_ascii(volume).c_str());
        return;
    }

    VolumeData vd;
    if (!vd.build_databse(volume)) {
        fprintf(stderr, "volume: %s build database failed\n", unicode_to_ascii(volume).c_str());
        return;
    }

    std::vector<std::wstring> result;

    if (ignoreCase) {
        result = vd.query_ignore_case(pattern);
    }
    else {
        result = vd.query(pattern);
    }

    for (const std::wstring& wstr : result) {
        printf("%s\n", unicode_to_ascii(wstr).c_str());
    }
}

int main() {
    if (!is_running_as_admin()) {
        if (!start_as_admin()) {
            fprintf(stderr, "not admin, cannot execute!\n");
            return 1;
        }

        // old process just exit.
        return 0;
    }

    search_one_volume(L"C:", L"芬妮", true);
    search_one_volume(L"D:", L"爱莉", true);
    system("pause");
    return 0;
}
