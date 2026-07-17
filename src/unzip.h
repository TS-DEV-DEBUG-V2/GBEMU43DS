#pragma once
#include "gb_types.h"
#include <vector>
#include <string>

bool unzip_extract_rom(const std::string& zip_path, std::vector<u8>& out);

bool path_is_zip(const std::string& path);
