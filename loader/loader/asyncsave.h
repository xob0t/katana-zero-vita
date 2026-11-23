/*
 * asyncsave.h
 *
 * Background save writer (asyncsave.c): Netflix cloud slots and the runner's save files reach the memory card on
 * one writer thread, in the order the game issued them, so checkpoints don't stall the game thread.
 */
#ifndef ASYNCSAVE_H
#define ASYNCSAVE_H

#include <stddef.h>
#include <stdio.h>

// 1 once the writer thread runs (asyncSave=0 in yyl.cfg or exp_asyncsave0 keep every save synchronous, as before)
extern int asyncsave_on;
extern int asyncsave_cfg;

// Commits one file: returns 0 or a negative error. Runs on the writer thread.
typedef int (*asyncsave_commit_fn)(const char *path, const char *data, size_t len);

// Before the runner starts: finishes a commit that a crash interrupted, then starts the writer (unless exp_asyncsave0)
void asyncsave_init(const char *save_dir, int cpu_mask);

// Hands a whole file to the writer (takes ownership of data, a malloc block). A queued, not yet started write to the
// same path is dropped and the new one goes to the back of the queue. Returns -1 if it can't be queued (the caller
// writes synchronously then).
int asyncsave_queue(const char *path, char *data, size_t len, asyncsave_commit_fn commit);

// Runner save files: the .zero files in the save directory
int asyncsave_is_save_file(const char *path);
// fopen(path, "w"/"wb") for a save file: a FILE that keeps the data in memory and queues it at fclose.
// NULL: open the file as before.
FILE *asyncsave_fopen_write(const char *path);
// Commit used for runner files (path.new, fsync, rename over with path.bak)
int asyncsave_commit_file(const char *path, const char *data, size_t len);

// Returns once no write to path is queued or running (other file operations on it then see the new content).
// Free when nothing is pending.
void asyncsave_wait_path(const char *path);
// Returns once every queued write has finished
void asyncsave_wait_all(void);
// Exit/suspend: waits up to timeout_ms for the queue to drain; returns the number of writes still pending
int asyncsave_flush(int timeout_ms, const char *why);

// Runner imports on save paths, ordered after pending writes
int asyncsave_remove(const char *path);
int asyncsave_rename(const char *from, const char *to);

#endif
