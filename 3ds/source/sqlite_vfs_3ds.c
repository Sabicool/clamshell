// Minimal SQLite VFS for the 3DS SD card, built on newlib's POSIX file calls
// (which libctru routes to sdmc:). No locking: only this app touches the file.
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <3ds.h>

#include "sqlite3.h"

typedef struct {
    sqlite3_file base;
    int fd;
    char path[512];
    int delete_on_close;
} SdFile;

static int sd_close(sqlite3_file* f) {
    SdFile* p = (SdFile*)f;
    close(p->fd);
    if (p->delete_on_close) unlink(p->path);
    return SQLITE_OK;
}

static int sd_read(sqlite3_file* f, void* buf, int amt, sqlite3_int64 off) {
    SdFile* p = (SdFile*)f;
    if (lseek(p->fd, (off_t)off, SEEK_SET) != (off_t)off) return SQLITE_IOERR_READ;
    int got = 0;
    while (got < amt) {
        int n = read(p->fd, (char*)buf + got, amt - got);
        if (n < 0) return SQLITE_IOERR_READ;
        if (n == 0) break;
        got += n;
    }
    if (got < amt) {
        memset((char*)buf + got, 0, amt - got);
        return SQLITE_IOERR_SHORT_READ;
    }
    return SQLITE_OK;
}

static int sd_write(sqlite3_file* f, const void* buf, int amt, sqlite3_int64 off) {
    SdFile* p = (SdFile*)f;
    if (lseek(p->fd, (off_t)off, SEEK_SET) != (off_t)off) return SQLITE_IOERR_WRITE;
    int put = 0;
    while (put < amt) {
        int n = write(p->fd, (const char*)buf + put, amt - put);
        if (n <= 0) return SQLITE_IOERR_WRITE;
        put += n;
    }
    return SQLITE_OK;
}

static int sd_truncate(sqlite3_file* f, sqlite3_int64 size) {
    SdFile* p = (SdFile*)f;
    return ftruncate(p->fd, (off_t)size) == 0 ? SQLITE_OK : SQLITE_IOERR_TRUNCATE;
}

static int sd_sync(sqlite3_file* f, int flags) {
    (void)flags;
    SdFile* p = (SdFile*)f;
    return fsync(p->fd) == 0 ? SQLITE_OK : SQLITE_OK;  // some devoptabs lack fsync; data is still written
}

static int sd_size(sqlite3_file* f, sqlite3_int64* size) {
    SdFile* p = (SdFile*)f;
    struct stat st;
    if (fstat(p->fd, &st) != 0) return SQLITE_IOERR_FSTAT;
    *size = st.st_size;
    return SQLITE_OK;
}

static int sd_lock(sqlite3_file* f, int lvl) { (void)f; (void)lvl; return SQLITE_OK; }
static int sd_unlock(sqlite3_file* f, int lvl) { (void)f; (void)lvl; return SQLITE_OK; }
static int sd_check_reserved(sqlite3_file* f, int* out) { (void)f; *out = 0; return SQLITE_OK; }
static int sd_file_control(sqlite3_file* f, int op, void* arg) { (void)f; (void)op; (void)arg; return SQLITE_NOTFOUND; }
static int sd_sector_size(sqlite3_file* f) { (void)f; return 512; }
static int sd_device_chars(sqlite3_file* f) { (void)f; return 0; }

static const sqlite3_io_methods sd_io = {
    1, sd_close, sd_read, sd_write, sd_truncate, sd_sync, sd_size, sd_lock, sd_unlock,
    sd_check_reserved, sd_file_control, sd_sector_size, sd_device_chars,
};

static int sd_open(sqlite3_vfs* vfs, const char* name, sqlite3_file* f, int flags, int* out_flags) {
    (void)vfs;
    SdFile* p = (SdFile*)f;
    memset(p, 0, sizeof *p);
    static int temp_counter = 0;
    if (!name) {
        snprintf(p->path, sizeof p->path, "sdmc:/3ds/clamshell/tmp-%d.db", temp_counter++);
        p->delete_on_close = 1;
    } else {
        snprintf(p->path, sizeof p->path, "%s", name);
        p->delete_on_close = (flags & SQLITE_OPEN_DELETEONCLOSE) != 0;
    }
    int oflags = 0;
    if (flags & SQLITE_OPEN_READWRITE) oflags |= O_RDWR;
    else oflags |= O_RDONLY;
    if (flags & SQLITE_OPEN_CREATE) oflags |= O_CREAT;
    if (flags & SQLITE_OPEN_EXCLUSIVE) oflags |= O_EXCL;
    p->fd = open(p->path, oflags, 0644);
    if (p->fd < 0 && (flags & SQLITE_OPEN_READWRITE)) {
        p->fd = open(p->path, O_RDONLY);
        if (p->fd >= 0 && out_flags) {
            *out_flags = (flags & ~SQLITE_OPEN_READWRITE) | SQLITE_OPEN_READONLY;
            p->base.pMethods = &sd_io;
            return SQLITE_OK;
        }
    }
    if (p->fd < 0) return SQLITE_CANTOPEN;
    if (out_flags) *out_flags = flags;
    p->base.pMethods = &sd_io;
    return SQLITE_OK;
}

static int sd_delete(sqlite3_vfs* vfs, const char* name, int sync_dir) {
    (void)vfs; (void)sync_dir;
    if (unlink(name) != 0 && errno != ENOENT) return SQLITE_IOERR_DELETE;
    return SQLITE_OK;
}

static int sd_access(sqlite3_vfs* vfs, const char* name, int flags, int* out) {
    (void)vfs; (void)flags;
    struct stat st;
    *out = stat(name, &st) == 0;
    return SQLITE_OK;
}

static int sd_full_path(sqlite3_vfs* vfs, const char* name, int n, char* out) {
    (void)vfs;
    snprintf(out, n, "%s", name);
    return SQLITE_OK;
}

static int sd_randomness(sqlite3_vfs* vfs, int n, char* out) {
    (void)vfs;
    for (int i = 0; i < n; i++) out[i] = (char)(rand() & 0xFF);
    return n;
}

static int sd_sleep(sqlite3_vfs* vfs, int us) {
    (void)vfs;
    svcSleepThread((s64)us * 1000);
    return us;
}

static int sd_current_time_int64(sqlite3_vfs* vfs, sqlite3_int64* out) {
    (void)vfs;
    // Julian day in milliseconds
    *out = (sqlite3_int64)time(NULL) * 1000 + 210866760000000LL;
    return SQLITE_OK;
}

static int sd_current_time(sqlite3_vfs* vfs, double* out) {
    sqlite3_int64 t;
    sd_current_time_int64(vfs, &t);
    *out = t / 86400000.0;
    return SQLITE_OK;
}

static int sd_last_error(sqlite3_vfs* vfs, int n, char* out) { (void)vfs; (void)n; (void)out; return 0; }

static sqlite3_vfs sd_vfs = {
    2, sizeof(SdFile), 512, NULL, "sdmc", NULL,
    sd_open, sd_delete, sd_access, sd_full_path,
    NULL, NULL, NULL, NULL,
    sd_randomness, sd_sleep, sd_current_time, sd_last_error, sd_current_time_int64,
};

int sqlite3_os_init(void) { return sqlite3_vfs_register(&sd_vfs, 1); }
int sqlite3_os_end(void) { return SQLITE_OK; }
