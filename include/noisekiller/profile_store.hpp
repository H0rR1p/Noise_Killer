#pragma once
#include "noisekiller/core.hpp"
#include <string>

namespace nk { namespace win {
bool save_profile(const std::wstring& path, const Profile& profile, std::string& error);
bool load_profile(const std::wstring& path, Profile& profile, std::string& error);
bool delete_profile(const std::wstring& path, std::string& error);
} }
