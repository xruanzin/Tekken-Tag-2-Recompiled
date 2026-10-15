#pragma once

#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>

namespace cafe::host {

std::filesystem::path executable_path();
std::filesystem::path home_directory();
std::filesystem::path config_directory();
std::filesystem::path data_directory();
std::filesystem::path cache_directory();
std::string path_string(const std::filesystem::path& path);
bool is_terminal(FILE* stream);
void set_thread_name(std::string_view name);

} // namespace cafe::host
