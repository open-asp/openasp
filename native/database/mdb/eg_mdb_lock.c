// Copyright (c) 2026 OpenASP.dev
// SPDX-License-Identifier: MIT

#include "eg_mdb_lock.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifdef __APPLE__
#include <sys/clonefile.h>
#endif

#ifdef __linux__
#include <sys/ioctl.h>
#include <linux/fs.h>
#endif

#define EG_MDB_JOURNAL_VERSION 1U

/*
 * Crash protocol:
 *   1. Acquire the per-path process mutex and sidecar fcntl write lock.
 *   2. Create and fsync a before-image at .egjournal.
 *   3. Atomically publish .egjournal.ready containing source identity.
 *   4. Mutate and fsync the MDB, then remove marker and journal.
 *
 * The ready marker is the commit point for recovery, not the journal file
 * itself. A crash while creating an unpublished journal is safe to discard.
 */
typedef struct EgMdbJournalMetadata {
    unsigned char magic[8];
    uint32_t version;
    uint32_t reserved;
    uint64_t device;
    uint64_t inode;
    uint64_t size;
} EgMdbJournalMetadata;

/*
 * Registry entries are process-lifetime objects. A dedicated sidecar fd avoids
 * POSIX record-lock loss when MDBTools closes an unrelated descriptor for the
 * database file. mutex serializes threads because fcntl locks are per process.
 */
typedef struct EgMdbLockEntry {
    char* db_path;
    char* lock_path;
    char* journal_path;
    char* ready_path;
    int lock_fd;
    pthread_mutex_t mutex;
    struct EgMdbLockEntry* next;
} EgMdbLockEntry;

static const unsigned char eg_mdb_journal_magic[8] = {'E', 'G', 'M', 'D', 'B', 'J', '1', '\0'};
static pthread_mutex_t eg_mdb_registry_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t eg_mdb_atfork_once = PTHREAD_ONCE_INIT;
static EgMdbLockEntry* eg_mdb_entries = NULL;

static void eg_mdb_lock_error(char* error, size_t capacity, const char* message) {
    if (error && capacity) {
        snprintf(error, capacity, "%s: %s", message, strerror(errno));
    }
}

static char* eg_mdb_suffix_path(const char* path, const char* suffix) {
    size_t path_length = strlen(path);
    size_t suffix_length = strlen(suffix);
    char* result = (char*)malloc(path_length + suffix_length + 1);
    if (result) {
        memcpy(result, path, path_length);
        memcpy(result + path_length, suffix, suffix_length + 1);
    }
    return result;
}

static int eg_mdb_set_file_lock(int fd, short type) {
    struct flock file_lock;
    memset(&file_lock, 0, sizeof(file_lock));
    file_lock.l_type = type;
    file_lock.l_whence = SEEK_SET;
    while (fcntl(fd, type == F_UNLCK ? F_SETLK : F_SETLKW, &file_lock) != 0) {
        if (errno != EINTR) {
            return 0;
        }
    }
    return 1;
}

static int eg_mdb_copy_contents(int source, int destination) {
    unsigned char buffer[131072];
    if (lseek(source, 0, SEEK_SET) < 0 || lseek(destination, 0, SEEK_SET) < 0 || ftruncate(destination, 0) != 0) {
        return 0;
    }
    for (;;) {
        ssize_t amount = read(source, buffer, sizeof(buffer));
        size_t written = 0;
        if (amount < 0) {
            if (errno == EINTR) {
                continue;
            }
            return 0;
        }
        if (amount == 0) {
            break;
        }
        while (written < (size_t)amount) {
            ssize_t result = write(destination, buffer + written, (size_t)amount - written);
            if (result < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return 0;
            }
            written += (size_t)result;
        }
    }
    return fsync(destination) == 0;
}

static int eg_mdb_write_all(int fd, const void* data, size_t length) {
    const unsigned char* bytes = (const unsigned char*)data;
    size_t written = 0;
    while (written < length) {
        ssize_t result = write(fd, bytes + written, length - written);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return 0;
        }
        written += (size_t)result;
    }
    return 1;
}

static int eg_mdb_read_all(int fd, void* data, size_t length) {
    unsigned char* bytes = (unsigned char*)data;
    size_t received = 0;
    while (received < length) {
        ssize_t result = read(fd, bytes + received, length - received);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return 0;
        }
        if (result == 0) {
            errno = EINVAL;
            return 0;
        }
        received += (size_t)result;
    }
    return 1;
}

static void eg_mdb_sync_parent(const char* path) {
    char* directory = strdup(path);
    char* slash;
    int fd;
    if (!directory) {
        return;
    }
    slash = strrchr(directory, '/');
    if (!slash) {
        free(directory);
        return;
    }
    if (slash == directory) {
        slash[1] = '\0';
    } else {
        *slash = '\0';
    }
    fd = open(directory, O_RDONLY);
    if (fd >= 0) {
        (void)fsync(fd);
        close(fd);
    }
    free(directory);
}

static void eg_mdb_atfork_prepare(void) {
    EgMdbLockEntry* entry;
    pthread_mutex_lock(&eg_mdb_registry_mutex);
    entry = eg_mdb_entries;
    while (entry) {
        pthread_mutex_lock(&entry->mutex);
        entry = entry->next;
    }
}

static void eg_mdb_atfork_unlock(void) {
    EgMdbLockEntry* entry = eg_mdb_entries;
    while (entry) {
        pthread_mutex_unlock(&entry->mutex);
        entry = entry->next;
    }
    pthread_mutex_unlock(&eg_mdb_registry_mutex);
}

static void eg_mdb_register_atfork(void) {
    (void)pthread_atfork(eg_mdb_atfork_prepare, eg_mdb_atfork_unlock, eg_mdb_atfork_unlock);
}

static EgMdbLockEntry* eg_mdb_find_or_create_entry(const char* db_path, char* error, size_t error_capacity) {
    EgMdbLockEntry* entry;
    char* canonical = realpath(db_path, NULL);
    if (!canonical) {
        eg_mdb_lock_error(error, error_capacity, "unable to resolve MDB path");
        return NULL;
    }
    pthread_once(&eg_mdb_atfork_once, eg_mdb_register_atfork);
    pthread_mutex_lock(&eg_mdb_registry_mutex);
    entry = eg_mdb_entries;
    while (entry && strcmp(entry->db_path, canonical) != 0) {
        entry = entry->next;
    }
    if (!entry) {
        entry = (EgMdbLockEntry*)calloc(1, sizeof(*entry));
        if (entry) {
            entry->db_path = canonical;
            canonical = NULL;
            entry->lock_path = eg_mdb_suffix_path(entry->db_path, ".eglock");
            entry->journal_path = eg_mdb_suffix_path(entry->db_path, ".egjournal");
            entry->ready_path = eg_mdb_suffix_path(entry->db_path, ".egjournal.ready");
            entry->lock_fd = -1;
            if (!entry->lock_path || !entry->journal_path || !entry->ready_path
                || pthread_mutex_init(&entry->mutex, NULL) != 0
                || (entry->lock_fd = open(entry->lock_path, O_RDWR | O_CREAT, 0666)) < 0) {
                if (entry->lock_fd >= 0) {
                    close(entry->lock_fd);
                }
                free(entry->ready_path);
                free(entry->journal_path);
                free(entry->lock_path);
                free(entry->db_path);
                free(entry);
                entry = NULL;
            } else {
                entry->next = eg_mdb_entries;
                eg_mdb_entries = entry;
            }
        }
    }
    pthread_mutex_unlock(&eg_mdb_registry_mutex);
    free(canonical);
    if (!entry) {
        errno = ENOMEM;
        eg_mdb_lock_error(error, error_capacity, "unable to create MDB path lock");
    }
    return entry;
}

static int eg_mdb_read_metadata(EgMdbLockEntry* entry, EgMdbJournalMetadata* metadata) {
    int fd = open(entry->ready_path, O_RDONLY);
    int result;
    if (fd < 0) {
        return 0;
    }
    result = eg_mdb_read_all(fd, metadata, sizeof(*metadata));
    close(fd);
    if (!result || memcmp(metadata->magic, eg_mdb_journal_magic, sizeof(metadata->magic)) != 0
        || metadata->version != EG_MDB_JOURNAL_VERSION) {
        errno = EINVAL;
        return -1;
    }
    return 1;
}

/* Restore only a journal whose metadata still identifies the same MDB inode. */
static int eg_mdb_restore_journal(EgMdbLockEntry* entry, char* error, size_t error_capacity) {
    EgMdbJournalMetadata metadata;
    struct stat database_status;
    struct stat journal_status;
    int database_fd = -1;
    int journal_fd = -1;
    int metadata_result = eg_mdb_read_metadata(entry, &metadata);
    if (metadata_result == 0) {
        (void)unlink(entry->journal_path);
        return 1;
    }
    if (metadata_result < 0 || stat(entry->db_path, &database_status) != 0
        || stat(entry->journal_path, &journal_status) != 0
        || (uint64_t)database_status.st_dev != metadata.device
        || (uint64_t)database_status.st_ino != metadata.inode
        || (uint64_t)journal_status.st_size != metadata.size) {
        errno = EINVAL;
        eg_mdb_lock_error(error, error_capacity, "invalid or mismatched MDB recovery journal");
        return 0;
    }
    journal_fd = open(entry->journal_path, O_RDONLY);
    database_fd = open(entry->db_path, O_RDWR);
    if (journal_fd < 0 || database_fd < 0 || !eg_mdb_copy_contents(journal_fd, database_fd)) {
        eg_mdb_lock_error(error, error_capacity, "unable to restore MDB recovery journal");
        if (database_fd >= 0) {
            close(database_fd);
        }
        if (journal_fd >= 0) {
            close(journal_fd);
        }
        return 0;
    }
    close(database_fd);
    close(journal_fd);
    if (unlink(entry->ready_path) != 0 || unlink(entry->journal_path) != 0) {
        eg_mdb_lock_error(error, error_capacity, "unable to remove restored MDB recovery journal");
        return 0;
    }
    eg_mdb_sync_parent(entry->db_path);
    return 1;
}

/*
 * Readers take a shared sidecar lock. If they observe a ready journal, they
 * temporarily upgrade through a fresh exclusive acquisition so recovery always
 * completes before any reader opens the database.
 */
int eg_mdb_lock_acquire(const char* db_path, int exclusive, EgMdbLock* lock, char* error, size_t error_capacity) {
    EgMdbLockEntry* entry;
    if (!lock) {
        errno = EINVAL;
        eg_mdb_lock_error(error, error_capacity, "invalid MDB lock");
        return 0;
    }
    memset(lock, 0, sizeof(*lock));
    entry = eg_mdb_find_or_create_entry(db_path, error, error_capacity);
    if (!entry) {
        return 0;
    }
    pthread_mutex_lock(&entry->mutex);
    if (!eg_mdb_set_file_lock(entry->lock_fd, exclusive ? F_WRLCK : F_RDLCK)) {
        eg_mdb_lock_error(error, error_capacity, "unable to acquire MDB sidecar lock");
        pthread_mutex_unlock(&entry->mutex);
        return 0;
    }
    if (!exclusive && access(entry->ready_path, F_OK) == 0) {
        (void)eg_mdb_set_file_lock(entry->lock_fd, F_UNLCK);
        pthread_mutex_unlock(&entry->mutex);
        if (!eg_mdb_lock_acquire(db_path, 1, lock, error, error_capacity)) {
            return 0;
        }
        eg_mdb_lock_release(lock);
        return eg_mdb_lock_acquire(db_path, 0, lock, error, error_capacity);
    }
    if (exclusive && !eg_mdb_restore_journal(entry, error, error_capacity)) {
        (void)eg_mdb_set_file_lock(entry->lock_fd, F_UNLCK);
        pthread_mutex_unlock(&entry->mutex);
        return 0;
    }
    lock->entry = entry;
    lock->exclusive = exclusive != 0;
    return 1;
}

/* Prefer copy-on-write clones, falling back to a fully fsynced stream copy. */
static int eg_mdb_clone_or_copy(EgMdbLockEntry* entry, const char* temporary, mode_t mode) {
    int source = -1;
    int destination = -1;
    int result = 0;
#ifdef __APPLE__
    if (clonefile(entry->db_path, temporary, CLONE_NOFOLLOW) == 0) {
        destination = open(temporary, O_RDWR);
        if (destination >= 0) {
            result = fsync(destination) == 0;
            close(destination);
        }
        return result;
    }
#endif
    source = open(entry->db_path, O_RDONLY);
    destination = open(temporary, O_RDWR | O_CREAT | O_EXCL, mode & 0777);
    if (source < 0 || destination < 0) {
        goto cleanup;
    }
#ifdef __linux__
    if (ioctl(destination, FICLONE, source) == 0) {
        result = fsync(destination) == 0;
        goto cleanup;
    }
#endif
    result = eg_mdb_copy_contents(source, destination);

cleanup:
    if (destination >= 0) {
        close(destination);
    }
    if (source >= 0) {
        close(source);
    }
    return result;
}

/*
 * Publish the before-image with rename + directory fsync ordering. transaction
 * state becomes active only after both payload and ready metadata are durable.
 */
int eg_mdb_transaction_begin(EgMdbLock* lock, char* error, size_t error_capacity) {
    EgMdbLockEntry* entry;
    EgMdbJournalMetadata metadata;
    struct stat status;
    char* journal_temporary;
    char* ready_temporary;
    size_t temporary_capacity;
    int ready_fd = -1;
    int result = 0;
    if (!lock || !lock->entry || !lock->exclusive || lock->transaction_active) {
        errno = EINVAL;
        eg_mdb_lock_error(error, error_capacity, "invalid MDB transaction state");
        return 0;
    }
    entry = (EgMdbLockEntry*)lock->entry;
    if (stat(entry->db_path, &status) != 0) {
        eg_mdb_lock_error(error, error_capacity, "unable to inspect MDB before transaction");
        return 0;
    }
    temporary_capacity = strlen(entry->journal_path) + 48;
    journal_temporary = (char*)malloc(temporary_capacity);
    ready_temporary = (char*)malloc(temporary_capacity);
    if (!journal_temporary || !ready_temporary) {
        errno = ENOMEM;
        eg_mdb_lock_error(error, error_capacity, "unable to allocate MDB journal path");
        goto cleanup;
    }
    snprintf(journal_temporary, temporary_capacity, "%s.tmp.%ld", entry->journal_path, (long)getpid());
    snprintf(ready_temporary, temporary_capacity, "%s.tmp.%ld", entry->ready_path, (long)getpid());
    (void)unlink(journal_temporary);
    (void)unlink(ready_temporary);
    if (!eg_mdb_clone_or_copy(entry, journal_temporary, status.st_mode)) {
        eg_mdb_lock_error(error, error_capacity, "unable to create MDB recovery journal");
        goto cleanup;
    }
    memset(&metadata, 0, sizeof(metadata));
    memcpy(metadata.magic, eg_mdb_journal_magic, sizeof(metadata.magic));
    metadata.version = EG_MDB_JOURNAL_VERSION;
    metadata.device = (uint64_t)status.st_dev;
    metadata.inode = (uint64_t)status.st_ino;
    metadata.size = (uint64_t)status.st_size;
    ready_fd = open(ready_temporary, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (ready_fd < 0 || !eg_mdb_write_all(ready_fd, &metadata, sizeof(metadata)) || fsync(ready_fd) != 0) {
        eg_mdb_lock_error(error, error_capacity, "unable to write MDB journal metadata");
        goto cleanup;
    }
    close(ready_fd);
    ready_fd = -1;
    if (rename(journal_temporary, entry->journal_path) != 0 || rename(ready_temporary, entry->ready_path) != 0) {
        eg_mdb_lock_error(error, error_capacity, "unable to publish MDB recovery journal");
        goto cleanup;
    }
    eg_mdb_sync_parent(entry->db_path);
    lock->transaction_active = 1;
    result = 1;

cleanup:
    if (ready_fd >= 0) {
        close(ready_fd);
    }
    if (!result) {
        if (journal_temporary) {
            (void)unlink(journal_temporary);
        }
        if (ready_temporary) {
            (void)unlink(ready_temporary);
        }
    }
    free(journal_temporary);
    free(ready_temporary);
    return result;
}

/* Durably flush the MDB before deleting the recovery marker and journal. */
int eg_mdb_transaction_commit(EgMdbLock* lock, char* error, size_t error_capacity) {
    EgMdbLockEntry* entry;
    int database_fd;
    if (!lock || !lock->entry || !lock->transaction_active) {
        errno = EINVAL;
        eg_mdb_lock_error(error, error_capacity, "invalid MDB transaction commit");
        return 0;
    }
    entry = (EgMdbLockEntry*)lock->entry;
    database_fd = open(entry->db_path, O_RDONLY);
    if (database_fd < 0 || fsync(database_fd) != 0) {
        eg_mdb_lock_error(error, error_capacity, "unable to durably flush MDB transaction");
        if (database_fd >= 0) {
            close(database_fd);
        }
        return 0;
    }
    close(database_fd);
    if (unlink(entry->ready_path) != 0 || unlink(entry->journal_path) != 0) {
        eg_mdb_lock_error(error, error_capacity, "unable to remove committed MDB journal");
        return 0;
    }
    eg_mdb_sync_parent(entry->db_path);
    lock->transaction_active = 0;
    return 1;
}

/* Restore the published before-image while the caller still owns write lock. */
int eg_mdb_transaction_rollback(EgMdbLock* lock, char* error, size_t error_capacity) {
    if (!lock || !lock->entry || !lock->transaction_active) {
        return 1;
    }
    if (!eg_mdb_restore_journal((EgMdbLockEntry*)lock->entry, error, error_capacity)) {
        return 0;
    }
    lock->transaction_active = 0;
    return 1;
}

void eg_mdb_lock_release(EgMdbLock* lock) {
    EgMdbLockEntry* entry;
    char ignored[1];
    if (!lock || !lock->entry) {
        return;
    }
    entry = (EgMdbLockEntry*)lock->entry;
    if (lock->transaction_active) {
        (void)eg_mdb_transaction_rollback(lock, ignored, sizeof(ignored));
    }
    (void)eg_mdb_set_file_lock(entry->lock_fd, F_UNLCK);
    pthread_mutex_unlock(&entry->mutex);
    memset(lock, 0, sizeof(*lock));
}
