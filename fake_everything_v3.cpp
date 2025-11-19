/*
    @author yuan

    This program is used to search the files on your disk, it uses USN_JOURNAL_DATA just like the famous everything software.
    A C++17 compiler is needed to compile this program.
*/
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <shellapi.h>
#include <winioctl.h>
#include <algorithm>
#include <iostream>
#include <memory>
#include <string>
#include <system_error>
#include <functional>
#include <utility>
#include <vector>
#include <map>
#include <cstdlib>
#include <cstdint>
#include <cctype>

// encoding conversion, only for windows platform.
namespace win_encoding {
    std::string unicode_to_ascii(const std::wstring& wstr) {
        int len = WideCharToMultiByte(CP_ACP, 0, wstr.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (len == 0) {
            throw std::system_error(GetLastError(), std::system_category(), "unicode to ascii failed");
        }

        auto buf = std::make_unique<char[]>(len);

        len = WideCharToMultiByte(CP_ACP, 0, wstr.c_str(), -1, buf.get(), len, nullptr, nullptr);
        if (len == 0) {
            throw std::system_error(GetLastError(), std::system_category(), "unicode to ascii failed");
        }

        return std::string(buf.get());
    }
};

using PathDatabase = std::map<DWORDLONG, std::pair<std::wstring, DWORDLONG>>;
using SearchCallback = std::function<void(const std::wstring&)>;

class PathDataBuilder {
    HANDLE handle;
    USN_JOURNAL_DATA ujd;
public:
    PathDataBuilder(const std::wstring& volume) {
        std::wstring temp = L"\\\\.\\" + volume;
        handle = CreateFileW(temp.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_EXISTING,
            0,
            nullptr);

        if (handle == INVALID_HANDLE_VALUE) {
            throw std::system_error(GetLastError(), std::system_category(), "CreateFileW failed");
        }

        // create ujd.
        USN_JOURNAL_DATA ujd;
        CREATE_USN_JOURNAL_DATA cujd;
        DWORD bytes_returned;

        bool result = DeviceIoControl(handle,
            FSCTL_CREATE_USN_JOURNAL,
            &cujd,
            sizeof(cujd),
            nullptr,
            0,
            &bytes_returned,
            nullptr);

        if (!result) {
            auto err = GetLastError();
            CloseHandle(handle);
            handle = INVALID_HANDLE_VALUE;
            throw std::system_error(err, std::system_category(), "DeviceIoControl CREATE failed");
        }
    }

    ~PathDataBuilder() {
        if (handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
        }

        DELETE_USN_JOURNAL_DATA dujd = { ujd.UsnJournalID, USN_DELETE_FLAG_DELETE };
        DWORD bytes_returned;

        DeviceIoControl(handle,
            FSCTL_DELETE_USN_JOURNAL,
            &dujd,
            sizeof(dujd),
            nullptr,
            0,
            &bytes_returned,
            nullptr);
    }

    void build(PathDatabase& db) {
        const uint32_t bufSize = 1024 * 1024;
        auto buf = std::make_unique<char[]>(bufSize);

#ifdef _MSC_VER
        MFT_ENUM_DATA_V0 med = { 0, 0, ujd.NextUsn };
#else
        MFT_ENUM_DATA med = { 0, 0, ujd.NextUsn };
#endif

        // begin query.
        DWORD bytes_returned = 0;
        bool result;

        result = DeviceIoControl(handle,
            FSCTL_QUERY_USN_JOURNAL,
            nullptr,
            0,
            &ujd,
            sizeof(ujd),
            &bytes_returned,
            nullptr);

        if (!result) {
            throw std::system_error(GetLastError(), std::system_category(), "DeviceIoControl QUERY failed");
        }

        // fill db.
        while (true) {
            bool result = DeviceIoControl(handle,
                FSCTL_ENUM_USN_DATA,
                (LPVOID)&med,
                (DWORD)sizeof(med),
                (LPVOID)buf.get(),
                bufSize,
                (LPDWORD)&bytes_returned,
                nullptr);

            if (!result) {
                DWORD err = GetLastError();

                if (err == ERROR_HANDLE_EOF) {
                    return;
                }
                else {
                    throw std::system_error(err, std::system_category(), "DeviceIoControl ENUM failed");
                }
            }

            bytes_returned -= sizeof(USN);
            PUSN_RECORD rec = (PUSN_RECORD)((PCHAR)buf.get() + sizeof(USN));

            while (bytes_returned > 0) {
                DWORDLONG fileReferenceNumber = rec->FileReferenceNumber;
                DWORDLONG parentFileReferenceNumber = rec->ParentFileReferenceNumber;
                std::wstring name(rec->FileName, rec->FileNameLength / 2);

                db.emplace(fileReferenceNumber, std::make_pair(name, parentFileReferenceNumber));
                bytes_returned -= rec->RecordLength;
                rec = (PUSN_RECORD)((PCHAR)rec + rec->RecordLength);
            }

            med.StartFileReferenceNumber = *(USN*)(&buf[0]);
        }
    }
};

class SearchEngine {
    std::wstring volume;
    PathDatabase db;

    std::wstring get_absolute_name_by_reference_number(DWORDLONG number) noexcept {
        DWORDLONG parentNumber = db[number].second;
        std::wstring temp = db[number].first;

        // concatenate the complete path.
        while (db.count(parentNumber) > 0) {
            temp = db[parentNumber].first + L"\\" + temp;
            parentNumber = db[parentNumber].second;
        }

        return volume + L"\\" + temp;
    }
public:
    SearchEngine(const std::wstring& _volume) : volume { _volume } {
        PathDataBuilder builder{ volume };
        builder.build(db);
    }

    void search(const std::wstring& pattern, bool ignoreCase, SearchCallback callback) noexcept {
        if (ignoreCase) {
            auto searchLambda = [](wchar_t a, wchar_t b) -> bool {
                return std::tolower(a) == std::tolower(b);
                };

            for (const auto& entry : db) {
                auto& [name, number] = entry.second;
                auto searchResult = std::search(name.begin(), name.end(), pattern.begin(), pattern.end(), searchLambda);

                if (searchResult != name.end()) {
                    callback(get_absolute_name_by_reference_number(entry.first));
                }
            }
        }
        else {
            for (const auto& entry : db) {
                auto& [name, number] = entry.second;

                if (name.find(pattern) != std::wstring::npos) {
                    callback(get_absolute_name_by_reference_number(entry.first));
                }
            }
        }
    }
};

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
    GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
    
    SHELLEXECUTEINFOW shellInfo = { 0 };
    shellInfo.cbSize = sizeof(SHELLEXECUTEINFOW);
    shellInfo.lpVerb = L"runas";
    shellInfo.lpFile = modulePath;
    shellInfo.nShow = SW_SHOWNORMAL;
    shellInfo.fMask = SEE_MASK_NOCLOSEPROCESS;
    
    if (ShellExecuteExW(&shellInfo)) {
        WaitForSingleObject(shellInfo.hProcess, 2000);
        CloseHandle(shellInfo.hProcess);
        return true;
    }

    return false;
}

bool is_ntfs(const std::wstring& volume) {
    wchar_t buf[MAX_PATH];
    std::wstring temp = volume + L"\\";

    bool result = GetVolumeInformationW(temp.c_str(), nullptr, 0, nullptr, nullptr, nullptr, buf, MAX_PATH);
    return result && wcscmp(buf, L"NTFS") == 0;
}

void search_one_volume(const std::wstring& volume, const std::wstring& pattern, bool ignoreCase) {
    if (!is_ntfs(volume)) {
        std::cerr << win_encoding::unicode_to_ascii(volume) << " is not NTFS\n";
        return;
    }

    try {
        SearchEngine se{ volume };

        se.search(pattern, ignoreCase, [](const std::wstring& wstr){
            std::cout << win_encoding::unicode_to_ascii(wstr) << "\n";
        });
    }
    catch(const std::system_error& e) {
        std::cerr << win_encoding::unicode_to_ascii(volume) << ", " << e.code() << ", " << e.what() << "\n";
    }
}

int main() {
    if (!is_running_as_admin()) {
        if (!start_as_admin()) {
            std::cerr << "not admin, cannot execute!\n";
            return 1;
        }

        // old process just exit.
        return 0;
    }

    search_one_volume(L"C:", L"miku", true);
    search_one_volume(L"D:", L"miku", true);
    system("pause");
    return 0;
}
