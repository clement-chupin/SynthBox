#pragma once
// HAL: the card's small files, its events (inserted, removed) and the folders an app wants on it. One implementation per board: the
// first prototype reads and writes the card on its card task (the calls wait for it, at most about a second); the desktop's card is
// a folder on the PC. The HAL knows no layout: an app says which folders it uses (storage_set_folders).
// Not for anything large: a file is read or written whole, in one call.
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define STORAGE_FILE_MAX 4096                       // the largest file that is read or written whole

// The folders the app uses, relative to the card's root, parents before their children (they are made in this order), 8 at most.
// The list must stay valid (a static array). Call it before the first card event is polled; without it the card has no folders to
// check or make.
#define STORAGE_FOLDERS_MAX 8
void storage_set_folders(const char *const *folders, int count);
// Files or folders of an older layout: before the folders are made (storage_make_folders), each `from` that exists is moved to `to`
// when `to` does not exist yet (the folders that lead to `to` are made first). Optional; the list must stay valid.
typedef struct { const char *from, *to; } storage_move_t;
void storage_set_moves(const storage_move_t *moves, int count);

// Reads the file `name` ("system/config/keys.cfg") into buf (at most cap - 1 bytes, then a 0). Returns the length, or -1 when there is no card or no file.
int  storage_read(const char *name, char *buf, int cap);

// Replaces the file `name` with `len` bytes of data. Returns false when there is no card or the write failed.
bool storage_write(const char *name, const char *data, int len);

// Deletes the file `name` (the sample versions the history trims). False when there is no card or it could not be removed.
bool storage_remove(const char *name);

// Card events: the board watches the card and queues what happened; the application polls them (once per step) and tells the user.
typedef enum {
    STORAGE_EV_NONE,
    STORAGE_EV_INSERTED,        // a card was mounted and checked: read time, slow, missing folders
    STORAGE_EV_REMOVED,
    STORAGE_EV_FOLDERS_DONE,    // storage_make_folders() finished: ok, moved
} storage_ev_kind_t;

typedef struct {
    uint8_t  kind;              // storage_ev_kind_t
    bool     slow;              // INSERTED: a sector read takes longer than read_limit_us: samples are off for this card (settings files still work)
    uint8_t  missing;           // INSERTED: bit i set = folder i of the app's list (storage_set_folders) is missing
    bool     ok;                // FOLDERS_DONE: every folder exists now
    bool     moved;             // FOLDERS_DONE: files of an older layout were moved first (storage_set_moves)
    uint32_t read_us;           // INSERTED: the measured time of one sector read
    uint32_t read_limit_us;     // INSERTED: the limit for streaming samples
} storage_event_t;

// The next card event (false: none). Never blocks.
bool storage_poll_event(storage_event_t *e);

// Creates the missing folders of the app's list on the card. Runs in the background; STORAGE_EV_FOLDERS_DONE follows.
void storage_make_folders(void);

#ifdef __cplusplus
}
#endif
