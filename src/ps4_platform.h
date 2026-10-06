#ifndef PS4_PLATFORM_H
#define PS4_PLATFORM_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Read-only game data inside the pkg, and writable working dir on the console.
#define PS4_GAMEDATA_DIR "/app0/gamedata"
#define PS4_WORK_DIR "/data/fallout2"

#define PS4_SCREEN_WIDTH 1920
#define PS4_SCREEN_HEIGHT 1080

// Appends a line to PS4_WORK_DIR/ps4.log (and stdout). printf-style.
void ps4_log(const char* fmt, ...);

// Prepares the writable working dir (seeds config/data on first run) (no chdir). Must run before SDL_Init and the engine. Returns 0 on success.
int ps4_init_filesystem(void);

// Resolves an engine path (relative, possibly with backslashes) to an absolute
// native path under PS4_WORK_DIR. chdir() is not allowed in the Orbis sandbox.
void ps4_abs_path(const char* in, char* out, size_t cap);
char* ps4_getcwd(char* buf, size_t size);
int ps4_chdir(const char* path);

// Milliseconds since first call, from the Orbis kernel clock (std::chrono / SDL_GetTicks were frozen).
unsigned int ps4_ticks_ms(void);

// Called once per frame/poll; the watchdog logs a stall if it stops.
void ps4_heartbeat(void);

// Opens the first gamepad (hot-plug safe, cheap to call every frame).
void ps4_pad_open(void);

// Virtual mouse driven by the left stick. dx/dy are in engine pixels since the
// previous call; buttons[0] = left (Cross), buttons[1] = right (Circle).
void ps4_pad_get_mouse(int* dx, int* dy, int* left, int* right);

// Pops the next synthetic keyboard event (SDL scancode, down/up) produced by
// pad button presses. Returns 0 when the queue is empty.
int ps4_pad_next_key(int* scancode, int* down);

// System on-screen keyboard for text fields: the typed text is replayed as key events.
void ps4_text_begin(const char* initialText);
void ps4_text_end(void);

// Creates the missing parent directories of an absolute file path (for writes).
void ps4_make_parent_dirs(const char* path);

// Immediate, clean process exit (skips static destructors / SDL_Quit).
void ps4_exit(int code);

#ifdef __cplusplus
}
#endif

#endif
