#include "noisekiller/profile_store.hpp"
#ifdef _WIN32
#include <windows.h>

#include <aclapi.h>
#include <array>
#include <cstring>
#include <dpapi.h>
#include <limits>
#include <vector>

namespace nk {
namespace win {
namespace {
constexpr std::array<unsigned char, 8> Magic{{'N', 'K', 'P', 'R', 'O', 'F', '1', 0}};
constexpr std::size_t MaximumFile = 1024 * 1024;
std::string failure(const char *operation) {
    return std::string(operation) + " (Windows " + std::to_string(GetLastError()) + ")";
}
struct Wipe {
    std::vector<unsigned char> &bytes;
    ~Wipe() {
        if (!bytes.empty())
            SecureZeroMemory(bytes.data(), bytes.size());
    }
};
void u32(std::vector<unsigned char> &b, std::uint32_t v) {
    for (unsigned i = 0; i < 4; ++i)
        b.push_back(static_cast<unsigned char>(v >> (8 * i)));
}
void u64(std::vector<unsigned char> &b, std::uint64_t v) {
    for (unsigned i = 0; i < 8; ++i)
        b.push_back(static_cast<unsigned char>(v >> (8 * i)));
}
void f32(std::vector<unsigned char> &b, float f) {
    std::uint32_t v = 0;
    std::memcpy(&v, &f, 4);
    u32(b, v);
}
struct Reader {
    const unsigned char *b;
    std::size_t size, pos = 0;
    bool good = true;
    std::uint32_t get32() {
        if (pos > size || size - pos < 4) {
            good = false;
            return 0;
        }
        std::uint32_t v = 0;
        for (unsigned i = 0; i < 4; ++i)
            v |= std::uint32_t(b[pos++]) << (8 * i);
        return v;
    }
    std::uint64_t get64() {
        const auto lo = get32();
        const auto hi = get32();
        return lo | (std::uint64_t(hi) << 32);
    }
    float getfloat() {
        const auto v = get32();
        float f = 0;
        std::memcpy(&f, &v, 4);
        return f;
    }
};
// Restrict newly written blobs to this user and SYSTEM; encryption is current-
// user DPAPI, never CRYPTPROTECT_LOCAL_MACHINE and never a plaintext fallback.
struct PrivateSecurity {
    HANDLE token = nullptr;
    std::vector<unsigned char> token_info;
    PACL acl = nullptr;
    SECURITY_DESCRIPTOR descriptor{};
    SECURITY_ATTRIBUTES attributes{};
    ~PrivateSecurity() {
        if (acl)
            LocalFree(acl);
        if (token)
            CloseHandle(token);
    }
    bool initialize(std::string &error) {
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
            error = failure("OpenProcessToken");
            return false;
        }
        DWORD size = 0;
        GetTokenInformation(token, TokenUser, nullptr, 0, &size);
        token_info.resize(size);
        if (size == 0 || !GetTokenInformation(token, TokenUser, token_info.data(), size, &size)) {
            error = failure("GetTokenInformation");
            return false;
        }
        unsigned char system_buffer[SECURITY_MAX_SID_SIZE]{};
        DWORD system_size = sizeof(system_buffer);
        if (!CreateWellKnownSid(WinLocalSystemSid, nullptr, system_buffer, &system_size)) {
            error = failure("CreateWellKnownSid");
            return false;
        }
        EXPLICIT_ACCESSW entries[2]{};
        for (auto &e : entries) {
            e.grfAccessPermissions = GENERIC_ALL;
            e.grfAccessMode = SET_ACCESS;
            e.grfInheritance = NO_INHERITANCE;
            e.Trustee.TrusteeForm = TRUSTEE_IS_SID;
            e.Trustee.TrusteeType = TRUSTEE_IS_USER;
        }
        entries[0].Trustee.ptstrName =
            reinterpret_cast<LPWSTR>(reinterpret_cast<TOKEN_USER *>(token_info.data())->User.Sid);
        entries[1].Trustee.ptstrName = reinterpret_cast<LPWSTR>(system_buffer);
        DWORD result = SetEntriesInAclW(2, entries, nullptr, &acl);
        if (result != ERROR_SUCCESS) {
            SetLastError(result);
            error = failure("SetEntriesInAcl");
            return false;
        }
        if (!InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION) ||
            !SetSecurityDescriptorDacl(&descriptor, TRUE, acl, FALSE) ||
            !SetSecurityDescriptorControl(&descriptor, SE_DACL_PROTECTED, SE_DACL_PROTECTED)) {
            error = failure("Private DACL");
            return false;
        }
        attributes.nLength = sizeof(attributes);
        attributes.lpSecurityDescriptor = &descriptor;
        attributes.bInheritHandle = FALSE;
        return true;
    }
};
} // namespace
bool save_profile(const std::wstring &path, const Profile &profile, std::string &error) {
    error.clear();
    if (path.empty() || !profile.valid()) {
        error = "Invalid profile or empty path";
        return false;
    }
    std::vector<unsigned char> plain;
    Wipe wipe{plain};
    plain.reserve(16384);
    plain.insert(plain.end(), Magic.begin(), Magic.end());
    u32(plain, 1);
    u32(plain, SampleRate);
    u32(plain, Hop);
    u32(plain, Window);
    u32(plain, FftSize);
    u32(plain, Bins);
    u32(plain, Features);
    u32(plain, MaxPrototypes);
    u32(plain, profile.schema);
    u32(plain, profile.feature_schema);
    u32(plain, profile.prototype_count);
    u32(plain, profile.accepted_frames);
    u64(plain, profile.anchor_hash);
    f32(plain, profile.accepted_seconds);
    for (const auto &p : profile.prototypes) {
        u32(plain, p.voiced ? 1 : 0);
        for (float f : p.power_basis)
            f32(plain, f);
        for (float f : p.mean)
            f32(plain, f);
        for (float f : p.variance)
            f32(plain, f);
    }
    DATA_BLOB in{static_cast<DWORD>(plain.size()), plain.data()}, encrypted{};
    if (!CryptProtectData(&in, L"NoiseKiller acoustic profile v1", nullptr, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &encrypted)) {
        error = failure("CryptProtectData");
        return false;
    }
    const auto cleanup = [&]() {
        if (encrypted.pbData) {
            SecureZeroMemory(encrypted.pbData, encrypted.cbData);
            LocalFree(encrypted.pbData);
            encrypted.pbData = nullptr;
        }
    };
    PrivateSecurity security;
    if (!security.initialize(error)) {
        cleanup();
        return false;
    }
    const std::wstring temporary =
        path + L".tmp." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(GetTickCount64());
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, &security.attributes, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = failure("Create encrypted profile");
        cleanup();
        return false;
    }
    DWORD written = 0;
    bool ok = WriteFile(file, encrypted.pbData, encrypted.cbData, &written, nullptr) != FALSE &&
              written == encrypted.cbData;
    if (ok)
        ok = FlushFileBuffers(file) != FALSE;
    if (!ok)
        error = failure("Write encrypted profile");
    CloseHandle(file);
    cleanup();
    if (ok &&
        !MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = failure("Replace encrypted profile");
        ok = false;
    }
    if (!ok)
        DeleteFileW(temporary.c_str());
    return ok;
}
bool load_profile(const std::wstring &path, Profile &profile, std::string &error) {
    error.clear();
    profile = Profile{};
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = failure("Open encrypted profile");
        return false;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > MaximumFile) {
        error = "Profile file size is invalid";
        CloseHandle(file);
        return false;
    }
    std::vector<unsigned char> cipher(static_cast<std::size_t>(size.QuadPart));
    DWORD read = 0;
    const bool read_ok =
        ReadFile(file, cipher.data(), static_cast<DWORD>(cipher.size()), &read, nullptr) != FALSE &&
        read == cipher.size();
    CloseHandle(file);
    if (!read_ok) {
        error = failure("Read encrypted profile");
        return false;
    }
    DATA_BLOB encrypted{static_cast<DWORD>(cipher.size()), cipher.data()}, plain{};
    if (!CryptUnprotectData(&encrypted, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN,
                            &plain)) {
        error = failure("CryptUnprotectData (wrong user, damaged or unencrypted file)");
        return false;
    }
    auto cleanup = [&]() {
        if (plain.pbData) {
            SecureZeroMemory(plain.pbData, plain.cbData);
            LocalFree(plain.pbData);
            plain.pbData = nullptr;
        }
    };
    if (plain.cbData < Magic.size() || std::memcmp(plain.pbData, Magic.data(), Magic.size()) != 0) {
        error = "Incompatible profile magic";
        cleanup();
        return false;
    }
    Reader r{plain.pbData, plain.cbData, Magic.size()};
    if (r.get32() != 1 || r.get32() != SampleRate || r.get32() != Hop || r.get32() != Window ||
        r.get32() != FftSize || r.get32() != Bins || r.get32() != Features || r.get32() != MaxPrototypes) {
        error = "Incompatible profile serialization or analysis configuration; re-enroll";
        cleanup();
        return false;
    }
    Profile candidate{};
    candidate.schema = r.get32();
    candidate.feature_schema = r.get32();
    candidate.prototype_count = r.get32();
    candidate.accepted_frames = r.get32();
    candidate.anchor_hash = r.get64();
    candidate.accepted_seconds = r.getfloat();
    for (auto &p : candidate.prototypes) {
        const auto voiced = r.get32();
        if (voiced > 1)
            r.good = false;
        p.voiced = voiced != 0;
        for (float &f : p.power_basis)
            f = r.getfloat();
        for (float &f : p.mean)
            f = r.getfloat();
        for (float &f : p.variance)
            f = r.getfloat();
    }
    const bool ok = r.good && r.pos == r.size && candidate.valid();
    cleanup();
    if (!ok) {
        SecureZeroMemory(&candidate, sizeof(candidate));
        error = "Invalid or incompatible acoustic profile; re-enroll";
        return false;
    }
    profile = candidate;
    SecureZeroMemory(&candidate, sizeof(candidate));
    return true;
}
bool delete_profile(const std::wstring &path, std::string &error) {
    error.clear();
    if (path.empty()) {
        error = "Empty profile path";
        return false;
    }
    if (DeleteFileW(path.c_str()))
        return true;
    const DWORD code = GetLastError();
    if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND)
        return true;
    error = failure("Delete profile");
    return false;
}
} // namespace win
} // namespace nk
#endif
