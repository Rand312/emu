/*
 * Copyright (C) 2010 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "make_ext4fs.h"
#include "ext4_utils.h"
#include "allocate.h"
#include "contents.h"
#include "uuid.h"
#include "wipe.h"

#include <private/android_filesystem_config.h>
#include <sparse/sparse.h>

#include <assert.h>
#include <dirent.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _MSC_VER
#include <libgen.h>
#include <unistd.h>
#endif
#include <sys/types.h>

#include "android/utils/file_io.h"
#include "android/utils/path.h"
#if defined(USE_MINGW) || defined(_MSC_VER)

#include <winsock2.h>

/* These match the Linux definitions of these flags.
   L_xx is defined to avoid conflicting with the win32 versions.
*/
#define L_S_IRUSR 00400
#define L_S_IWUSR 00200
#define L_S_IXUSR 00100
#define S_ISUID 0004000
#define S_ISGID 0002000
#define S_ISVTX 0001000
/* MinGW defines this macro as well */
#ifdef S_IRWXU
#undef S_IRWXU
#endif
#define S_IRWXU (L_S_IRUSR | L_S_IWUSR | L_S_IXUSR)
#else

#include <selinux/selinux.h>
#include <selinux/label.h>
#include <selinux/android.h>

#define O_BINARY 0

#endif

#if defined(USE_MINGW) || defined(_MSC_VER)
static int scandir_win(const char* path, struct dirent ***namelist) {
    int pathSize = strlen(path);
    int nameBufferSize = pathSize + 1 + 1;
    char* nameBuffer = malloc(nameBufferSize);
    memcpy(nameBuffer, path, pathSize);
    strcpy(nameBuffer + pathSize, "*");
    int nameBufferSizeW = MultiByteToWideChar(CP_UTF8, 0,
                                       nameBuffer, nameBufferSize,
                                       NULL, 0);
    wchar_t* nameBufferW = calloc(nameBufferSizeW, sizeof(wchar_t));
    MultiByteToWideChar(CP_UTF8, 0,
                        nameBuffer, nameBufferSize,
                        nameBufferW, nameBufferSizeW);
    HANDLE hndl;
    WIN32_FIND_DATAW fileData;
    hndl = FindFirstFileW(nameBufferW, &fileData);
    if (hndl == INVALID_HANDLE_VALUE) {
        int err = GetLastError();
        free(nameBuffer);
        free(nameBufferW);
        if (err == ERROR_NO_MORE_FILES) {
            return 0;
        } else {
            return ENOENT;
        }
    }
    int capacity = 4;
    *namelist = malloc(capacity * sizeof(struct dirent *));
    int idx = 0;
    do {
        if (wcscmp(L".", fileData.cFileName) != 0
                && wcscmp(L"..", fileData.cFileName) != 0) {
            if (idx >= capacity) {
                capacity *= 2;
                *namelist = realloc(*namelist,
                                capacity * sizeof(struct dirent *));
            }
            (*namelist)[idx] = malloc(sizeof(struct dirent));
            assert(wcslen(fileData.cFileName) < MAX_PATH);
            WideCharToMultiByte(CP_UTF8, 0,
                                fileData.cFileName, -1,
                                (*namelist)[idx]->d_name, MAX_PATH,
                                NULL, NULL);
            idx ++;
        }
    } while (FindNextFileW(hndl, &fileData));
    FindClose(hndl);
    free(nameBuffer);
    free(nameBufferW);
    return idx;
}
#endif

/* TODO: Not implemented:
   Allocating blocks in the same block group as the file inode
   Hash or binary tree directories
   Special files: sockets, devices, fifos
 */

static u32 build_default_directory_structure(const char *dir_path,
					     struct selabel_handle *sehnd)
{
	u32 inode;
	u32 root_inode;
	struct dentry dentries = {
			.filename = "lost+found",
			.file_type = EXT4_FT_DIR,
			.mode = S_IRWXU,
			.uid = 0,
			.gid = 0,
			.mtime = 0,
	};
	root_inode = make_directory(0, 1, &dentries, 1);
	inode = make_directory(root_inode, 0, NULL, 0);
	*dentries.inode = inode;
	inode_set_permissions(inode, dentries.mode,
		dentries.uid, dentries.gid, dentries.mtime);

#if !defined(USE_MINGW) && !defined(_MSC_VER)
	if (sehnd) {
		char *path = NULL;
		char *secontext = NULL;

		asprintf(&path, "%slost+found", dir_path);
		if (selabel_lookup(sehnd, &secontext, path, S_IFDIR) < 0) {
			error("cannot lookup security context for %s", path);
		} else {
			inode_set_selinux(inode, secontext);
			freecon(secontext);
		}
		free(path);
	}
#endif

	return root_inode;
}

static int filter_dot(const struct dirent *d)
{
    return (strcmp(d->d_name, "..") && strcmp(d->d_name, "."));
}

/* Read a local directory and create the same tree in the generated filesystem.
   Calls itself recursively with each directory in the given directory.
   full_path is an absolute or relative path, with a trailing slash, to the
   directory on disk that should be copied, or NULL if this is a directory
   that does not exist on disk (e.g. lost+found).
   dir_path is an absolute path, with trailing slash, to the same directory
   if the image were mounted at the specified mount point */
static u32 build_directory_structure(const char *full_path, const char *dir_path,
		u32 dir_inode, fs_config_func_t fs_config_func,
		struct selabel_handle *sehnd, int verbose)
{
	int entries = 0;
	struct dentry *dentries;
	struct dirent **namelist = NULL;
	struct stat _stat;
	int ret;
	int i;
	u32 inode;
	u32 entry_inode;
	u32 dirs = 0;
#if !defined(USE_MINGW) && !defined(_MSC_VER)
	bool needs_lost_and_found = false;
#endif

	if (full_path) {
#if defined(USE_MINGW) || defined(_MSC_VER)
            entries = scandir_win(full_path, &namelist);
#else
            entries = scandir(full_path, &namelist, filter_dot, (void*)alphasort);
#endif
            if (entries < 0) {
                error_errno("scandir");
                return EXT4_ALLOCATE_FAILED;
            }
	}

#if !defined(USE_MINGW) && !defined(_MSC_VER)
	if (dir_inode == 0) {
		/* root directory, check if lost+found already exists */
		for (i = 0; i < entries; i++)
			if (strcmp(namelist[i]->d_name, "lost+found") == 0)
				break;
		if (i == entries)
			needs_lost_and_found = true;
	}
#endif

	dentries = calloc(entries, sizeof(struct dentry));
	if (dentries == NULL)
		critical_error_errno("malloc");

	for (i = 0; i < entries; i++) {
		dentries[i].filename = strdup(namelist[i]->d_name);
		if (dentries[i].filename == NULL)
			critical_error_errno("strdup");

		asprintf(&dentries[i].path, "%s%s", dir_path, namelist[i]->d_name);
		asprintf(&dentries[i].full_path, "%s%s", full_path, namelist[i]->d_name);

		free(namelist[i]);
		ret = android_lstat(dentries[i].full_path, &_stat);
		if (ret < 0) {
			error_errno("lstat");
			i--;
			entries--;
			continue;
		}

		dentries[i].size = _stat.st_size;
#ifdef _MSC_VER
		dentries[i].mode = _stat.st_mode & (S_ISUID|S_ISGID|S_ISVTX|S_IRWXU);
#else
		dentries[i].mode = _stat.st_mode & (S_ISUID|S_ISGID|S_ISVTX|S_IRWXU|S_IRWXG|S_IRWXO);
#endif
		dentries[i].mtime = _stat.st_mtime;
		if (fs_config_func != NULL) {
			uint64_t capabilities;
			unsigned int mode = 0;
			unsigned int uid = 0;
			unsigned int gid = 0;
			int dir = S_ISDIR(_stat.st_mode);
			fs_config_func(dentries[i].path, dir, &uid, &gid, &mode, &capabilities);
			dentries[i].mode = mode;
			dentries[i].uid = uid;
			dentries[i].gid = gid;
			dentries[i].capabilities = capabilities;
		}
#if !defined(USE_MINGW) && !defined(_MSC_VER)
		if (sehnd) {
			if (selabel_lookup(sehnd, &dentries[i].secon, dentries[i].path, _stat.st_mode) < 0) {
				error("cannot lookup security context for %s", dentries[i].path);
			}

			if (dentries[i].secon && verbose)
				printf("Labeling %s as %s\n", dentries[i].path, dentries[i].secon);
		}
#endif

		if (S_ISREG(_stat.st_mode)) {
			dentries[i].file_type = EXT4_FT_REG_FILE;
		} else if (S_ISDIR(_stat.st_mode)) {
			dentries[i].file_type = EXT4_FT_DIR;
			dirs++;
		} else if (S_ISCHR(_stat.st_mode)) {
			dentries[i].file_type = EXT4_FT_CHRDEV;
		} else if (S_ISBLK(_stat.st_mode)) {
			dentries[i].file_type = EXT4_FT_BLKDEV;
		} else if (S_ISFIFO(_stat.st_mode)) {
			dentries[i].file_type = EXT4_FT_FIFO;
		}
#if !defined(USE_MINGW) && !defined(_MSC_VER)
                else if (S_ISSOCK(_stat.st_mode)) {
			dentries[i].file_type = EXT4_FT_SOCK;
		} else if (S_ISLNK(_stat.st_mode)) {
			dentries[i].file_type = EXT4_FT_SYMLINK;
			dentries[i].link = calloc(info.block_size, 1);
			readlink(dentries[i].full_path, dentries[i].link, info.block_size - 1);
		}
#endif
                else {
			error("unknown file type on %s", dentries[i].path);
			i--;
			entries--;
		}
	}
	free(namelist);

#if !defined(USE_MINGW) && !defined(_MSC_VER)
	if (needs_lost_and_found) {
		/* insert a lost+found directory at the beginning of the dentries */
		struct dentry *tmp = calloc(entries + 1, sizeof(struct dentry));
		memset(tmp, 0, sizeof(struct dentry));
		memcpy(tmp + 1, dentries, entries * sizeof(struct dentry));
		free(dentries);
		dentries = tmp;

		dentries[0].filename = strdup("lost+found");
		asprintf(&dentries[0].path, "%slost+found", dir_path);
		dentries[0].full_path = NULL;
		dentries[0].size = 0;
		dentries[0].mode = S_IRWXU;
		dentries[0].file_type = EXT4_FT_DIR;
		dentries[0].uid = 0;
		dentries[0].gid = 0;
		if (sehnd) {
			if (selabel_lookup(sehnd, &dentries[0].secon, dentries[0].path, dentries[0].mode) < 0)
				error("cannot lookup security context for %s", dentries[0].path);
		}
		entries++;
		dirs++;
	}
#endif

	inode = make_directory(dir_inode, entries, dentries, dirs);

	for (i = 0; i < entries; i++) {
		if (dentries[i].file_type == EXT4_FT_REG_FILE) {
			entry_inode = make_file(dentries[i].full_path, dentries[i].size);
		} else if (dentries[i].file_type == EXT4_FT_DIR) {
			char *subdir_full_path = NULL;
			char *subdir_dir_path;
			if (dentries[i].full_path) {
				ret = asprintf(&subdir_full_path, "%s/", dentries[i].full_path);
				if (ret < 0)
					critical_error_errno("asprintf");
			}
			ret = asprintf(&subdir_dir_path, "%s/", dentries[i].path);
			if (ret < 0)
				critical_error_errno("asprintf");
			entry_inode = build_directory_structure(subdir_full_path,
					subdir_dir_path, inode, fs_config_func, sehnd, verbose);
			free(subdir_full_path);
			free(subdir_dir_path);
		} else if (dentries[i].file_type == EXT4_FT_SYMLINK) {
			entry_inode = make_link(dentries[i].link);
		} else {
			error("unknown file type on %s", dentries[i].path);
			entry_inode = 0;
		}
		*dentries[i].inode = entry_inode;

		ret = inode_set_permissions(entry_inode, dentries[i].mode,
			dentries[i].uid, dentries[i].gid,
			dentries[i].mtime);
		if (ret)
			error("failed to set permissions on %s\n", dentries[i].path);

		/*
		 * It's important to call inode_set_selinux() before
		 * inode_set_capabilities(). Extended attributes need to
		 * be stored sorted order, and we guarantee this by making
		 * the calls in the proper order.
		 * Please see xattr_assert_sane() in contents.c
		 */
		ret = inode_set_selinux(entry_inode, dentries[i].secon);
		if (ret)
			error("failed to set SELinux context on %s\n", dentries[i].path);
		ret = inode_set_capabilities(entry_inode, dentries[i].capabilities);
		if (ret)
			error("failed to set capability on %s\n", dentries[i].path);

		free(dentries[i].path);
		free(dentries[i].full_path);
		free(dentries[i].link);
		free((void *)dentries[i].filename);
		free(dentries[i].secon);
	}

	free(dentries);
	return inode;
}

static u32 compute_block_size()
{
	return 4096;
}

static u32 compute_journal_blocks()
{
	u32 journal_blocks = DIV_ROUND_UP(info.len, info.block_size) / 64;
	if (journal_blocks < 1024)
		journal_blocks = 1024;
	if (journal_blocks > 32768)
		journal_blocks = 32768;
	return journal_blocks;
}

static u32 compute_blocks_per_group()
{
	return info.block_size * 8;
}

static u32 compute_inodes()
{
	return DIV_ROUND_UP(info.len, info.block_size) / 4;
}

static u32 compute_inodes_per_group()
{
	u32 blocks = DIV_ROUND_UP(info.len, info.block_size);
	u32 block_groups = DIV_ROUND_UP(blocks, info.blocks_per_group);
	u32 inodes = DIV_ROUND_UP(info.inodes, block_groups);
	inodes = ALIGN(inodes, (info.block_size / info.inode_size));

	/* After properly rounding up the number of inodes/group,
	 * make sure to update the total inodes field in the info struct.
	 */
	info.inodes = inodes * block_groups;

	return inodes;
}

static u32 compute_bg_desc_reserve_blocks()
{
	u32 blocks = DIV_ROUND_UP(info.len, info.block_size);
	u32 block_groups = DIV_ROUND_UP(blocks, info.blocks_per_group);
	u32 bg_desc_blocks = DIV_ROUND_UP(block_groups * sizeof(struct ext2_group_desc),
			info.block_size);

	u32 bg_desc_reserve_blocks =
			DIV_ROUND_UP(block_groups * 1024 * sizeof(struct ext2_group_desc),
					info.block_size) - bg_desc_blocks;

	if (bg_desc_reserve_blocks > info.block_size / sizeof(u32))
		bg_desc_reserve_blocks = info.block_size / sizeof(u32);

	return bg_desc_reserve_blocks;
}

void reset_ext4fs_info() {
    // Reset all the global data structures used by make_ext4fs so it
    // can be called again.
    memset(&info, 0, sizeof(info));
    memset(&aux_info, 0, sizeof(aux_info));

    if (ext4_sparse_file) {
        sparse_file_destroy(ext4_sparse_file);
        ext4_sparse_file = NULL;
    }
}

int make_ext4fs_sparse_fd(int fd, long long len,
                const char *mountpoint, struct selabel_handle *sehnd)
{
	reset_ext4fs_info();
	info.len = len;

	return make_ext4fs_internal(fd, NULL, mountpoint, fs_config, 0, 1, 0, 0, sehnd, -1);
}

int make_ext4fs(const char *filename, long long len,
                const char *mountpoint, struct selabel_handle *sehnd)
{
    return make_ext4fs_from_dir(filename, NULL, len, mountpoint, sehnd, 0);
}

int make_ext4fs_from_dir(const char *filename, const char *dirname,
                         long long len, const char *mountpoint,
                         struct selabel_handle *sehnd, int verbose) {
    int fd;
    int status;

    reset_ext4fs_info();
    info.len = len;

    fd = path_open(filename, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0644);
    if (fd < 0) {
        error_errno("open");
        return EXIT_FAILURE;
    }

    status = make_ext4fs_internal(fd, dirname, mountpoint, fs_config, 0, 0, 0, 1, sehnd, verbose);
    close(fd);

    return status;
}

/* return a newly-malloc'd string that is a copy of str.  The new string
   is guaranteed to have a trailing slash.  If absolute is true, the new string
   is also guaranteed to have a leading slash.
*/
static char *canonicalize_slashes(const char *str, bool absolute)
{
	char *ret;
	int len = strlen(str);
	int newlen = len;
	char *ptr;

	if (len == 0) {
		if (absolute)
			return strdup("/");
		else
			return strdup("");
	}

	if (str[0] != '/' && absolute) {
		newlen++;
	}
	if (str[len - 1] != '/') {
		newlen++;
	}
	ret = malloc(newlen + 1);
	if (!ret) {
		critical_error("malloc");
	}

	ptr = ret;
	if (str[0] != '/' && absolute) {
		*ptr++ = '/';
	}

	strcpy(ptr, str);
	ptr += len;

	if (str[len - 1] != '/') {
		*ptr++ = '/';
	}

	if (ptr != ret + newlen) {
		critical_error("assertion failed\n");
	}

	*ptr = '\0';

	return ret;
}

static char *canonicalize_abs_slashes(const char *str)
{
	return canonicalize_slashes(str, true);
}

static char *canonicalize_rel_slashes(const char *str)
{
	return canonicalize_slashes(str, false);
}

int make_ext4fs_internal(int fd, const char *_directory,
                         const char *_mountpoint, fs_config_func_t fs_config_func, int gzip,
                         int sparse, int crc, int wipe,
                         struct selabel_handle *sehnd, int verbose)
{
	u32 root_inode_num;
	u16 root_mode;
	char *mountpoint;
	char *directory = NULL;

	if (setjmp(setjmp_env))
		return EXIT_FAILURE; /* Handle a call to longjmp() */

	if (_mountpoint == NULL) {
		mountpoint = strdup("");
	} else {
		mountpoint = canonicalize_abs_slashes(_mountpoint);
	}

	if (_directory) {
		directory = canonicalize_rel_slashes(_directory);
	}

	if (info.len <= 0)
		info.len = get_file_size(fd);

	if (info.len <= 0) {
		fprintf(stderr, "Need size of filesystem\n");
		return EXIT_FAILURE;
	}

	if (info.block_size <= 0)
		info.block_size = compute_block_size();

	/* Round down the filesystem length to be a multiple of the block size */
	info.len &= ~((u64)info.block_size - 1);

	if (info.journal_blocks == 0)
		info.journal_blocks = compute_journal_blocks();

	if (info.no_journal == 0)
		info.feat_compat = EXT4_FEATURE_COMPAT_HAS_JOURNAL;
	else
		info.journal_blocks = 0;

	if (info.blocks_per_group <= 0)
		info.blocks_per_group = compute_blocks_per_group();

	if (info.inodes <= 0)
		info.inodes = compute_inodes();

	if (info.inode_size <= 0)
		info.inode_size = 256;

	if (info.label == NULL)
		info.label = "";

	info.inodes_per_group = compute_inodes_per_group();

	info.feat_compat |=
			EXT4_FEATURE_COMPAT_RESIZE_INODE |
			EXT4_FEATURE_COMPAT_EXT_ATTR;

	info.feat_ro_compat |=
			EXT4_FEATURE_RO_COMPAT_SPARSE_SUPER |
			EXT4_FEATURE_RO_COMPAT_LARGE_FILE |
			EXT4_FEATURE_RO_COMPAT_GDT_CSUM;

	info.feat_incompat |=
			EXT4_FEATURE_INCOMPAT_EXTENTS |
			EXT4_FEATURE_INCOMPAT_FILETYPE;


	info.bg_desc_reserve_blocks = compute_bg_desc_reserve_blocks();

	if (verbose >= 0) {
		printf("Creating filesystem with parameters:\n");
		printf("    Size: %"PRIu64"\n", info.len);
		printf("    Block size: %d\n", info.block_size);
		printf("    Blocks per group: %d\n", info.blocks_per_group);
		printf("    Inodes per group: %d\n", info.inodes_per_group);
		printf("    Inode size: %d\n", info.inode_size);
		printf("    Journal blocks: %d\n", info.journal_blocks);
		printf("    Label: %s\n", info.label);
	}

	ext4_create_fs_aux_info();
	if (verbose >= 0) {
		printf("    Blocks: %"PRIu64"\n", aux_info.len_blocks);
		printf("    Block groups: %d\n", aux_info.groups);
		printf("    Reserved block group size: %d\n", info.bg_desc_reserve_blocks);
	}

	aux_info.last_data_extent = &(aux_info.data_extents);

	ext4_sparse_file = sparse_file_new(info.block_size, info.len);

	block_allocator_init();

	ext4_fill_in_sb();

	if (reserve_inodes(0, 10) == EXT4_ALLOCATE_FAILED)
		error("failed to reserve first 10 inodes");

	if (info.feat_compat & EXT4_FEATURE_COMPAT_HAS_JOURNAL)
		ext4_create_journal_inode();

	if (info.feat_compat & EXT4_FEATURE_COMPAT_RESIZE_INODE)
		ext4_create_resize_inode();

	if (directory)
		root_inode_num = build_directory_structure(directory, mountpoint, 0,
                        fs_config_func, sehnd, verbose);
	else
		root_inode_num = build_default_directory_structure(mountpoint, sehnd);

	root_mode = S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH;
	inode_set_permissions(root_inode_num, root_mode, 0, 0, 0);

#if !defined(USE_MINGW) && !defined(_MSC_VER)
	if (sehnd) {
		char *secontext = NULL;

		if (selabel_lookup(sehnd, &secontext, mountpoint, S_IFDIR) < 0) {
			error("cannot lookup security context for %s", mountpoint);
		}
		if (secontext) {
			if (verbose) {
				printf("Labeling %s as %s\n", mountpoint, secontext);
			}
			inode_set_selinux(root_inode_num, secontext);
		}
		freecon(secontext);
	}
#endif

	ext4_update_free();

	ext4_queue_sb();

        if (verbose >= 0) {
          printf("Created filesystem with %d/%d inodes and %d/%d blocks\n",
                 aux_info.sb->s_inodes_count - aux_info.sb->s_free_inodes_count,
                 aux_info.sb->s_inodes_count,
                 aux_info.sb->s_blocks_count_lo - aux_info.sb->s_free_blocks_count_lo,
                 aux_info.sb->s_blocks_count_lo);
        }

	if (wipe && WIPE_IS_SUPPORTED) {
		wipe_block_device(fd, info.len);
	}

	write_ext4_image(fd, gzip, sparse, crc);

	sparse_file_destroy(ext4_sparse_file);
	ext4_sparse_file = NULL;
	block_allocator_free();
	{
		struct data_extents_node *curr = &(aux_info.data_extents);
		if (curr->addr) {
			free(curr->addr);
		}
		curr = curr->next;

		while (curr != NULL) {
			if (curr->addr) {
				free(curr->addr);
			}
			void* prev = curr;
			curr = curr->next;
			free(prev);
		}
	}
	ext4_free_fs_aux_info();
	free(mountpoint);
	free(directory);

	return 0;
}
