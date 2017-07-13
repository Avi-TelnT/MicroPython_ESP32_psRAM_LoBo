/*
 * spiffs VFS operations
 *
 * Author: LoBo (loboris@gmail.com / https://github.com/loboris)
 *
 * Part of this code is copied from or inspired by LUA-RTOS_ESP32 project:
 *
 * https://github.com/whitecatboard/Lua-RTOS-ESP32
 * IBEROXARXA SERVICIOS INTEGRALES, S.L. & CSS IBÉRICA, S.L.
 * Jaume Olivé (jolive@iberoxarxa.com / jolive@whitecatboard.org)
 *
 */

#include "py/mpconfig.h"
#include "py/runtime.h"
#include "py/mperrno.h"

#include <freertos/FreeRTOS.h>

#include <string.h>
#include <stdio.h>
#include <limits.h>
#include "esp_log.h"

#include <sys/stat.h>

#include "esp_vfs.h"
#include "esp_attr.h"
#include <errno.h>

#include <spiffs.h>
#include <esp_spiffs.h>
#include <spiffs_nucleus.h>
#include "list.h"
#include <sys/fcntl.h>
#include <sys/dirent.h>
#include "sdkconfig.h"


#ifdef PATH_MAX
#undef PATH_MAX
#endif
#define PATH_MAX MAXNAMLEN+8

#define SPIFFS_ERASE_SIZE       4096
#define SPIFFS_BASE_ADDR        0x180000
#define SPIFFS_SIZE             0x200000
#define SPIFFS_LOG_PAGE_SIZE    256
#define SPIFFS_LOG_BLOCK_SIZE   8192

int spiffs_is_mounted = 0;

static int IRAM_ATTR vfs_spiffs_open(const char *path, int flags, int mode);
static size_t IRAM_ATTR vfs_spiffs_write(int fd, const void *data, size_t size);
static ssize_t IRAM_ATTR vfs_spiffs_read(int fd, void * dst, size_t size);
static int IRAM_ATTR vfs_spiffs_fstat(int fd, struct stat * st);
static int IRAM_ATTR vfs_spiffs_close(int fd);
static off_t IRAM_ATTR vfs_spiffs_lseek(int fd, off_t size, int mode);

typedef struct {
	DIR dir;
	spiffs_DIR spiffs_dir;
	char path[MAXNAMLEN + 1];
	struct dirent ent;
	uint8_t read_mount;
} vfs_spiffs_dir_t;

typedef struct {
	spiffs_file spiffs_file;
	char path[MAXNAMLEN + 1];
	uint8_t is_dir;
} vfs_spiffs_file_t;

typedef struct {
	time_t mtime;
	time_t ctime;
	time_t atime;
	uint8_t spare[SPIFFS_OBJ_META_LEN - (sizeof(time_t)*3)];
} spiffs_metadata_t;

typedef struct _fs_spiffs_mount_t {
    mp_obj_base_t base;
    spiffs spiffs_fs;
} fs_spiffs_mount_t;


static spiffs fs;
static struct list files;

static u8_t *my_spiffs_work_buf;
static u8_t *my_spiffs_fds;
static u8_t *my_spiffs_cache;


/*
 * ########################################
 * file names/paths passed to the functions
 * do not contain '/spiffs' prefix
 * ########################################
 */

//----------------------------------------------------
void spiffs_fs_stat(uint32_t *total, uint32_t *used) {
	if (SPIFFS_info(&fs, total, used) != SPIFFS_OK) {
		*total = 0;
		*used = 0;
	}
}

/*
 * Test if path corresponds to a directory. Return 0 if is not a directory,
 * 1 if it's a directory.
 *
 */
//-----------------------------------
static int is_dir(const char *path) {
    spiffs_DIR d;
    char npath[PATH_MAX + 1];
    int res = 0;

    struct spiffs_dirent e;

    // Add /. to path
    strlcpy(npath, path, PATH_MAX);
    if (strcmp(path,"/") != 0) {
        strlcat(npath,"/.", PATH_MAX);
    } else {
    	strlcat(npath,".", PATH_MAX);
    }

    SPIFFS_opendir(&fs, "/", &d);
    while (SPIFFS_readdir(&d, &e)) {
        if (strncmp(npath, (const char *)e.name, strlen(npath)) == 0) {
            res = 1;
            break;
        }
    }

    SPIFFS_closedir(&d);

    return res;
}

/*
 * This function translate error codes from SPIFFS to errno error codes
 *
 */
//-------------------------------
static int spiffs_result(int res) {
    switch (res) {
        case SPIFFS_OK:
        case SPIFFS_ERR_END_OF_OBJECT:
            return 0;

        case SPIFFS_ERR_NOT_FOUND:
        case SPIFFS_ERR_CONFLICTING_NAME:
            return ENOENT;

        case SPIFFS_ERR_NOT_WRITABLE:
        case SPIFFS_ERR_NOT_READABLE:
            return EACCES;

        case SPIFFS_ERR_FILE_EXISTS:
            return EEXIST;

        default:
            return res;
    }
}

//-----------------------------------------------------------------------------------------------------
static int IRAM_ATTR vfs_spiffs_getstat(spiffs_file fd, spiffs_stat *st, spiffs_metadata_t *metadata) {
    int res = SPIFFS_fstat(&fs, fd, st);
    if (res == SPIFFS_OK) {
        // Get file's time information from metadata
        memcpy(metadata, st->meta, sizeof(spiffs_metadata_t));
	}
    return res;
}

// ## path does not contain '/spiffs' prefix !
//---------------------------------------------------------------------------
static int IRAM_ATTR vfs_spiffs_open(const char *path, int flags, int mode) {
	int fd, result = 0, exists = 0;
	spiffs_stat stat;
	spiffs_metadata_t meta;

	// Allocate new file
	vfs_spiffs_file_t *file = calloc(1, sizeof(vfs_spiffs_file_t));
	if (!file) {
		errno = ENOMEM;
		return -1;
	}

    // Add file to file list. List index is file descriptor.
    int res = list_add(&files, file, &fd);
    if (res) {
    	free(file);
    	errno = res;
    	return -1;
    }

    // Check if file exists
    if (SPIFFS_stat(&fs, path, &stat) == SPIFFS_OK) exists = 1;

    // Make a copy of path
	strlcpy(file->path, path, MAXNAMLEN);

    // Open file
    spiffs_flags spiffs_mode = 0;

    // Translate flags to SPIFFS flags
    if (flags == O_RDONLY)
    	spiffs_mode |= SPIFFS_RDONLY;

    if (flags & O_WRONLY)
    	spiffs_mode |= SPIFFS_WRONLY;

    if (flags & O_RDWR)
    	spiffs_mode = SPIFFS_RDWR;

    if (flags & O_EXCL)
    	spiffs_mode |= SPIFFS_EXCL;

    if (flags & O_CREAT)
    	spiffs_mode |= SPIFFS_CREAT;

    if (flags & O_TRUNC)
    	spiffs_mode |= SPIFFS_TRUNC;

    if (is_dir(path)) {
        char npath[PATH_MAX + 1];

        // Add /. to path
        strlcpy(npath, path, PATH_MAX);
        if (strcmp(path,"/") != 0) {
            strlcat(npath,"/.", PATH_MAX);
        } else {
        	strlcat(npath,".", PATH_MAX);
        }

        // Open SPIFFS file
        file->spiffs_file = SPIFFS_open(&fs, npath, spiffs_mode, 0);
        if (file->spiffs_file < 0) {
            result = spiffs_result(fs.err_code);
        }

    	file->is_dir = 1;
    } else {
        // Open SPIFFS file
        file->spiffs_file = SPIFFS_open(&fs, path, spiffs_mode, 0);
        if (file->spiffs_file < 0) {
            result = spiffs_result(fs.err_code);
        }
    }

    if (result != 0) {
    	list_remove(&files, fd, 1);
    	errno = result;
    	return -1;
    }

    res = vfs_spiffs_getstat(file->spiffs_file, &stat, &meta);
	if (res == SPIFFS_OK) {
		// update file's time information
		meta.atime = time(NULL); // Get the system time to access time
		if (!exists) meta.ctime = meta.atime;
		if (spiffs_mode != SPIFFS_RDONLY) meta.mtime = meta.atime;
		SPIFFS_fupdate_meta(&fs, file->spiffs_file, &meta);
	}

    return fd;
}

//-------------------------------------------------------------------------------
static size_t IRAM_ATTR vfs_spiffs_write(int fd, const void *data, size_t size) {
	vfs_spiffs_file_t *file;
	int res;

    res = list_get(&files, fd, (void **)&file);
    if (res) {
		errno = EBADF;
		return -1;
    }

    if (file->is_dir) {
		errno = EBADF;
		return -1;
    }

    // Write SPIFFS file
	res = SPIFFS_write(&fs, file->spiffs_file, (void *)data, size);
	if (res >= 0) {
		return res;
	} else {
		res = spiffs_result(fs.err_code);
		if (res != 0) {
			errno = res;
			return -1;
		}
	}

	return -1;
}

//-------------------------------------------------------------------------
static ssize_t IRAM_ATTR vfs_spiffs_read(int fd, void * dst, size_t size) {
	vfs_spiffs_file_t *file;
	int res;

    res = list_get(&files, fd, (void **)&file);
    if (res) {
		errno = EBADF;
		return -1;
    }

    if (file->is_dir) {
		errno = EBADF;
		return -1;
    }

    // Read SPIFFS file
	res = SPIFFS_read(&fs, file->spiffs_file, dst, size);
	if (res >= 0) {
		return res;
	} else {
		res = spiffs_result(fs.err_code);
		if (res != 0) {
			errno = res;
			return -1;
		}

		// EOF
		return 0;
	}

	return -1;
}

//---------------------------------------------------------------
static int IRAM_ATTR vfs_spiffs_fstat(int fd, struct stat * st) {
	vfs_spiffs_file_t *file;
    spiffs_stat stat;
	int res;
	spiffs_metadata_t meta;

    res = list_get(&files, fd, (void **)&file);
    if (res) {
		errno = EBADF;
		return -1;
    }

    // Set block size for this file system
    st->st_blksize = SPIFFS_LOG_PAGE_SIZE;

    // Get file/directory statistics
    res = vfs_spiffs_getstat(file->spiffs_file, &stat, &meta);
    if (res == SPIFFS_OK) {
        // Set file's time information from metadata
        st->st_mtime = meta.mtime;
        st->st_ctime = meta.ctime;
        st->st_atime = meta.atime;

    	st->st_size = stat.size;

	} else {
        st->st_mtime = 0;
        st->st_ctime = 0;
        st->st_atime = 0;
		st->st_size = 0;
	    errno = spiffs_result(fs.err_code);
		//printf("SPIFFS_STAT: error %d\r\n", res);
    	return -1;
    }

    // Test if it's a directory entry
    if (file->is_dir) st->st_mode = S_IFDIR;
    else st->st_mode = S_IFREG;

    return 0;
}

//---------------------------------------------
static int IRAM_ATTR vfs_spiffs_close(int fd) {
	vfs_spiffs_file_t *file;
	int res;

    res = list_get(&files, fd, (void **)&file);
    if (res) {
		errno = EBADF;
		return -1;
    }

	res = SPIFFS_close(&fs, file->spiffs_file);
	if (res) {
		res = spiffs_result(fs.err_code);
	}

	if (res < 0) {
		errno = res;
		return -1;
	}

	list_remove(&files, fd, 1);

	return 0;
}

//---------------------------------------------------------------------
static off_t IRAM_ATTR vfs_spiffs_lseek(int fd, off_t size, int mode) {
	vfs_spiffs_file_t *file;
	int res;

    res = list_get(&files, fd, (void **)&file);
    if (res) {
		errno = EBADF;
		return -1;
    }

    if (file->is_dir) {
		errno = EBADF;
		return -1;
    }

	int whence = SPIFFS_SEEK_CUR;

    switch (mode) {
        case SEEK_SET: whence = SPIFFS_SEEK_SET;break;
        case SEEK_CUR: whence = SPIFFS_SEEK_CUR;break;
        case SEEK_END: whence = SPIFFS_SEEK_END;break;
    }

    res = SPIFFS_lseek(&fs, file->spiffs_file, size, whence);
    if (res < 0) {
        res = spiffs_result(fs.err_code);
        errno = res;
        return -1;
    }

    return res;
}

//-------------------------------------------------------------------------
static int IRAM_ATTR vfs_spiffs_stat(const char * path, struct stat * st) {
	int fd;
	int res;
	fd = vfs_spiffs_open(path, 0, 0);
	res = vfs_spiffs_fstat(fd, st);
	vfs_spiffs_close(fd);

	return res;
}

//--------------------------------------------------------
static int IRAM_ATTR vfs_spiffs_unlink(const char *path) {
    char npath[PATH_MAX + 1];

    strlcpy(npath, path, PATH_MAX);

    if (is_dir(path)) {
        // Check if  directory is empty
    	int nument = 0;
    	sprintf(npath, "/spiffs");
    	strlcat(npath, path, PATH_MAX);

    	DIR *dir = opendir(npath);
        if (dir) {
            struct dirent *ent;
			// Read directory entries
			while ((ent = readdir(dir)) != NULL) {
				nument++;
			}
        }
        else {
        	errno = ENOTEMPTY;
        	return -1;
        }
        closedir(dir);

        if (nument > 0) {
        	// Directory not empty, cannot remove
        	errno = ENOTEMPTY;
        	return -1;
        }

        strlcpy(npath, path, PATH_MAX);
    	// Add /. to path
	    if (strcmp(path,"/") != 0) {
	        strlcat(npath,"/.", PATH_MAX);
	    }
	}

    // Open SPIFFS file
	spiffs_file FP = SPIFFS_open(&fs, npath, SPIFFS_RDWR, 0);
    if (FP < 0) {
    	errno = spiffs_result(fs.err_code);
    	return -1;
    }

    // Remove SPIFSS file
    if (SPIFFS_fremove(&fs, FP) < 0) {
        errno = spiffs_result(fs.err_code);
    	SPIFFS_close(&fs, FP);
    	return -1;
    }

	SPIFFS_close(&fs, FP);

	return 0;
}

//------------------------------------------------------------------------
static int IRAM_ATTR vfs_spiffs_rename(const char *src, const char *dst) {
    if (SPIFFS_rename(&fs, src, dst) < 0) {
    	errno = spiffs_result(fs.err_code);
    	return -1;
    }

    return 0;
}

//------------------------------------------------
static DIR* vfs_spiffs_opendir(const char* name) {
	struct stat st;

    if (strcmp(name, "/") != 0) {
    	// Not on root
    	if (vfs_spiffs_stat(name, &st)) {
    		// Not found
    		errno = ENOENT;
    		return NULL;
        }
    	if (!S_ISDIR(st.st_mode)) {
    		// Not a directory
    		errno = ENOTDIR;
    		return NULL;
        }
    }

	vfs_spiffs_dir_t *dir = calloc(1, sizeof(vfs_spiffs_dir_t));

	if (!dir) {
		errno = ENOMEM;
		return NULL;
	}

	if (!SPIFFS_opendir(&fs, name, &dir->spiffs_dir)) {
        free(dir);
        errno = spiffs_result(fs.err_code);
        return NULL;
    }

	strlcpy(dir->path, name, MAXNAMLEN);

	return (DIR *)dir;
}

//---------------------------------------------------
static struct dirent* vfs_spiffs_readdir(DIR* pdir) {
    int res = 0, len = 0, entries = 0;
	vfs_spiffs_dir_t* dir = (vfs_spiffs_dir_t*) pdir;

	struct spiffs_dirent e;
    struct spiffs_dirent *pe = &e;

    struct dirent *ent = &dir->ent;

    char *fn;

    // Clear current dirent
    memset(ent,0,sizeof(struct dirent));

    // If this is the first call to readdir for pdir, and
    // directory is the root path, return the mounted point if any
    if (!dir->read_mount) {
    	if (strcmp(dir->path,"/") == 0) {
			strlcpy(ent->d_name, "/spiffs", PATH_MAX);
			ent->d_type = DT_DIR;
			dir->read_mount = 1;

			return ent;
    	}

    	dir->read_mount = 1;
    }

    // Search for next entry
    for(;;) {
        // Read directory
        pe = SPIFFS_readdir(&dir->spiffs_dir, pe);
        if (!pe) {
            res = spiffs_result(fs.err_code);
            errno = res;
            break;
        }

        // Break condition
        if (pe->name[0] == 0) break;

        // Get name and length
        fn = (char *)pe->name;
        len = strlen(fn);

        // Get entry type and size
        ent->d_type = DT_REG;

        if (len >= 2) {
            if (fn[len - 1] == '.') {
                if (fn[len - 2] == '/') {
                    ent->d_type = DT_DIR;

                    fn[len - 2] = '\0';

                    len = strlen(fn);

                    // Skip root dir
                    if (len == 0) {
                        continue;
                    }
                }
            }
        }

        // Skip entries not belonged to path
        if (strncmp(fn, dir->path, strlen(dir->path)) != 0) {
            continue;
        }

        if (strlen(dir->path) > 1) {
            if (*(fn + strlen(dir->path)) != '/') {
                continue;
            }
        }

        // Skip root directory
        fn = fn + strlen(dir->path);
        len = strlen(fn);
        if (len == 0) {
            continue;
        }

        // Skip initial /
        if (len > 1) {
            if (*fn == '/') {
                fn = fn + 1;
                len--;
            }
        }

        // Skip subdirectories
        if (strchr(fn,'/')) {
            continue;
        }

        //ent->d_fsize = pe->size;

        strlcpy(ent->d_name, fn, MAXNAMLEN);

        entries++;

        break;
    }

    if (entries > 0) {
    	return ent;
    } else {
    	return NULL;
    }
}

//--------------------------------------------------
static int IRAM_ATTR vfs_piffs_closedir(DIR* pdir) {
	vfs_spiffs_dir_t* dir = (vfs_spiffs_dir_t*) pdir;
	int res;

	if (!pdir) {
		errno = EBADF;
		return -1;
	}

	if ((res = SPIFFS_closedir(&dir->spiffs_dir)) < 0) {
		errno = spiffs_result(fs.err_code);;
		return -1;
	}

	free(dir);

    return 0;
}

//--------------------------------------------------------------------
static int IRAM_ATTR vfs_spiffs_mkdir(const char *path, mode_t mode) {
    char npath[PATH_MAX + 1];
    int res;

    // Add /. to path
    strlcpy(npath, path, PATH_MAX);
    if ((strcmp(path,"/") != 0) && (strcmp(path,"/.") != 0)) {
        strlcat(npath,"/.", PATH_MAX);
    }

    spiffs_file fd = SPIFFS_open(&fs, npath, SPIFFS_CREAT, 0);
    if (fd < 0) {
        res = spiffs_result(fs.err_code);
        errno = res;
        return -1;
    }

    if (SPIFFS_close(&fs, fd) < 0) {
        res = spiffs_result(fs.err_code);
        errno = res;
        return -1;
    }

	spiffs_metadata_t meta;
	meta.atime = time(NULL); // Get the system time to access time
	meta.ctime = meta.atime;
	meta.mtime = meta.atime;
	SPIFFS_update_meta(&fs, npath, &meta);

    return 0;
}


//==================
int spiffs_mount() {

	if (spiffs_is_mounted) return 1;

	spiffs_config cfg;
    int res = 0;
    int retries = 0;
    int err = 0;

    printf("[SPIFFS] Mounting SPIFFS files system\n");

    cfg.phys_addr 		 = SPIFFS_BASE_ADDR;
    cfg.phys_size 		 = SPIFFS_SIZE;
    cfg.phys_erase_block = SPIFFS_ERASE_SIZE;
    cfg.log_page_size    = SPIFFS_LOG_PAGE_SIZE;
    cfg.log_block_size   = SPIFFS_LOG_BLOCK_SIZE;

	cfg.hal_read_f  = (spiffs_read)low_spiffs_read;
	cfg.hal_write_f = (spiffs_write)low_spiffs_write;
	cfg.hal_erase_f = (spiffs_erase)low_spiffs_erase;

    my_spiffs_work_buf = malloc(cfg.log_page_size * 8);
    if (!my_spiffs_work_buf) {
    	err = 1;
    	goto err_exit;
    }

    int fds_len = sizeof(spiffs_fd) * SPIFFS_TEMPORAL_CACHE_HIT_SCORE;
    my_spiffs_fds = malloc(fds_len);
    if (!my_spiffs_fds) {
        free(my_spiffs_work_buf);
    	err = 2;
    	goto err_exit;
    }

    int cache_len = cfg.log_page_size * SPIFFS_TEMPORAL_CACHE_HIT_SCORE;
    my_spiffs_cache = malloc(cache_len);
    if (!my_spiffs_cache) {
        free(my_spiffs_work_buf);
        free(my_spiffs_fds);
    	err = 3;
    	goto err_exit;
    }

    printf("Start address: 0x%x; Size %d KB\n", cfg.phys_addr, cfg.phys_size / 1024);
    printf("  Work buffer: %d B\n", cfg.log_page_size * 8);
    printf("   FDS buffer: %d B\n", sizeof(spiffs_fd) * SPIFFS_TEMPORAL_CACHE_HIT_SCORE);
    printf("   Cache size: %d B\n", cfg.log_page_size * SPIFFS_TEMPORAL_CACHE_HIT_SCORE);
    while (retries < 2) {
		res = SPIFFS_mount(
				&fs, &cfg, my_spiffs_work_buf, my_spiffs_fds,
				fds_len, my_spiffs_cache, cache_len, NULL
		);

		if (res < 0) {
			if (fs.err_code == SPIFFS_ERR_NOT_A_FS) {
				printf("No file system detected, formating...\n");
				SPIFFS_unmount(&fs);
				res = SPIFFS_format(&fs);
				if (res < 0) {
			        free(my_spiffs_work_buf);
			        free(my_spiffs_fds);
			        free(my_spiffs_cache);
					printf("Format error\n");
					goto exit;
				}
			}
			else {
		        free(my_spiffs_work_buf);
		        free(my_spiffs_fds);
		        free(my_spiffs_cache);
				printf("Error mounting fs (%d)\n", res);
				goto exit;
			}
		}
		else break;
		retries++;
    }

    if (retries > 1) {
        free(my_spiffs_work_buf);
        free(my_spiffs_fds);
        free(my_spiffs_cache);
		printf("Can't mount\n");
		goto exit;
    }

    list_init(&files, 0);

    printf("Mounted\n");

    spiffs_is_mounted = 1;
    return 1;

err_exit:
	printf("Error allocating fs structures (%d)\n", err);
exit:
	return 0;
}

//===========
void init() {

	spiffs_mount();
}

//====================
int spiffs_unmount() {

	if (!spiffs_is_mounted) return 1;

	SPIFFS_unmount(&fs);
    spiffs_is_mounted = 0;

	return 1;
}

//===========================================================================================================

//------------------------------------------------------------------------------------
STATIC mp_obj_t spiffs_vfs_mount(mp_obj_t self_in, mp_obj_t readonly, mp_obj_t mkfs) {
    int res = spiffs_mount();
    if (res == 0) {
        mp_raise_OSError(MP_EPERM);
    }

    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_3(spiffs_vfs_mount_obj, spiffs_vfs_mount);

//---------------------------------------------------
STATIC mp_obj_t spiffs_vfs_umount(mp_obj_t self_in) {
    int res = spiffs_unmount();
    if (res == 0) {
        mp_raise_OSError(MP_EPERM);
    }
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(spiffs_vfs_umount_obj, spiffs_vfs_umount);

//--------------------------------------------------------------
STATIC mp_obj_t spiffs_vfs_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args) {

    // create new object
    fs_spiffs_mount_t *vfs = m_new_obj(fs_spiffs_mount_t);
    vfs->base.type = type;
    vfs->spiffs_fs = fs;

    return MP_OBJ_FROM_PTR(vfs);
}

//-------------------------------------------------
STATIC mp_obj_t spiffs_vfs_mkfs(mp_obj_t self_in) {

    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(spiffs_vfs_mkfs_fun_obj, spiffs_vfs_mkfs);
STATIC MP_DEFINE_CONST_STATICMETHOD_OBJ(spiffs_vfs_mkfs_obj, MP_ROM_PTR(&spiffs_vfs_mkfs_fun_obj));

/*
// Factory function for I/O stream classes
mp_obj_t fatfs_builtin_open_self(mp_obj_t self_in, mp_obj_t path, mp_obj_t mode) {
    // TODO: analyze buffering args and instantiate appropriate type
    fs_user_mount_t *self = MP_OBJ_TO_PTR(self_in);
    mp_arg_val_t arg_vals[FILE_OPEN_NUM_ARGS];
    arg_vals[0].u_obj = path;
    arg_vals[1].u_obj = mode;
    arg_vals[2].u_obj = mp_const_none;
    return file_open(self, &mp_type_textio, arg_vals);
}

STATIC MP_DEFINE_CONST_FUN_OBJ_3(fat_vfs_open_obj, fatfs_builtin_open_self);
*/

// Get the status of a VFS.
STATIC mp_obj_t spiffs_vfs_statvfs(mp_obj_t vfs_in, mp_obj_t path_in) {
    (void)path_in;
    //fs_spiffs_mount_t *self = MP_OBJ_TO_PTR(vfs_in);

	if (!spiffs_is_mounted) {
        mp_raise_OSError(MP_EPERM);
    }

    uint32_t total = 0;
    uint32_t used = 0;

	if (SPIFFS_info(&fs, &total, &used) != SPIFFS_OK) {
        mp_raise_OSError(MP_EPERM);
	}

    mp_obj_tuple_t *t = MP_OBJ_TO_PTR(mp_obj_new_tuple(2, NULL));

    t->items[0] = MP_OBJ_NEW_SMALL_INT(total); // total size
    t->items[1] = MP_OBJ_NEW_SMALL_INT(used); // used size

    return MP_OBJ_FROM_PTR(t);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_2(spiffs_vfs_statvfs_obj, spiffs_vfs_statvfs);






//===============================================================
STATIC const mp_rom_map_elem_t spiffs_vfs_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_mkfs), MP_ROM_PTR(&spiffs_vfs_mkfs_obj) },
    //{ MP_ROM_QSTR(MP_QSTR_open), MP_ROM_PTR(&spiffs_vfs_open_obj) },
    //{ MP_ROM_QSTR(MP_QSTR_ilistdir), MP_ROM_PTR(&spiffs_vfs_ilistdir_obj) },
    //{ MP_ROM_QSTR(MP_QSTR_mkdir), MP_ROM_PTR(&spiffs_vfs_mkdir_obj) },
    //{ MP_ROM_QSTR(MP_QSTR_rmdir), MP_ROM_PTR(&spiffs_vfs_rmdir_obj) },
    //{ MP_ROM_QSTR(MP_QSTR_chdir), MP_ROM_PTR(&spiffs_vfs_chdir_obj) },
    //{ MP_ROM_QSTR(MP_QSTR_getcwd), MP_ROM_PTR(&spiffs_vfs_getcwd_obj) },
    //{ MP_ROM_QSTR(MP_QSTR_remove), MP_ROM_PTR(&spiffs_vfs_remove_obj) },
    //{ MP_ROM_QSTR(MP_QSTR_rename), MP_ROM_PTR(&spiffs_vfs_rename_obj) },
    //{ MP_ROM_QSTR(MP_QSTR_stat), MP_ROM_PTR(&spiffs_vfs_stat_obj) },
    { MP_ROM_QSTR(MP_QSTR_statvfs), MP_ROM_PTR(&spiffs_vfs_statvfs_obj) },
    { MP_ROM_QSTR(MP_QSTR_mount), MP_ROM_PTR(&spiffs_vfs_mount_obj) },
    { MP_ROM_QSTR(MP_QSTR_umount), MP_ROM_PTR(&spiffs_vfs_umount_obj) },
};
STATIC MP_DEFINE_CONST_DICT(spiffs_vfs_locals_dict, spiffs_vfs_locals_dict_table);

//========================================
const mp_obj_type_t mp_spiffs_vfs_type = {
    { &mp_type_type },
    .name = MP_QSTR_VfsSpiffs,
    .make_new = spiffs_vfs_make_new,
    .locals_dict = (mp_obj_dict_t*)&spiffs_vfs_locals_dict,
};

