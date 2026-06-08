#pragma once
#include <switch.h>

// Deletes all files and directories under the root of save_fs.
// Returns false if the directory enumeration itself fails (I/O error),
// or if any deletion fails. True only when the root is fully cleared.
bool clear_save_root(FsFileSystem* save_fs);
