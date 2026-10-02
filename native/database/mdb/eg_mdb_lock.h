// Copyright (c) 2026 OpenASP.dev
// SPDX-License-Identifier: MIT

#ifndef EG_MDB_LOCK_H
#define EG_MDB_LOCK_H

#include <stddef.h>

/*
 * One acquired database lock. `entry` is an opaque process-local path entry,
 * `exclusive` records shared versus writer mode, and `transaction_active`
 * means a before-image journal must be committed or rolled back before release.
 * Callers initialize ownership only through eg_mdb_lock_acquire().
 */
typedef struct EgMdbLock {
    void* entry;
    int exclusive;
    int transaction_active;
} EgMdbLock;

/*
 * All cooperating readers lock <database>.eglock in shared mode; writers use
 * exclusive mode. Transactions snapshot the database, fsync commits, and
 * restore the journal on rollback or crash recovery. Every successful acquire
 * must have exactly one release, including error paths.
 */
int eg_mdb_lock_acquire(const char* db_path, int exclusive, EgMdbLock* lock, char* error, size_t error_capacity);
int eg_mdb_transaction_begin(EgMdbLock* lock, char* error, size_t error_capacity);
int eg_mdb_transaction_commit(EgMdbLock* lock, char* error, size_t error_capacity);
int eg_mdb_transaction_rollback(EgMdbLock* lock, char* error, size_t error_capacity);
void eg_mdb_lock_release(EgMdbLock* lock);

#endif
