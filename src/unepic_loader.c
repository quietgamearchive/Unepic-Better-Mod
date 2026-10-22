/* Unepic mod loader -- installed as SDL2.dll.
 *
 * Two jobs:
 *   1. Forward the 14 SDL2 imports to SDL2_orig.dll (the real library).
 *   2. Host the cheat core from mod\unepic_mod_core.dll and hot-reload it:
 *      whenever that file's timestamp/size changes, the running core is asked
 *      to uninstall itself (CoreStop) and a fresh copy is loaded.  The old
 *      core is intentionally NOT unloaded - its code may still be executing
 *      on a game thread for a few instructions, so its memory stays valid.
 *
 * It also applies the startup window size (the exe hardcodes 1024x768 for
 * screen mode 2 at RVA 0x1491) before the game creates its window.
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#define EXP __declspec(dllexport)

/* ---------------- SDL2 forwarding ---------------- */
typedef int  (*fn_u2i_t)(unsigned int);
typedef void (*fn_v_t)(void);
typedef const char *(*fn_s_t)(void);
typedef int  (*fn_p2i_t)(void *);
typedef int  (*fn_pp2i_t)(void *, int);
typedef short (*fn_pp2s_t)(void *, int);
typedef unsigned char (*fn_pp2b_t)(void *, int);
typedef const char *(*fn_i2s_t)(int);
typedef int  (*fn_i2i_t)(int);

enum { P_INIT, P_QUIT, P_ERR, P_NUMJ, P_OPEN, P_CLOSE, P_GETB, P_NUMB,
       P_GETA, P_NUMA, P_GETH, P_NUMH, P_UPD, P_JNAME, P_COUNT };
static const char *g_names[P_COUNT] = {
    "SDL_Init", "SDL_Quit", "SDL_GetError", "SDL_NumJoysticks", "SDL_JoystickOpen",
    "SDL_JoystickClose", "SDL_JoystickGetButton", "SDL_JoystickNumButtons",
    "SDL_JoystickGetAxis", "SDL_JoystickNumAxes", "SDL_JoystickGetHat",
    "SDL_JoystickNumHats", "SDL_JoystickUpdate", "SDL_JoystickName" };
static HMODULE g_real;
static void *g_p[P_COUNT];

EXP int  __cdecl SDL_Init(unsigned int f)                 { return ((fn_u2i_t)g_p[P_INIT])(f); }
EXP void __cdecl SDL_Quit(void)                           { ((fn_v_t)g_p[P_QUIT])(); }
EXP const char *__cdecl SDL_GetError(void)                { return ((fn_s_t)g_p[P_ERR])(); }
EXP int  __cdecl SDL_NumJoysticks(void)                   { return ((fn_p2i_t)g_p[P_NUMJ])(NULL); }
EXP int  __cdecl SDL_JoystickOpen(int i)                  { return ((fn_i2i_t)g_p[P_OPEN])(i); }
EXP void __cdecl SDL_JoystickClose(void *j)               { ((fn_p2i_t)g_p[P_CLOSE])(j); }
EXP int  __cdecl SDL_JoystickGetButton(void *j, int b)    { return ((fn_pp2i_t)g_p[P_GETB])(j, b); }
EXP int  __cdecl SDL_JoystickNumButtons(void *j)          { return ((fn_p2i_t)g_p[P_NUMB])(j); }
EXP short __cdecl SDL_JoystickGetAxis(void *j, int a)     { return ((fn_pp2s_t)g_p[P_GETA])(j, a); }
EXP int  __cdecl SDL_JoystickNumAxes(void *j)             { return ((fn_p2i_t)g_p[P_NUMA])(j); }
EXP unsigned char __cdecl SDL_JoystickGetHat(void *j, int h) { return ((fn_pp2b_t)g_p[P_GETH])(j, h); }
EXP int  __cdecl SDL_JoystickNumHats(void *j)             { return ((fn_p2i_t)g_p[P_NUMH])(j); }
EXP void __cdecl SDL_JoystickUpdate(void)                 { ((fn_v_t)g_p[P_UPD])(); }
EXP const char *__cdecl SDL_JoystickName(int i)           { return ((fn_i2s_t)g_p[P_JNAME])(i); }

/* ---------------- paths / log ---------------- */
static char g_dir[MAX_PATH];        /* game directory (unepic.exe) */
static char g_moddir[MAX_PATH];     /* <game>\mod */
static char g_logpath[MAX_PATH];
static char g_ini[MAX_PATH];

static void logmsg(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = _vsnprintf(buf, sizeof buf - 2, fmt, ap);
    va_end(ap);
    if (n < 0) n = (int)sizeof buf - 2;
    buf[n++] = '\n';
    HANDLE hf = CreateFileA(g_logpath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf != INVALID_HANDLE_VALUE) {
        DWORD w;
        WriteFile(hf, buf, (DWORD)n, &w, NULL);
        CloseHandle(hf);
    }
}

/* ---------------- window size ----------------
 * [video] scale=N  ->  window = 1024*N x 768*N  (1..10, clamped to the desktop)
 * The patch only affects windowed mode: the game uses these constants in its
 * windowed branch and the desktop size in fullscreen, so switching to
 * fullscreen in game keeps working untouched.
 */
static void patch_window_size(void)
{
    int resize = GetPrivateProfileIntA("video", "resize", 1, g_ini);
    int scale  = GetPrivateProfileIntA("video", "scale", 20, g_ini);  /* tenths */
    int aspect = GetPrivateProfileIntA("video", "aspect", 0, g_ini);
    if (aspect < 0 || aspect > 2) aspect = 0;
    int w = GetPrivateProfileIntA("video", "width", 0, g_ini);
    int h = GetPrivateProfileIntA("video", "height", 0, g_ini);
    unsigned char *p;
    static const unsigned char PRO[] = {
        0xBE,0x00,0x04,0x00,0x00, 0xBF,0x00,0x03,0x00,0x00 };
    DWORD old;
    RECT wa;
    int req_w, req_h;

    if (!resize) { logmsg("window size: resize disabled"); return; }
    if (scale < 10) scale *= 10;      /* old whole-number value (2 -> 2.0x) */
    {
        int maxw = GetSystemMetrics(SM_CXSCREEN);
        int maxh = GetSystemMetrics(SM_CYSCREEN);
        int bw = 1024, bh = 768;
        int max10, t;
        if (aspect == 1)      { bw = 1280; bh = 720; }   /* 16:9  */
        else if (aspect == 2) { bw = 1280; bh = 800; }   /* 16:10 */
        max10 = maxw * 10 / bw;
        t = maxh * 10 / bh;
        if (t < max10) max10 = t;
        max10 = max10 / 2 * 2;        /* step 0.2x */
        if (max10 < 10) max10 = 10;
        if (max10 > 50) max10 = 50;
        if (scale > max10) {
            logmsg("scale %d.%dx capped to %d.%dx for aspect %d (screen %dx%d)",
                   scale / 10, scale % 10, max10 / 10, max10 % 10, aspect, maxw, maxh);
            scale = max10;
        }
        if (w > 0 && h > 0) {
            req_w = w; req_h = h;
        } else {
            req_w = (bw * scale + 5) / 10;
            req_h = (bh * scale + 5) / 10;
        }
        w = req_w; h = req_h;
    }
    if (w < 640) w = 640;
    if (h < 480) h = 480;
    p = (unsigned char *)GetModuleHandleA(NULL) + 0x1491;
    if (memcmp(p, PRO, sizeof PRO) != 0) {
        logmsg("window-size pattern differs: %02x %02x %02x %02x %02x | %02x %02x %02x %02x %02x",
               p[0],p[1],p[2],p[3],p[4], p[5],p[6],p[7],p[8],p[9]);
        return;
    }
    if (!VirtualProtect(p, sizeof PRO, PAGE_EXECUTE_READWRITE, &old)) {
        logmsg("window-size VirtualProtect failed: %lu", GetLastError());
        return;
    }
    *(unsigned *)(p + 1) = (unsigned)w;
    *(unsigned *)(p + 6) = (unsigned)h;
    VirtualProtect(p, sizeof PRO, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, sizeof PRO);
    logmsg("window size patched: req %dx%d -> %dx%d (aspect %d scale %d.%dx)",
           req_w, req_h, w, h, aspect, scale / 10, scale % 10);
}

/* Windows' fullscreen optimisation promotes a desktop-sized window to a flip
 * present that bypasses DWM, so topmost overlays (our panel) cannot be drawn
 * over it.  The standard per-app fix is the AppCompat layer flag; it is
 * reversible: delete the exe value under
 *   HKCU\Software\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Layers
 */
static void disable_fso(void)
{
    HKEY h;
    char exe[MAX_PATH], val[512] = {0};
    DWORD sz = sizeof val, type = 0;
    LONG r;
    if (!GetPrivateProfileIntA("video", "disable_fso", 1, g_ini)) return;
    GetModuleFileNameA(NULL, exe, MAX_PATH);
    if (RegCreateKeyExA(HKEY_CURRENT_USER,
            "Software\\Microsoft\\Windows NT\\CurrentVersion\\AppCompatFlags\\Layers",
            0, NULL, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, NULL, &h, NULL) != ERROR_SUCCESS) {
        logmsg("FSO: cannot open AppCompatFlags key");
        return;
    }
    r = RegQueryValueExA(h, exe, NULL, &type, (BYTE *)val, &sz);
    if (r != ERROR_SUCCESS) val[0] = 0;
    if (!strstr(val, "DISABLEDXMAXIMIZEDWINDOWEDMODE")) {
        if (val[0]) strncat(val, " ", sizeof val - strlen(val) - 1);
        strncat(val, "~ DISABLEDXMAXIMIZEDWINDOWEDMODE", sizeof val - strlen(val) - 1);
        if (RegSetValueExA(h, exe, 0, REG_SZ, (BYTE *)val,
                           (DWORD)strlen(val) + 1) == ERROR_SUCCESS)
            logmsg("FSO disabled for unepic.exe (effective next launch)");
        else
            logmsg("FSO: RegSetValueEx failed %lu", GetLastError());
    } else {
        logmsg("FSO already disabled");
    }
    RegCloseKey(h);
}

/* ---------------- core hosting / hot reload ---------------- */
typedef int  (*core_start_t)(void);
typedef void (*core_stop_t)(void);

static char g_core[MAX_PATH];
static core_stop_t g_core_stop;
static unsigned long long g_core_stamp;
static int g_core_no;

static int file_stamp(const char *path, unsigned long long *out)
{
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &d)) return 0;
    *out = ((unsigned long long)d.ftLastWriteTime.dwHighDateTime << 32) |
           (unsigned long long)d.ftLastWriteTime.dwLowDateTime;
    *out ^= ((unsigned long long)d.nFileSizeLow << 40) ^ (unsigned long long)d.nFileSizeHigh;
    return 1;
}

static void core_stop(void)
{
    if (g_core_stop) {
        core_stop_t f = g_core_stop;
        g_core_stop = NULL;
        f();
    }
}

static void core_load(int first)
{
    char tmp[MAX_PATH];
    unsigned long long st;
    HMODULE m;
    core_start_t start;

    if (!file_stamp(g_core, &st)) {
        if (first) logmsg("core not found: %s", g_core);
        return;
    }
    if (st == g_core_stamp) return;
    g_core_stamp = st;

    logmsg("reload: stopping core");
    core_stop();
    logmsg("reload: stopped, copying");
    snprintf(tmp, sizeof tmp, "%s\\core_%d_%lu.dll", g_moddir, ++g_core_no,
             (unsigned long)GetTickCount());
    if (!CopyFileA(g_core, tmp, FALSE)) {
        logmsg("core copy failed: %lu", GetLastError());
        return;
    }
    logmsg("reload: copied -> %s", tmp);
    m = LoadLibraryA(tmp);
    logmsg("reload: LoadLibrary %p err=%lu", (void *)m, GetLastError());
    if (!m) return;
    start = (core_start_t)GetProcAddress(m, "CoreStart");
    g_core_stop = (core_stop_t)GetProcAddress(m, "CoreStop");
    logmsg("reload: start=%p stop=%p", (void *)start, (void *)g_core_stop);
    if (start && start()) logmsg("core #%d loaded", g_core_no);
    else logmsg("core #%d CoreStart failed", g_core_no);
}

static DWORD WINAPI watch(LPVOID p)
{
    (void)p;
    /* outside DllMain: the work-area query and registry access need a thread */
    patch_window_size();
    disable_fso();
    core_load(1);
    for (;;) {
        Sleep(1000);
        core_load(0);
    }
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID reserved)
{
    char self[MAX_PATH], orig[MAX_PATH], *s;
    (void)reserved;
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(h);

    GetModuleFileNameA(h, self, MAX_PATH);
    s = strrchr(self, '\\');
    if (s) *s = 0;
    snprintf(g_dir, sizeof g_dir, "%s", self);

    GetModuleFileNameA(NULL, self, MAX_PATH);      /* game directory */
    s = strrchr(self, '\\');
    if (s) *s = 0;
    if (strcmp(self, g_dir) != 0) snprintf(g_dir, sizeof g_dir, "%s", self);

    snprintf(g_moddir, sizeof g_moddir, "%s\\mod", g_dir);
    snprintf(g_logpath, sizeof g_logpath, "%s\\unepic_mod.log", g_dir);
    snprintf(g_ini, sizeof g_ini, "%s\\unepic_mod.ini", g_dir);
    snprintf(g_core, sizeof g_core, "%s\\unepic_mod_core.dll", g_moddir);
    snprintf(orig, sizeof orig, "%s\\SDL2_orig.dll", g_dir);

    g_real = LoadLibraryA(orig);
    if (!g_real) g_real = LoadLibraryA("SDL2_orig.dll");
    if (g_real) {
        for (int i = 0; i < P_COUNT; i++) g_p[i] = (void *)GetProcAddress(g_real, g_names[i]);
    }

    CreateThread(NULL, 0, watch, NULL, 0, NULL);
    return TRUE;
}
