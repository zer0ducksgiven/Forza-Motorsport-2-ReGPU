#pragma once
#include <filesystem>
#include <string>

// Guest path ("game:\media\x.bin", "\Device\Cdrom0\...", symbolic links)
// to host path. readOnly is set for the game folder.
bool VfsResolve(std::string guest, std::filesystem::path& host, bool* readOnly);
// Mods' loose files: Documents\fm2\mods\<mod>\game\<path> (or \update\<path>) replaces or adds the game's
// (or the title update's) file at that path, e.g. mods\ForzaPlus2\game\Media\DB\gamedb.slt. Mods apply in name
// order (a later one wins); a folder starting with '_' or '.' is skipped. FM2_NO_MODS=1 turns them off.
// The mod's file for a game path ("Media\DB\gamedb.slt"), if one has it.
bool VfsModFile(const std::string& root, const std::string& rest, std::filesystem::path& host);
void VfsAddLink(const std::string& name, const std::string& target);
void VfsRemoveLink(const std::string& name);
