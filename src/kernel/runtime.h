#pragma once
#include <filesystem>

// Starts the runtime services the title expects from the console: the
// timestamp bundle, the file system roots, the fake GPU and audio clocks.
void RuntimeInit(const std::filesystem::path& gameDir);

const std::filesystem::path& GameDir();
const std::filesystem::path& UserDir();
// The signed-in profile's saves (\Device\Content).
const std::filesystem::path& ContentDir();
// Downloadable content shared by every profile (\Device\Dlc; one folder per package).
const std::filesystem::path& DlcDir();
// A DLC package's folder that holds Media (some sit one folder deeper) (imports_xam.cpp).
std::filesystem::path DlcPackageRoot(const std::filesystem::path& package);
