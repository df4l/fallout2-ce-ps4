#include "ps4_platform.h"

#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include <fcntl.h>
#include <pthread.h>
#include <signal.h>

#include <SDL.h>
#include <orbis/libkernel.h>
#include <orbis/Pad.h>
#include <orbis/UserService.h>
#include <orbis/Sysmodule.h>
#include <orbis/SystemService.h>
#include <orbis/CommonDialog.h>
#include <orbis/ImeDialog.h>

// ---------------------------------------------------------------- logging

void ps4_log(const char* fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    printf("[ps4] %s\n", line);

    FILE* f = fopen(PS4_WORK_DIR "/ps4.log", "a");
    if (f != NULL) {
        fprintf(f, "%s\n", line);
        fclose(f);
    }
}


// ----------------------------------------------------------- crash handler
// Writes the signal, fault address and the code-looking words of the stack to
// ps4.log so that the crash site can be resolved offline with the ELF symbols
// (subtract the load base: &ps4_log at runtime minus its address in the ELF).

#define PS4_SIGILL 4
#define PS4_SIGABRT 6
#define PS4_SIGFPE 8
#define PS4_SIGBUS 10
#define PS4_SIGSEGV 11
#define PS4_SA_SIGINFO 0x40

static void crash_write(int fd, const char* fmt, ...)
{
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n > 0) {
        write(fd, line, n < (int)sizeof(line) ? n : (int)sizeof(line) - 1);
    }
}

static void crash_handler(int sig, siginfo_t* info, void* ucontext)
{
    int fd = open(PS4_WORK_DIR "/ps4.log", O_WRONLY | O_APPEND | O_CREAT, 0644);
    if (fd < 0) {
        _exit(1);
    }

    unsigned long* ctx = (unsigned long*)ucontext;
    // FreeBSD amd64: ucontext_t = { sigset_t (16 bytes); mcontext_t ... }.
    // mcontext offsets: rbp 72, rip 160, rsp 184.
    unsigned long rbp = ctx[(16 + 72) / 8];
    unsigned long rip = ctx[(16 + 160) / 8];
    unsigned long rsp = ctx[(16 + 184) / 8];
    unsigned long base = (unsigned long)&ps4_log;

    crash_write(fd, "CRASH: signal=%d si_addr=%p rip=%#lx rsp=%#lx rbp=%#lx\n", sig, info ? info->si_addr : NULL, rip, rsp, rbp);
    crash_write(fd, "CRASH: &ps4_log=%#lx (rip - &ps4_log = %ld)\n", base, (long)(rip - base));

    unsigned long* sp = (unsigned long*)rsp;
    int logged = 0;
    for (int i = 0; i < 4096 && logged < 40; i++) {
        unsigned long w = sp[i];
        long d = (long)(w - base);
        if (d > -0x700000 && d < 0x200000) {
            crash_write(fd, "CRASH: stack[%d] %#lx (rel %ld)\n", i, w, d);
            logged++;
        }
    }
    close(fd);
    _exit(128 + sig);
}

static void install_crash_handler(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.__sa_handler.__sa_sigaction = crash_handler;
    sa.sa_flags = PS4_SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    int sigs[] = { PS4_SIGILL, PS4_SIGABRT, PS4_SIGFPE, PS4_SIGBUS, PS4_SIGSEGV };
    for (size_t i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++) {
        sigaction(sigs[i], &sa, NULL);
    }
}


// ---------------------------------------------------------------- watchdog
// If the main thread stops beating for PS4_WATCHDOG_SECONDS (after the first
// beat), signal it so the crash handler logs where it is stuck.
#define PS4_WATCHDOG_SECONDS 15

extern "C" int pthread_kill(pthread_t, int);

static volatile unsigned int g_heartbeat = 0;
static pthread_t g_main_thread;

void ps4_heartbeat(void)
{
    g_heartbeat++;
}

unsigned int ps4_ticks_ms(void)
{
    static uint64_t start = 0;
    uint64_t now = sceKernelGetProcessTime();
    if (start == 0)
        start = now;
    return (unsigned int)((now - start) / 1000);
}

static void* watchdog_main(void*)
{
    unsigned int last = 0;
    int stalled = 0;
    int ticks = 0;
    for (;;) {
        sleep(1);
        unsigned int now = g_heartbeat;
        if (++ticks % 5 == 0 && ticks <= 300) {
            ps4_log("wd: alive t=%ds heartbeat=%u ticks=%u sdl=%u", ticks, now, ps4_ticks_ms(), SDL_GetTicks());
        }
        if (now == 0 || now != last) {
            last = now;
            stalled = 0;
            continue;
        }
        if (++stalled >= PS4_WATCHDOG_SECONDS) {
            ps4_log("WATCHDOG: main thread stalled for %d s, signalling it", stalled);
            pthread_kill(g_main_thread, PS4_SIGABRT);
            return NULL;
        }
    }
}

static void log_at_exit(void)
{
    ps4_log("EXIT: atexit handler ran");
}

static void start_watchdog(void)
{
    atexit(log_at_exit);
    g_main_thread = pthread_self();
    pthread_t t;
    pthread_create(&t, NULL, watchdog_main, NULL);
}

// ------------------------------------------------------------ filesystem

static void make_dirs(const char* path)
{
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char* p = tmp + 1; *p != '\0'; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0777);
            *p = '/';
        }
    }
    mkdir(tmp, 0777);
}

static bool file_exists(const char* path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

static bool copy_file(const char* src, const char* dst)
{
    FILE* in = fopen(src, "rb");
    if (in == NULL) {
        return false;
    }
    FILE* out = fopen(dst, "wb");
    if (out == NULL) {
        fclose(in);
        return false;
    }

    char buf[64 * 1024];
    size_t n;
    bool ok = true;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            ok = false;
            break;
        }
    }
    fclose(in);
    fclose(out);
    return ok;
}

// Makes every directory below [path] world-writable (older runs created 0755 dirs).
static void chmod_tree(const char* path)
{
    chmod(path, 0777);
    DIR* dir = opendir(path);
    if (dir == NULL) {
        return;
    }
    struct dirent* e;
    while ((e = readdir(dir)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
            continue;
        }
        char sub[1024];
        snprintf(sub, sizeof(sub), "%s/%s", path, e->d_name);
        struct stat st;
        if (stat(sub, &st) == 0 && S_ISDIR(st.st_mode)) {
            chmod_tree(sub);
        }
    }
    closedir(dir);
}

// Copies a directory tree, never overwriting existing files (keeps saves/edits).
static void copy_tree(const char* src, const char* dst)
{
    DIR* dir = opendir(src);
    if (dir == NULL) {
        return;
    }
    make_dirs(dst);

    struct dirent* e;
    while ((e = readdir(dir)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
            continue;
        }
        char s[1024], d[1024];
        snprintf(s, sizeof(s), "%s/%s", src, e->d_name);
        snprintf(d, sizeof(d), "%s/%s", dst, e->d_name);

        struct stat st;
        if (stat(s, &st) != 0) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            copy_tree(s, d);
        } else if (!file_exists(d)) {
            copy_file(s, d);
        }
    }
    closedir(dir);
}

// The big archives and the music stay in the read-only pkg and are referenced
// by absolute path from the config; everything else lives in the writable
// working dir. Runs on every start so configs seeded by older builds (which
// may hold user settings) get the paths fixed too.
static const char* const kGamedataKeys[] = { "master_dat", "critter_dat", "music_path1", "music_path2" };

static void seed_config(void)
{
    const char* dst = PS4_WORK_DIR "/fallout2.cfg";
    const char* tmp = PS4_WORK_DIR "/fallout2.cfg.tmp";
    const char* src = file_exists(dst) ? dst : PS4_GAMEDATA_DIR "/fallout2.cfg";

    FILE* in = fopen(src, "r");
    FILE* out = fopen(tmp, "w");
    if (in == NULL || out == NULL) {
        ps4_log("cannot seed fallout2.cfg (in=%p out=%p errno=%d)", (void*)in, (void*)out, errno);
        if (in) fclose(in);
        if (out) fclose(out);
        return;
    }

    char line[512];
    int changed = 0;
    while (fgets(line, sizeof(line), in) != NULL) {
        bool rewritten = false;
        for (const char* key : kGamedataKeys) {
            size_t len = strlen(key);
            char* value = line + len + 1;
            if (strncmp(line, key, len) == 0 && line[len] == '=' && *value != '/'
                && *value != '\r' && *value != '\n' && *value != '\0') {
                for (char* p = value; *p != '\0'; p++) {
                    if (*p == '\\') {
                        *p = '/';
                    }
                }
                fprintf(out, "%s=%s/%s", key, PS4_GAMEDATA_DIR, value);
                rewritten = true;
                changed++;
                break;
            }
        }
        if (!rewritten) {
            fputs(line, out);
        }
    }
    fclose(in);
    fclose(out);

    if (src != dst || changed > 0) {
        if (rename(tmp, dst) != 0) {
            ps4_log("cannot replace fallout2.cfg errno=%d", errno);
        } else {
            ps4_log("fallout2.cfg: %d path(s) pointed at " PS4_GAMEDATA_DIR, changed);
        }
    } else {
        unlink(tmp);
    }
}

void ps4_make_parent_dirs(const char* path)
{
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", path);
    char* slash = strrchr(tmp, '/');
    if (slash != NULL && slash != tmp) {
        *slash = '\0';
        make_dirs(tmp);
    }
}

void ps4_exit(int code)
{
    ps4_log("EXIT: ps4_exit(%d)", code);
    // Ask the system to close the app instead of killing the process.
    int rc = sceSystemServiceLoadExec("exit", NULL);
    ps4_log("EXIT: LoadExec returned 0x%x", rc);
    sceKernelUsleep(2000000);
    _exit(code);
}

int ps4_init_filesystem(void)
{
    umask(0);
    install_crash_handler();
    start_watchdog();
    make_dirs(PS4_WORK_DIR);
    ps4_log("---- start ----");

    if (!file_exists(PS4_GAMEDATA_DIR "/master.dat")) {
        ps4_log("missing " PS4_GAMEDATA_DIR "/master.dat");
        return -1;
    }

    seed_config();

    const char* small_files[] = { "f2_res.ini", "f2_res.dat", "patch000.dat" };
    for (size_t i = 0; i < sizeof(small_files) / sizeof(small_files[0]); i++) {
        char s[256], d[256];
        snprintf(s, sizeof(s), "%s/%s", PS4_GAMEDATA_DIR, small_files[i]);
        snprintf(d, sizeof(d), "%s/%s", PS4_WORK_DIR, small_files[i]);
        if (file_exists(s) && !file_exists(d) && !copy_file(s, d)) {
            ps4_log("failed to copy %s (errno=%d)", small_files[i], errno);
        }
    }
    copy_tree(PS4_GAMEDATA_DIR "/data", PS4_WORK_DIR "/data");
    chmod_tree(PS4_WORK_DIR "/data");
    make_dirs(PS4_WORK_DIR "/data/SAVEGAME");
    make_dirs(PS4_WORK_DIR "/data/MAPS");
    make_dirs(PS4_WORK_DIR "/MAPS"); // automap fallback path used by the engine
    make_dirs(PS4_WORK_DIR "/SAVEGAME");

    // chdir() is refused by the Orbis sandbox (EPERM), so the engine's relative
    // paths are resolved against PS4_WORK_DIR by ps4_abs_path() instead.
    ps4_log("working dir ready: " PS4_WORK_DIR);
    return 0;
}

void ps4_abs_path(const char* in, char* out, size_t cap)
{
    if (in[0] == '/') {
        snprintf(out, cap, "%s", in);
    } else {
        // The engine mixes "SAVEGAME\..." / "MAPS\..." / "proto\..." (resolved
        // against the work dir) with "data\SAVEGAME\..." etc. (master_patches).
        // Fold the second form onto the first so both name the same files.
        const char* rel = in;
        if (strncasecmp(rel, "data", 4) == 0 && (rel[4] == '\\' || rel[4] == '/')) {
            static const char* const kFolded[] = { "SAVEGAME", "MAPS", "proto" };
            const char* sub = rel + 5;
            for (const char* name : kFolded) {
                size_t len = strlen(name);
                if (strncasecmp(sub, name, len) == 0
                    && (sub[len] == '\0' || sub[len] == '\\' || sub[len] == '/')) {
                    rel = sub;
                    break;
                }
            }
        }
        snprintf(out, cap, "%s/%s", PS4_WORK_DIR, rel);
    }
    for (char* p = out; *p != '\0'; p++) {
        if (*p == '\\') {
            *p = '/';
        }
    }
}

char* ps4_getcwd(char* buf, size_t size)
{
    if (strlen(PS4_WORK_DIR) + 1 > size) {
        return nullptr;
    }
    strcpy(buf, PS4_WORK_DIR);
    return buf;
}

// chdir() replacement used by the engine only as a "directory exists" probe.
int ps4_chdir(const char* path)
{
    char abs[1024];
    ps4_abs_path(path, abs, sizeof(abs));
    struct stat st;
    if (stat(abs, &st) != 0 || !S_ISDIR(st.st_mode)) {
        return -1;
    }
    return 0;
}

// ---------------------------------------------------------------- gamepad
// Button indices of the SDL2 PS4 joystick driver (see samples/SDL2/SDL2/Game.h).
enum {
    PAD_CROSS = 0,
    PAD_CIRCLE = 1,
    PAD_SQUARE = 2,
    PAD_TRIANGLE = 3,
    PAD_L1 = 4,
    PAD_R1 = 5,
    PAD_OPTIONS = 9,
    PAD_L3 = 11,
    PAD_R3 = 12,
    PAD_UP = 13,
    PAD_DOWN = 14,
    PAD_LEFT = 15,
    PAD_RIGHT = 16,
    PAD_TOUCHPAD = 17,
    PAD_L2 = 18,
    PAD_R2 = 19,
    PAD_BUTTON_COUNT = 20,
};

struct KeyBinding {
    int button;
    SDL_Scancode key;
};

// Mouse-driven game: Cross/Circle are the mouse buttons, the rest are hotkeys.
static const KeyBinding kBindings[] = {
    { PAD_OPTIONS, SDL_SCANCODE_ESCAPE },
    { PAD_SQUARE, SDL_SCANCODE_RETURN },
    { PAD_TRIANGLE, SDL_SCANCODE_TAB }, // automap
    { PAD_L1, SDL_SCANCODE_I }, // inventory
    { PAD_R1, SDL_SCANCODE_C }, // character
    { PAD_L2, SDL_SCANCODE_P }, // pip-boy
    { PAD_R2, SDL_SCANCODE_SPACE }, // end turn
    { PAD_TOUCHPAD, SDL_SCANCODE_F6 }, // quick save
    { PAD_L3, SDL_SCANCODE_F7 }, // quick load
    { PAD_UP, SDL_SCANCODE_UP },
    { PAD_DOWN, SDL_SCANCODE_DOWN },
    { PAD_LEFT, SDL_SCANCODE_LEFT },
    { PAD_RIGHT, SDL_SCANCODE_RIGHT },
};
static const int kBindingCount = sizeof(kBindings) / sizeof(kBindings[0]);

static SDL_Joystick* gPad = NULL;
static bool gPrevButtons[PAD_BUTTON_COUNT];
static bool gSlowMode = false;
static float gRemX = 0.0f;
static float gRemY = 0.0f;
static Uint32 gLastMouseTick = 0;

// Pending synthetic key events (small ring buffer).
static struct {
    int scancode;
    int down;
} gKeyQueue[64];
static int gKeyHead = 0;
static int gKeyTail = 0;

static void push_key(int scancode, int down)
{
    int next = (gKeyTail + 1) % 64;
    if (next == gKeyHead) {
        return;
    }
    gKeyQueue[gKeyTail].scancode = scancode;
    gKeyQueue[gKeyTail].down = down;
    gKeyTail = next;
}

void ps4_pad_open(void)
{
    if (gPad != NULL) {
        return;
    }
    if (!SDL_WasInit(SDL_INIT_JOYSTICK)) {
        if (SDL_InitSubSystem(SDL_INIT_JOYSTICK) != 0) {
            return;
        }
    }
    if (SDL_NumJoysticks() > 0) {
        gPad = SDL_JoystickOpen(0);
        ps4_log("gamepad %s", gPad != NULL ? "opened" : "open failed");
    }
}

static bool button(int index)
{
    return gPad != NULL && SDL_JoystickGetButton(gPad, index) != 0;
}

// Touchpad click toggles a menu: first press sends F6, second press sends Esc.
static bool gTouchpadMenuOpen = false;
static int gTouchpadKey = 0;

static void update_buttons(void)
{
    for (int i = 0; i < kBindingCount; i++) {
        int b = kBindings[i].button;
        bool now = button(b);
        if (now != gPrevButtons[b]) {
            if (b == PAD_TOUCHPAD) {
                if (now) {
                    gTouchpadKey = gTouchpadMenuOpen ? SDL_SCANCODE_ESCAPE : kBindings[i].key;
                    gTouchpadMenuOpen = !gTouchpadMenuOpen;
                    push_key(gTouchpadKey, 1);
                } else {
                    push_key(gTouchpadKey, 0);
                }
                continue;
            }
            if (now && (b == PAD_OPTIONS || b == PAD_CIRCLE)) {
                gTouchpadMenuOpen = false; // menu was closed some other way
            }
            push_key(kBindings[i].key, now ? 1 : 0);
        }
    }
    // Button state is stored after the key pass; mouse buttons read live state.
    for (int b = 0; b < PAD_BUTTON_COUNT; b++) {
        bool now = button(b);
        if (b == PAD_R3 && now && !gPrevButtons[b]) {
            gSlowMode = !gSlowMode;
        }
        gPrevButtons[b] = now;
    }
}

// Raw stick bytes straight from ScePad (SDL's PS4 joystick axes were seen to
// stay latched at the last non-centered value after the stick is released).
static int gRawPad = -1;
static bool gRawTried = false;
static uint8_t gRawAxis[2] = { 128, 128 };
static bool gRawValid = false;
static float gTouchDx = 0.0f; // touchpad finger movement since last read, in pad units
static float gTouchDy = 0.0f;
static bool gTouchDown = false;
static int gTouchPrevX = 0;
static int gTouchPrevY = 0;
// Tap-to-click: a short, nearly motionless touch acts like a Cross press.
static Uint32 gTouchStartTick = 0;
static float gTouchTravel = 0.0f;
static bool gTouchCancelTap = false;
static Uint32 gTapClickUntil = 0;

static void read_raw_sticks(void)
{
    gRawValid = false;
    if (!gRawTried) {
        gRawTried = true;
        int32_t user = 0;
        int rc = sceUserServiceGetInitialUser(&user);
        gRawPad = rc == 0 ? scePadGetHandle(user, 0, 0) : -1;
        ps4_log("pad: raw handle=%d (user rc=%d id=%d)", gRawPad, rc, (int)user);
    }
    if (gRawPad < 0) {
        return;
    }
    OrbisPadData d;
    memset(&d, 0, sizeof(d));
    if (scePadReadState(gRawPad, &d) != 0 || !d.connected) {
        return;
    }
    gTouchDx = 0.0f;
    gTouchDy = 0.0f;
    if (d.touch.fingers > 1 || (d.buttons & ORBIS_PAD_BUTTON_TOUCH_PAD)) {
        gTouchCancelTap = true; // multi-finger or physical click: not a tap
    }
    if (d.touch.fingers > 0) {
        int tx = d.touch.touch[0].x;
        int ty = d.touch.touch[0].y;
        if (gTouchDown) {
            gTouchDx = (float)(tx - gTouchPrevX);
            gTouchDy = (float)(ty - gTouchPrevY);
            gTouchTravel += fabsf(gTouchDx) + fabsf(gTouchDy);
        } else {
            gTouchStartTick = SDL_GetTicks();
            gTouchTravel = 0.0f;
            gTouchCancelTap = d.touch.fingers > 1;
        }
        gTouchPrevX = tx;
        gTouchPrevY = ty;
        gTouchDown = true;
    } else {
        if (gTouchDown && !gTouchCancelTap && gTouchTravel < 40.0f) {
            Uint32 t = SDL_GetTicks();
            if (t - gTouchStartTick < 250) {
                gTapClickUntil = t + 100; // hold long enough for the engine to see it
            }
        }
        gTouchDown = false;
    }
    gRawAxis[0] = d.leftStick.x;
    gRawAxis[1] = d.leftStick.y;
    gRawValid = true;
}

static float stick_axis(int axis)
{
    float v;
    if (gRawValid) {
        v = ((int)gRawAxis[axis] - 128) / 127.0f;
    } else {
        v = SDL_JoystickGetAxis(gPad, axis) / 32767.0f;
    }
    const float dead = 0.2f;
    float a = fabsf(v);
    if (a < dead) {
        return 0.0f;
    }
    if (a > 1.0f) {
        a = 1.0f;
    }
    a = (a - dead) / (1.0f - dead);
    a = a * a; // finer control near the center
    return v < 0 ? -a : a;
}

void ps4_pad_get_mouse(int* dx, int* dy, int* left, int* right)
{
    ps4_heartbeat();
    *dx = 0;
    *dy = 0;
    *left = 0;
    *right = 0;

    ps4_pad_open();
    if (gPad == NULL) {
        return;
    }
    SDL_JoystickUpdate();
    update_buttons();
    read_raw_sticks();

    static int logCount = 0;
    if (logCount < 40 && (stick_axis(0) != 0.0f || stick_axis(1) != 0.0f || gRawAxis[0] != 128)) {
        logCount++;
        ps4_log("pad: raw=%d,%d valid=%d sdl=%d,%d", gRawAxis[0], gRawAxis[1], (int)gRawValid,
            SDL_JoystickGetAxis(gPad, 0), SDL_JoystickGetAxis(gPad, 1));
    }

    Uint32 now = SDL_GetTicks();
    float dt = gLastMouseTick == 0 ? 0.0f : (now - gLastMouseTick) / 1000.0f;
    gLastMouseTick = now;
    if (dt > 0.1f) {
        dt = 0.1f;
    }

    const float speed = gSlowMode ? 150.0f : 520.0f; // engine pixels per second
    gRemX += stick_axis(0) * speed * dt;
    gRemY += stick_axis(1) * speed * dt;
    // Touchpad: relative, precise cursor control (pad is 1920 wide, game ~1024).
    const float touchScale = 0.56f; // 20% slower than the original 0.7
    if (gRawValid) {
        gRemX += gTouchDx * touchScale;
        gRemY += gTouchDy * touchScale;
    }

    *dx = (int)gRemX;
    *dy = (int)gRemY;
    gRemX -= *dx;
    gRemY -= *dy;

    *left = (button(PAD_CROSS) || SDL_GetTicks() < gTapClickUntil) ? 1 : 0;
    *right = button(PAD_CIRCLE) ? 1 : 0;
}


// ------------------------------------------------------- on-screen keyboard
// The engine reads typed characters from keyboard scancodes, so the system IME
// dialog result is replayed as synthetic key events (paced, to not overflow the
// engine's key queue).
static const int kImeMaxLen = 32;
static bool gImeActive = false;
static bool gImeModulesTried = false;
static uint16_t gImeText[kImeMaxLen + 1];
static int gImeInitLen = 0;
static uint16_t gImeTitle[16];

static struct {
    int scancode;
    int down;
} gInject[1024];
static int gInjHead = 0;
static int gInjTail = 0;
static unsigned int gInjLastTick = 0;
static int gInjBudget = 0;

static void inject(int scancode, int down)
{
    int next = (gInjTail + 1) % 1024;
    if (next == gInjHead) {
        return;
    }
    gInject[gInjTail].scancode = scancode;
    gInject[gInjTail].down = down;
    gInjTail = next;
}

static void inject_tap(int scancode)
{
    inject(scancode, 1);
    inject(scancode, 0);
}

static void inject_char(int c)
{
    int sc = 0;
    bool shift = false;
    if (c >= 'a' && c <= 'z') {
        sc = SDL_SCANCODE_A + (c - 'a');
    } else if (c >= 'A' && c <= 'Z') {
        sc = SDL_SCANCODE_A + (c - 'A');
        shift = true;
    } else if (c >= '1' && c <= '9') {
        sc = SDL_SCANCODE_1 + (c - '1');
    } else if (c == '0') {
        sc = SDL_SCANCODE_0;
    } else if (c == ' ') {
        sc = SDL_SCANCODE_SPACE;
    } else if (c == '-') {
        sc = SDL_SCANCODE_MINUS;
    } else if (c == '_') {
        sc = SDL_SCANCODE_MINUS;
        shift = true;
    } else if (c == '.') {
        sc = SDL_SCANCODE_PERIOD;
    } else if (c == ',') {
        sc = SDL_SCANCODE_COMMA;
    } else if (c == '\'') {
        sc = SDL_SCANCODE_APOSTROPHE;
    }
    if (sc == 0) {
        return;
    }
    if (shift) {
        inject(SDL_SCANCODE_LSHIFT, 1);
    }
    inject_tap(sc);
    if (shift) {
        inject(SDL_SCANCODE_LSHIFT, 0);
    }
}

void ps4_text_begin(const char* initialText)
{
    if (gImeActive) {
        return;
    }
    if (!gImeModulesTried) {
        gImeModulesTried = true;
        int r1 = sceSysmoduleLoadModule((enum OrbisSysModule)ORBIS_SYSMODULE_INTERNAL_COMMON_DIALOG);
        int r2 = sceSysmoduleLoadModule(ORBIS_SYSMODULE_IME_DIALOG);
        int r3 = sceCommonDialogInitialize();
        ps4_log("ime: modules common=0x%x ime=0x%x init=0x%x", r1, r2, r3);
    }
    int32_t user = 0;
    sceUserServiceGetInitialUser(&user);

    // Prefill the dialog with the field's current text (save description,
    // character name...) so it can be edited instead of retyped.
    memset(gImeText, 0, sizeof(gImeText));
    gImeInitLen = 0;
    if (initialText != NULL) {
        while (gImeInitLen < kImeMaxLen && initialText[gImeInitLen] != '\0') {
            gImeText[gImeInitLen] = (unsigned char)initialText[gImeInitLen];
            gImeInitLen++;
        }
    }
    const char* title = "Name";
    for (int i = 0; title[i] != '\0' && i < 15; i++) {
        gImeTitle[i] = (uint16_t)title[i];
    }

    OrbisImeDialogSetting p;
    memset(&p, 0, sizeof(p));
    p.userId = (uint32_t)user;
    p.type = ORBIS_TYPE_BASIC_LATIN;
    p.supportedLanguages = 0;
    p.enterLabel = ORBIS_BUTTON_LABEL_DEFAULT;
    p.inputMethod = ORBIS__DEFAULT;
    p.filter = NULL;
    p.option = 0;
    p.maxTextLength = kImeMaxLen;
    p.inputTextBuffer = (wchar_t*)gImeText;
    p.posx = PS4_SCREEN_WIDTH / 2.0f;
    p.posy = PS4_SCREEN_HEIGHT / 2.0f;
    p.horizontalAlignment = ORBIS_H_CENTER;
    p.verticalAlignment = ORBIS_V_CENTER;
    p.placeholder = NULL;
    p.title = (const wchar_t*)gImeTitle;

    int rc = sceImeDialogInit(&p, NULL);
    ps4_log("ime: sceImeDialogInit -> 0x%x", rc);
    gImeActive = rc == 0;
}

void ps4_text_end(void)
{
    if (gImeActive) {
        sceImeDialogForceClose();
        sceImeDialogTerm();
        gImeActive = false;
    }
}

static void ime_poll(void)
{
    if (!gImeActive || sceImeDialogGetStatus() != ORBIS_DIALOG_STATUS_STOPPED) {
        return;
    }
    OrbisDialogResult result;
    memset(&result, 0, sizeof(result));
    sceImeDialogGetResult(&result);
    sceImeDialogTerm();
    gImeActive = false;
    ps4_log("ime: closed endstatus=%d", (int)result.endstatus);

    if (result.endstatus != ORBIS_DIALOG_OK) {
        inject_tap(SDL_SCANCODE_ESCAPE);
        return;
    }
    // Clear the field's current text (what the dialog was prefilled with).
    int clear = gImeInitLen > 12 ? gImeInitLen : 12;
    for (int i = 0; i < clear; i++) {
        inject_tap(SDL_SCANCODE_BACKSPACE);
    }
    for (int i = 0; i < kImeMaxLen && gImeText[i] != 0; i++) {
        inject_char(gImeText[i] < 128 ? (int)gImeText[i] : 0);
    }
    inject_tap(SDL_SCANCODE_RETURN);
}

int ps4_pad_next_key(int* scancode, int* down)
{
    ps4_pad_open();
    if (gPad != NULL) {
        SDL_JoystickUpdate();
        update_buttons();
    }
    ime_poll();
    if (gKeyHead == gKeyTail && gInjHead != gInjTail) {
        // Budget of one key tap (down + up) per engine frame.
        unsigned int t = ps4_ticks_ms();
        if (t - gInjLastTick >= 30) {
            gInjLastTick = t;
            gInjBudget = 2;
        }
        if (gInjBudget > 0) {
            gInjBudget--;
            *scancode = gInject[gInjHead].scancode;
            *down = gInject[gInjHead].down;
            gInjHead = (gInjHead + 1) % 1024;
            return 1;
        }
    }
    if (gKeyHead == gKeyTail) {
        return 0;
    }
    *scancode = gKeyQueue[gKeyHead].scancode;
    *down = gKeyQueue[gKeyHead].down;
    gKeyHead = (gKeyHead + 1) % 64;
    return 1;
}
