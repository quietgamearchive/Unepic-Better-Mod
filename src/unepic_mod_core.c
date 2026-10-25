/* Unepic v1.51 (build 15101) cheat core.
 *
 * Loaded by SDL2.dll (loader) from mod\unepic_mod_core.dll.  The loader
 * forwards the SDL2 imports, patches the startup window size and hot-reloads
 * this file; this DLL owns the hooks, the panel and unepic_mod.ini.
 *
 * Verified layout (non-ASLR exe, base 0x400000):
 *   [0x0071C5C0] -> stats: +0x160 xp, +0x164 need, +0x168 level, +0x16C points
 *                   (mirrors +0x17C..+0x188, masks 0x67232C / 0x672330),
 *                   +0xD18 tamper flag, +0x17F78 -> player entity
 *   entity+0x470 max hp (int), entity+0x474 hp (float)
 *   0x0049F490 damage_int(int)      real player damage (monsters / traps)
 *   0x00445A40 add_hp(float)        healing path
 *   0x0049F730 hurt(float,...)      secondary damage path
 *   0x00423480 stack consume(int n) potions / arrows / keys
 *   0x00422520 masked count setter  used only by the savegame loader
 */
#include <windows.h>
#include <windowsx.h>
#include <tlhelp32.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <wchar.h>
#include <wctype.h>
#include <imm.h>

#define EXP __declspec(dllexport)

static const int ENABLE_PATCH   = 1;
static const int NEUTRALISE_FLAG = 1;
static const int USE_FREEZE      = 0;

/* ---- confirmed stack types: 6005 = healing potion.  Extend from the log. ---- */
#define WHITEMASK_A 6005
#define WHITEMASK_B (-1)          /* unused slot, never matches */

#define ADDR_MASK1     0x0067232Cu
#define ADDR_MASK2     0x00672330u
#define ADDR_STATS_PTR 0x0071C5C0u
#define STATS_ALT      0x00DA7270u      /* hardcoded in the exe at 0x00445A9B */
#define ADDR_MP_MODE   0x00672334u      /* 0 = single-player, non-zero = multiplayer */
#define ITEM_COIN      9101             /* money record, in both game modes */
#define ITEM_UN        9102             /* UN-points record, rebuilt from stats+0x170 */

#define OFF_ENTITY   0x17F78
#define OFF_LEVEL_OBJ 0x17F7C           /* the loaded level object (0 outside) */
#define LEVEL_GROUND_OFF 0xBCA4         /* its pile of items lying on the floor */
#define OFF_XP       0x0160
#define OFF_NEED     0x0164
#define OFF_LEVEL    0x0168
#define OFF_POINTS   0x016C
#define OFF_MIRROR   0x001C            /* mirror = primary + OFF_MIRROR for these 4 */
#define OFF_HUD_MAXHP  0x01A4
#define OFF_HUD_CURHP  0x01A8
#define OFF_TAMPER   0x0D18

#define ENT_MAXHP    0x470
#define ENT_CURHP    0x474

#define FN_ADDHP     0x00445A40u
#define FN_HURT      0x0049F730u      /* entity::hurt(float amount, int, int, int) */
#define FN_HURT2     0x0049F490u      /* entity::damage_int(int amount)            */
#define FN_CONSUME   0x00423480u      /* stack consume(int n) -> 1 left / 0 removed */
#define FN_COUNT_SET 0x00422520u
#define SAVE_SCRATCH 0x00146A980u  /* the game's own save scratch buffer */

static char g_dir[MAX_PATH], g_logpath[MAX_PATH], g_ini[MAX_PATH];
static HANDLE g_worker;
static DWORD  g_wtid;
static volatile LONG g_stopping;
static volatile LONG g_speed_pct = 100;
static volatile LONG g_airjump = 0;
static volatile LONG g_jump_vk;
static volatile LONG g_jump_down;        /* edge tracker: ignore key auto-repeat */
static int g_jump_key_forced;            /* [cheats] jump_key overrides learning */
static int g_mp_tp = 1;                  /* [cheats] mp_tp: in-room teleport in MP */
static volatile LONG g_aj_probe_t0;      /* airjump probe: tick after the vy write */
static volatile LONG g_aj_probe_ent;
static volatile LONG g_aj_probe_y0;      /* float bits, before the write */
static volatile LONG g_aj_probe_vy0;     /* float bits, before the write */
static volatile LONG g_aj_retry_until;   /* watch the impulse until this tick */
static volatile LONG g_aj_retry_left;    /* how many restores are still allowed */
static HANDLE g_netsync;                 /* multiplayer position catch-up thread */
static int g_netsync_on;                 /* [debug] netsync: 0 = park the thread */
static HANDLE g_godguard;                /* instant-death undo thread (god mode) */
/* An air jump must feel like a real ground jump.  Measured takeoff vy of the
 * game's own jump: -339 .. -368 (trace, 2026-10-22).  The old code "learned" the
 * impulse from whatever frame it sampled and once caught a decayed -138, which
 * is why the extra jumps had almost no height and looked like hovering; the
 * other half of that symptom was auto-repeat firing the write ~20x per second,
 * re-launching from a fixed small vy.  Fixed value, at the strong end. */
#define AIRJUMP_IMPULSE (-368.0f)
/* right-click teleport: world = room centre + (cursor - client centre) * scale,
 * where scale is [video] aspect base / client size (0.625 world units per client
 * pixel at scale 16, 1280 wide inside a 2048 wide window), corrected by
 * [cheats] tp_scale=100 (percent, read on every click, no restart).
 * The derived point is then resolved by the room grid - see grid_find_landing. */
static volatile LONG g_teleport = 0;
/* per-effect switches, index = effect id: g_buff_on keeps a positive effect at
 * max duration, g_debuff_on clears a negative one every worker tick.  The
 * Buffs window edits these; they are saved in mod\buffs.ini. */
static unsigned char g_buff_on[110];
static unsigned char g_debuff_on[110];
/* slot the saves window asked the game to load; consumed by WM_APP+18 */
static volatile LONG g_load_slot = -1;
static float g_tp_scale = 1.0f;
static volatile LONG g_tp_check_tick;
static volatile LONG g_tp_old_x, g_tp_old_y, g_tp_new_x, g_tp_new_y;   /* float bits */
static volatile LONG g_tp_ent;    /* entity the last teleport moved      */
static HWND g_cb_tp;
static HHOOK g_mouse;
static volatile LONG g_last_vk;
static volatile LONG g_last_vk_tick;
static float g_prev_vy;
static float g_speed_mul = 1.0f;
static volatile LONG g_atkspd = 10;      /* attack speed, tenths (10..50) */
static float g_atk_div = 1.0f;           /* attack phase duration is divided by this */
static volatile LONG g_player_x;
static volatile LONG g_speed_cnt;
static volatile LONG g_last_speed;
static volatile LONG g_tp_guard_until;  /* position guard ignores jumps until this tick */
static volatile LONG g_last_x_eip;      /* last instruction seen writing entity+0x520 */
static LONG g_speed_last_cnt;
static void ime_guard(void);
static int  un_current(void);
static void panel_show(void);
static void panel_hide(void);
static HWND find_game_window(void);
static int  game_windowed(void);
static void hook_steam_write(void);
static void report_steam_write(void);

#if 0
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
static char g_logpath[MAX_PATH] = "unepic_mod.log";
static char g_dir[MAX_PATH] = ".";

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

int  SDLCALL SDL_Init(unsigned int f)                 { return ((fn_u2i_t)g_p[P_INIT])(f); }
void SDLCALL SDL_Quit(void)                           { ((fn_v_t)g_p[P_QUIT])(); }
const char *SDLCALL SDL_GetError(void)                { return ((fn_s_t)g_p[P_ERR])(); }
int  SDLCALL SDL_NumJoysticks(void)                   { return ((fn_p2i_t)g_p[P_NUMJ])(NULL); }
int  SDLCALL SDL_JoystickOpen(int i)                  { return ((fn_i2i_t)g_p[P_OPEN])(i); }
void SDLCALL SDL_JoystickClose(void *j)               { ((fn_p2i_t)g_p[P_CLOSE])(j); }
int  SDLCALL SDL_JoystickGetButton(void *j, int b)    { return ((fn_pp2i_t)g_p[P_GETB])(j, b); }
int  SDLCALL SDL_JoystickNumButtons(void *j)          { return ((fn_p2i_t)g_p[P_NUMB])(j); }
short SDLCALL SDL_JoystickGetAxis(void *j, int a)     { return ((fn_pp2s_t)g_p[P_GETA])(j, a); }
int  SDLCALL SDL_JoystickNumAxes(void *j)             { return ((fn_p2i_t)g_p[P_NUMA])(j); }
unsigned char SDLCALL SDL_JoystickGetHat(void *j, int h) { return ((fn_pp2b_t)g_p[P_GETH])(j, h); }
int  SDLCALL SDL_JoystickNumHats(void *j)             { return ((fn_p2i_t)g_p[P_NUMH])(j); }
void SDLCALL SDL_JoystickUpdate(void)                 { ((fn_v_t)g_p[P_UPD])(); }
const char *SDLCALL SDL_JoystickName(int i)           { return ((fn_i2s_t)g_p[P_JNAME])(i); }
#endif /* SDL2 forwarding */

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

/* ---------------- cheat state (read straight from the asm handlers) -------- */
static volatile LONG g_god    = 0;   /* god mode - no damage                */
static volatile LONG g_maxxp  = 0;   /* keep XP at the next-level requirement */
static volatile LONG g_nocons = 1;   /* consumables are not used up         */
static volatile LONG g_nogun  = 1;   /* ranged ammo/durability is not spent ([cheats] nogun) */

/* diagnostics filled in by the add_hp handler */
static volatile LONG g_hp_calls;      /* every call                          */
static volatile LONG g_hp_player;     /* call on the player entity           */
static volatile LONG g_hp_neg;        /* call on the player with delta < 0   */
static volatile LONG g_hp_lastthis;
static volatile LONG g_hp_lastret;
static volatile LONG g_hp_lastdelta;
static volatile LONG g_clamp_hits;    /* item-count decreases refused        */

/* diagnostics for entity::hurt (0x0049F730) -- the real damage path */
static volatile LONG g_dm_calls;
static volatile LONG g_dm_player;
static volatile LONG g_dm_blocked;
static volatile LONG g_dm_lastthis;
static volatile LONG g_dm_lastret;
static volatile LONG g_dm_lastamt;    /* float bits of the damage amount     */

/* last item-count decrease seen, whatever the type */
static volatile LONG g_dec_total;
static volatile LONG g_dec_type;
static volatile LONG g_dec_old;
static volatile LONG g_dec_new;
static volatile LONG g_dec_ret;
static volatile LONG g_dec_slot;

/* observation of the masked count ADDERS - 0x00422540 (+0x04 quantity),
 * 0x00422660 (+0x0C shots/uses) and 0x004225A0 (+0x20).  The first two also
 * refuse a decrease that lands on the player's own bag when [cheats] nocons is
 * on (that is what "Infinite items" now really means - see patch_count_adder);
 * the last one is capture only.  g_mu_* hold the most recent call so the
 * periodic report can show caller / record / type / old->new / where. */
static volatile LONG g_mu_calls;
static volatile LONG g_mu_dec;
static volatile LONG g_mu_422540;
static volatile LONG g_mu_422660;
static volatile LONG g_mu_4225a0;
static volatile LONG g_mu_slot;
static volatile LONG g_mu_delta;
static volatile LONG g_mu_ret;
static volatile LONG g_mu_old;
static volatile LONG g_mu_new;
static volatile LONG g_mu_type;
static volatile LONG g_mu_src;
static volatile LONG g_mu_blocked;

/* diagnostics for entity::damage_int (0x0049F490) -- the real damage path */
static volatile LONG g_d2_calls;
static volatile LONG g_d2_player;
static volatile LONG g_d2_blocked;
static volatile LONG g_d2_lastthis;
static volatile LONG g_d2_lastret;
static volatile LONG g_d2_lastamt;

/* diagnostics for stack consume (0x00423480) */
static volatile LONG g_it_calls;
static volatile LONG g_it_blocked;
static volatile LONG g_it_lasttype;
static volatile LONG g_it_lastn;
static volatile LONG g_it_lastret;

/* ---- hardware write watchpoint on the player's HP (diagnostic) -------------
 * Sets DR0 = entity+0x474 on every other thread of this process.  Any write
 * raises EXCEPTION_SINGLE_STEP with Dr6 bit0 set; we log the writer's EIP and
 * let the write happen.  This finds damage paths no matter how they are coded.
 */
static const int DIAG_WP = 0;      /* hardware watchpoint, for reverse engineering */
static volatile LONG g_wp_n;          /* total watchpoint hits             */
static volatile LONG g_wp_eip[64];    /* distinct writer EIPs              */
static volatile LONG g_wp_cnt[64];    /* hits per writer since the report  */
static volatile LONG g_wp_seen;       /* number of distinct EIPs           */
static volatile LONG g_wp_period;     /* hits since the last report        */
static volatile LONG g_wp_addr;
static DWORD g_wp_tid;              /* game window thread for DR0 (0 = all threads) */
static unsigned g_watch_ent_off;    /* [debug] watchent=offset, resolved from the entity */
static int g_watch_rw;              /* [debug] watchrw=1 -> trap reads as well as writes */
static int g_watch_net;             /* [debug] watchnet=1 -> watch the net player's x */
static PVOID g_veh;

static LONG CALLBACK veh_handler(EXCEPTION_POINTERS *ep)
{
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP)
        return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT *c = ep->ContextRecord;
    if (!(c->Dr6 & 0x1)) return EXCEPTION_CONTINUE_SEARCH;
    {
        LONG eip = (LONG)c->Eip, k, n;
        InterlockedIncrement(&g_wp_n);
        InterlockedIncrement(&g_wp_period);
        n = g_wp_seen;
        for (k = 0; k < n && k < 64; k++)
            if (g_wp_eip[k] == eip) { InterlockedIncrement(&g_wp_cnt[k]); break; }
        if (k == n && n < 64) {
            unsigned *sp = (unsigned *)c->Esp;
            unsigned char *ip = (unsigned char *)c->Eip;
            g_wp_eip[n] = eip;
            g_wp_cnt[n] = 1;
            InterlockedIncrement(&g_wp_seen);
            if (!IsBadReadPtr(sp, 28) && !IsBadReadPtr(ip, 6))
                logmsg("WATCH eip=%08x edi=%08x esi=%08x op=%02x %02x %02x %02x "
                       "sp: %08x %08x %08x %08x %08x %08x %08x",
                       (unsigned)eip, (unsigned)c->Edi, (unsigned)c->Esi,
                       ip[0], ip[1], ip[2], ip[3],
                       sp[0], sp[1], sp[2], sp[3], sp[4], sp[5], sp[6]);
        }
    }
    /* The value checks below read the watched address.  That is fine for a
     * write-only breakpoint, but with [debug] watchrw=1 the read itself raises
     * the same breakpoint again: the handler recursed until the game died.
     * Read+write mode only logs the unique EIPs above; it must not touch the
     * watched data. */
    if (!g_watch_rw) {
        /* Death-state marker: when the watched field is the entity state
         * (+0x540) and the value that was just written is 118, log the writer.
         * This is the instant-kill path (spikes) that does not go through
         * damage_int: measured 2026-10-24, HP 330 -> 0 and state 118 in the
         * same tick. */
        if (g_watch_ent_off == 0x540 && g_wp_addr &&
            !IsBadReadPtr((void *)(size_t)g_wp_addr, 4)) {
            LONG v = *(LONG *)(size_t)g_wp_addr;
            if (v == 118)
                logmsg("WATCH DEATH state=118 eip=%08x ret=%08x", (unsigned)c->Eip,
                       IsBadReadPtr((unsigned *)c->Esp, 4) ? 0 : ((unsigned *)c->Esp)[3]);
        }
        /* Hit-point writes: the instant-kill traps zero HP directly (measured
         * 2026-10-24), so logging only non-positive values finds the trap code
         * without drowning in healing/UI writes. */
        if (g_watch_ent_off == 0x474 && g_wp_addr &&
            !IsBadReadPtr((void *)(size_t)g_wp_addr, 4)) {
            float hv = *(float *)(size_t)g_wp_addr;
            if (!(hv > 0.0f))
                logmsg("WATCH KILLHP eip=%08x value=%.1f ret=%08x", (unsigned)c->Eip,
                       (double)hv,
                       IsBadReadPtr((unsigned *)c->Esp, 4) ? 0 : ((unsigned *)c->Esp)[3]);
        }
    }
    if (g_watch_ent_off == 0x520) g_last_x_eip = (LONG)c->Eip;
    c->Dr6 = 0;
    c->EFlags &= ~0x100u;
    return EXCEPTION_CONTINUE_EXECUTION;
}

static void set_dr_one(DWORD tid, DWORD dr0, DWORD dr7)
{
    HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                          THREAD_SET_CONTEXT, FALSE, tid);
    if (!t) return;
    SuspendThread(t);
    CONTEXT c;
    memset(&c, 0, sizeof c);
    c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (GetThreadContext(t, &c)) {
        c.Dr0 = dr0; c.Dr1 = c.Dr2 = c.Dr3 = 0;
        c.Dr6 = 0;   c.Dr7 = dr7;
        SetThreadContext(t, &c);
    }
    ResumeThread(t);
    CloseHandle(t);
}

static void set_dr_all(DWORD dr0, DWORD dr7)
{
    DWORD me = GetCurrentThreadId(), pid = GetCurrentProcessId();
    HANDLE snap;
    THREADENTRY32 te;
    if (g_wp_tid) { set_dr_one(g_wp_tid, dr0, dr7); return; }
    snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    te.dwSize = sizeof te;
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != pid || te.th32ThreadID == me) continue;
            set_dr_one(te.th32ThreadID, dr0, dr7);
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
}

static void wp_arm(unsigned addr)
{
    if (g_wp_addr == (LONG)addr) return;
    if (addr && !g_veh) {
        /* never arm a debug register without our handler: the #DB would reach
         * the game's own handlers and take the process down (crashed this way
         * on 2026-10-24 with watchnet=1 before the install condition was fixed) */
        logmsg("watchpoint: refusing to arm %08x, no exception handler installed", addr);
        return;
    }
    if (g_wp_addr) set_dr_all(0, 0);
    g_wp_addr = (LONG)addr;
    if (addr) {
        if (!g_wp_tid) {
            HWND gw = find_game_window();
            if (gw) g_wp_tid = GetWindowThreadProcessId(gw, NULL);
        }
        if (!g_wp_tid) {
            logmsg("watchpoint: no game window, not armed");
            g_wp_addr = 0;
            return;
        }
        /* L0=1, 4 bytes; condition 01 = writes, 11 = reads and writes */
        set_dr_all(addr, g_watch_rw ? 0x000F0001u : 0x000D0001u);
        logmsg("watchpoint armed on %08x (%s, tid %lu, %ld hits so far)",
               addr, g_watch_rw ? "read+write" : "write", g_wp_tid, g_wp_n);
    }
}

/* ---------------- config (unepic_mod.ini next to unepic.exe) --------------- */
static int g_panel_x = -1, g_panel_y = -1;

static void cfg_path(char *out, size_t n)
{
    char p[MAX_PATH];
    GetModuleFileNameA(NULL, p, MAX_PATH);
    char *s = strrchr(p, '\\');
    if (s) *s = 0;
    _snprintf(out, n, "%s\\unepic_mod.ini", p);
}

static void cfg_load_cheats(void)
{
    char p[MAX_PATH];
    cfg_path(p, sizeof p);
    g_god    = GetPrivateProfileIntA("cheats", "god", 0, p);
    g_maxxp  = GetPrivateProfileIntA("cheats", "maxxp", 0, p);
    g_nocons = GetPrivateProfileIntA("cheats", "nocons", 1, p);
    g_nogun  = GetPrivateProfileIntA("cheats", "nogun", 1, p);
    g_teleport = GetPrivateProfileIntA("cheats", "teleport", 0, p);
    {
        int s = GetPrivateProfileIntA("cheats", "tp_scale", 100, p);
        if (s < 10) s = 10;
        if (s > 400) s = 400;
        g_tp_scale = (float)s / 100.0f;
    }
    g_speed_pct = GetPrivateProfileIntA("cheats", "speed", 100, p);
    g_airjump   = GetPrivateProfileIntA("cheats", "airjump", 0, p);
    g_mp_tp = GetPrivateProfileIntA("cheats", "mp_tp", 1, p);
    {
        int jk = GetPrivateProfileIntA("cheats", "jump_key", 0, p);
        if (jk > 0 && jk < 256) {
            g_jump_vk = jk;
            g_jump_key_forced = 1;
            logmsg("cfg: jump key forced to vk=%d", jk);
        }
    }
    g_atkspd    = GetPrivateProfileIntA("cheats", "atkspd", 10, p);
    if (g_speed_pct < 10) g_speed_pct = 10;
    if (g_speed_pct > 1000) g_speed_pct = 1000;
    g_speed_mul = (float)g_speed_pct / 100.0f;
    if (g_atkspd < 10) g_atkspd = 10;
    if (g_atkspd > 100) g_atkspd = 100;
    g_atk_div = (float)g_atkspd / 10.0f;
    logmsg("cfg: speed=%ld%% atkspd=%ld%%", g_speed_pct, g_atkspd);
    g_panel_x = GetPrivateProfileIntA("panel", "x", -1, p);
    g_panel_y = GetPrivateProfileIntA("panel", "y", -1, p);
    logmsg("cfg: god=%ld maxxp=%ld nocons=%ld nogun=%ld teleport=%ld scale=%.2f panel=%d,%d",
           g_god, g_maxxp, g_nocons, g_nogun, g_teleport, (double)g_tp_scale,
           g_panel_x, g_panel_y);
}

static void cfg_save_cheats(void)
{
    char p2[MAX_PATH];
    cfg_path(p2, sizeof p2);
    WritePrivateProfileStringA("cheats", "airjump", g_airjump ? "1" : "0", p2);
    char p[MAX_PATH];
    cfg_path(p, sizeof p);
    WritePrivateProfileStringA("cheats", "god",    g_god    ? "1" : "0", p);
    WritePrivateProfileStringA("cheats", "maxxp",  g_maxxp  ? "1" : "0", p);
    WritePrivateProfileStringA("cheats", "nocons", g_nocons ? "1" : "0", p);
    WritePrivateProfileStringA("cheats", "teleport", g_teleport ? "1" : "0", p);
}

/* ---------------- helpers -------------------------------------------------- */
static int readable(void *p, unsigned n)
{
    return n && !IsBadReadPtr(p, n);
}

static unsigned stats_addr(void)
{
    unsigned p = *(unsigned *)ADDR_STATS_PTR;
    if (readable((void *)p, 0x180)) return p;
    if (readable((void *)STATS_ALT, 0x180)) return STATS_ALT;
    return 0;
}

static unsigned entity_addr(void)
{
    unsigned s = stats_addr();
    unsigned e;
    if (!s || !readable((void *)(s + OFF_ENTITY), 4)) return 0;
    e = *(unsigned *)(s + OFF_ENTITY);
    if (!readable((void *)(e + ENT_CURHP), 4)) return 0;
    return e;
}

/* write a protected int exactly the way the game's own setter does */
static void stats_set(unsigned s, unsigned off, int v)
{
    int m1 = *(int *)ADDR_MASK1, m2 = *(int *)ADDR_MASK2;
    *(int *)(s + off)         = v ^ m1;
    *(int *)(s + off + 0x1C)  = v ^ m2;
}

static int stats_get(unsigned s, unsigned off)
{
    return *(int *)(s + off) ^ *(int *)ADDR_MASK1;
}

/* The game's save path reads this flag at 0x0043C8DB: 0 -> save\game_N.sav,
 * non-zero -> save\game_mp_N.sav.  It is 1 or 2 inside an MP session, so test
 * for non-zero.  Read it live: returning to a single-player game resets the
 * flag without restarting the process. */
static int game_is_mp(void)
{
    return readable((void *)ADDR_MP_MODE, 4) && *(unsigned *)ADDR_MP_MODE != 0;
}

/* Money is item 9101 in both modes.  A fresh MP bag can have no 9101 record at
 * all (the HUD then shows 0), so money_set() may have to create one.  Item 9102
 * is the UN-points record: the game rebuilds it from stats+0x170 at 0x0043322E
 * and adds 9102 pickups to stats+0x170 at 0x004345F6, so it is not money. */

/* ---------------- thread freezing (optional) ------------------------------- */
#define MAXTH 192
static HANDLE g_th[MAXTH];
static int g_nth;

static void freeze_other_threads(void)
{
    DWORD me = GetCurrentThreadId(), pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te;
    te.dwSize = sizeof te;
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID == pid && te.th32ThreadID != me && g_nth < MAXTH) {
                HANDLE t = OpenThread(THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
                if (t) { SuspendThread(t); g_th[g_nth++] = t; }
            }
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
    Sleep(20);
}

static void thaw_threads(void)
{
    for (int i = 0; i < g_nth; i++) { ResumeThread(g_th[i]); CloseHandle(g_th[i]); }
    g_nth = 0;
}

/* find "mov DWORD PTR [ecx+0xd18],1" and store 0 instead */
static unsigned char *g_tamper[8];
static unsigned char g_tamper_orig[8][4];
static int g_ntamper;

static void restore_tamper(void)
{
    for (int i = 0; i < g_ntamper; i++) {
        DWORD old;
        if (VirtualProtect(g_tamper[i], 4, PAGE_EXECUTE_READWRITE, &old)) {
            memcpy(g_tamper[i], g_tamper_orig[i], 4);
            VirtualProtect(g_tamper[i], 4, old, &old);
            FlushInstructionCache(GetCurrentProcess(), g_tamper[i], 4);
        }
    }
    g_ntamper = 0;
}

static void neutralise_tamper_flag(void)
{
    if (g_ntamper) return;             /* already patched by an earlier core */
    unsigned char *base = (unsigned char *)GetModuleHandleA(NULL);
    IMAGE_DOS_HEADER *dh = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + dh->e_lfanew);
    IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(nt);
    static const unsigned char pat[] = { 0xC7,0x41,0x18,0x01,0x00,0x00,0x00 };
    int n = 0;
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, s++) {
        if (memcmp(s->Name, ".text", 5) != 0) continue;
        unsigned char *p = base + s->VirtualAddress;
        unsigned char *end = p + s->Misc.VirtualSize;
        for (unsigned char *q = p; q + sizeof(pat) <= end; q++) {
            if (memcmp(q, pat, sizeof(pat)) != 0) continue;
            DWORD old;
            if (VirtualProtect(q + 3, 4, PAGE_EXECUTE_READWRITE, &old)) {
                if (g_ntamper < 8) {
                    g_tamper[g_ntamper] = q + 3;
                    memcpy(g_tamper_orig[g_ntamper], q + 3, 4);
                    g_ntamper++;
                }
                q[3] = 0x00;
                VirtualProtect(q + 3, 4, old, &old);
                FlushInstructionCache(GetCurrentProcess(), q, 8);
                logmsg("tamper-flag write neutralised at 0x%08x", (unsigned)(size_t)q);
                n++;
            }
        }
    }
    if (!n) logmsg("tamper-flag write NOT found");
}

static unsigned char *alloc_exec(unsigned n)
{
    unsigned char *p = (unsigned char *)VirtualAlloc(NULL, n, MEM_COMMIT | MEM_RESERVE,
                                                     PAGE_EXECUTE_READWRITE);
    if (p) memset(p, 0x90, n);
    return p;
}

/* dump the generated bytes so the handler can be disassembled offline */
static void dump_hex(const char *what, unsigned char *p, int n)
{
    char line[512];
    int o = 0, i = 0, part = 0;
    line[0] = 0;
    if (n < 0) n = 0;
    if (n > 1024) n = 1024;
    /* logmsg caps a line at 512 bytes, so long handlers are dumped in chunks */
    for (i = 0; i < n; i++) {
        if (o > (int)sizeof line - 4) {
            logmsg("%s bytes[%d]: %s", what, part++, line);
            o = 0; line[0] = 0;
        }
        o += snprintf(line + o, sizeof line - o, "%02x ", p[i]);
    }
    if (o > 0) logmsg("%s bytes[%d]: %s", what, part, line);
    else logmsg("%s bytes: (empty)", what);
}

typedef struct { unsigned char *tgt; unsigned nused; DWORD prot; unsigned char orig[16]; } Patch;
static Patch g_patch[16];
static int g_npatch;

static void record_patch(unsigned char *tgt, unsigned nused, DWORD prot)
{
    if (g_npatch >= 16 || nused > 16) return;
    g_patch[g_npatch].tgt = tgt;
    g_patch[g_npatch].nused = nused;
    g_patch[g_npatch].prot = prot;
    memcpy(g_patch[g_npatch].orig, tgt, nused);
    g_npatch++;
}

static void unpatch_all(void)
{
    for (int i = g_npatch - 1; i >= 0; i--) {
        DWORD old;
        if (VirtualProtect(g_patch[i].tgt, g_patch[i].nused, PAGE_EXECUTE_READWRITE, &old)) {
            memcpy(g_patch[i].tgt, g_patch[i].orig, g_patch[i].nused);
            VirtualProtect(g_patch[i].tgt, g_patch[i].nused, g_patch[i].prot, &old);
            FlushInstructionCache(GetCurrentProcess(), g_patch[i].tgt, g_patch[i].nused);
        }
    }
    g_npatch = 0;
}

static int install_jmp(unsigned char *tgt, unsigned nused, unsigned char *handler)
{
    DWORD old;
    if (USE_FREEZE) freeze_other_threads();
    if (!VirtualProtect(tgt, nused, PAGE_EXECUTE_READWRITE, &old)) {
        logmsg("VirtualProtect(0x%08x) failed err=%lu", (unsigned)(size_t)tgt, GetLastError());
        if (USE_FREEZE) thaw_threads();
        return 0;
    }
    record_patch(tgt, nused, old);
    tgt[0] = 0xE9;
    *(DWORD *)(tgt + 1) = (DWORD)(handler - (tgt + 5));
    for (unsigned i = 5; i < nused; i++) tgt[i] = 0x90;
    VirtualProtect(tgt, nused, old, &old);
    FlushInstructionCache(GetCurrentProcess(), tgt, nused);
    if (USE_FREEZE) thaw_threads();
    return 1;
}

/* ---- detour 1: item count setter (0x00422520) -- OBSERVE ONLY --------------
 * This setter turned out to be the save/load writer (caller 0x004732e4 reads
 * values from the save stream and stores them).  Clamping it would corrupt
 * loaded stacks, so it only records now; real consumption is detour 5.
 *
 *  0x00 83 3D <&g_nocons> 00   cmp dword[flag],0
 *  0x07 74 rel8                je  PASS(0x42)
 *  0x09 8B 51 04               mov edx,[ecx+4]
 *  0x0C 33 15 <m1>             xor edx,[m1]          ; edx = old value (READ ONLY)
 *  0x12 3B 54 24 04            cmp edx,[esp+4]
 *  0x16 0F 8E rel32            jle PASS              ; increase or equal
 *  0x1C 8B 01 / 8B 00          eax = type id
 *  0x20 A3 <&g_dec_type>       mov [dec_type],eax
 *  0x25 8B 44 24 04 / A3      mov [dec_new],[esp+4]
 *  0x2E 89 15 <&g_dec_old>     mov [dec_old],edx
 *  0x34 8B 04 24 / A3 <ret>   mov [dec_ret],[esp]    ; caller return address
 *  0x3C FF 05 <&g_dec_total>   inc [dec_total]
 *  0x42 89 0D <&g_dec_slot>    mov [dec_slot],ecx     ; which record it was
 *  0x48 8B 15 <m1>   PASS      mov edx,[m1]          ; original instruction 1
 *  0x4E FF 25 <slot>           jmp [slot]            ; -> tgt+6
 *  0x56 <slot>
 */
static void patch_count_setter(void)
{
    static const unsigned char SIG[] = {
        0x8B,0x15,0x2C,0x23,0x67,0x00, 0x8B,0x44,0x24,0x04, 0x33,0xD0,
        0x89,0x51,0x04, 0x8B,0x15,0x30,0x23,0x67,0x00, 0x33,0xD0,
        0x89,0x51,0x30, 0xC2,0x04,0x00 };
    const int PASS = 0x48;
    unsigned char *base = (unsigned char *)GetModuleHandleA(NULL);
    IMAGE_DOS_HEADER *dh = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + dh->e_lfanew);
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    unsigned char *tgt = NULL;
    for (int i = 0; i < nt->FileHeader.NumberOfSections && !tgt; i++) {
        if (memcmp(sec[i].Name, ".text", 5) != 0) continue;
        unsigned char *p = base + sec[i].VirtualAddress;
        unsigned char *end = p + sec[i].Misc.VirtualSize;
        for (unsigned char *q = p; q + sizeof(SIG) <= end; q++)
            if (memcmp(q, SIG, sizeof(SIG)) == 0) { tgt = q; break; }
    }
    if (!tgt) { logmsg("count setter not found"); return; }
    DWORD mask1 = *(DWORD *)(tgt + 2);

    unsigned char *h = alloc_exec(160);
    if (!h) { logmsg("VirtualAlloc failed"); return; }
    memcpy(h + 0x00, "\x83\x3D", 2);  *(DWORD *)(h + 2) = (DWORD)&g_nocons; h[6] = 0x00;
    h[0x07] = 0x74; h[0x08] = (unsigned char)(PASS - 0x09);
    memcpy(h + 0x09, "\x8B\x51\x04", 3);
    memcpy(h + 0x0C, "\x33\x15", 2);  *(DWORD *)(h + 0x0E) = mask1;
    memcpy(h + 0x12, "\x3B\x54\x24\x04", 4);
    memcpy(h + 0x16, "\x0F\x8E", 2);  *(DWORD *)(h + 0x18) = PASS - 0x1C;
    memcpy(h + 0x1C, "\x8B\x01\x8B\x00", 4);
    h[0x20] = 0xA3;                   *(DWORD *)(h + 0x21) = (DWORD)&g_dec_type;
    memcpy(h + 0x25, "\x8B\x44\x24\x04", 4);
    h[0x29] = 0xA3;                   *(DWORD *)(h + 0x2A) = (DWORD)&g_dec_new;
    memcpy(h + 0x2E, "\x89\x15", 2);  *(DWORD *)(h + 0x30) = (DWORD)&g_dec_old;
    memcpy(h + 0x34, "\x8B\x04\x24", 3);
    h[0x37] = 0xA3;                   *(DWORD *)(h + 0x38) = (DWORD)&g_dec_ret;
    memcpy(h + 0x3C, "\xFF\x05", 2);  *(DWORD *)(h + 0x3E) = (DWORD)&g_dec_total;
    memcpy(h + 0x42, "\x89\x0D", 2);  *(DWORD *)(h + 0x44) = (DWORD)&g_dec_slot;
    memcpy(h + PASS, tgt, 6);
    memcpy(h + 0x4E, "\xFF\x25", 2);  *(DWORD *)(h + 0x50) = (DWORD)(h + 0x54);
    *(DWORD *)(h + 0x54) = (DWORD)(tgt + 6);
    dump_hex("count-setter handler", h, 0x58);

    if (install_jmp(tgt, 5, h))
        logmsg("count-setter detour ok (observe only): tgt=0x%08x handler=0x%08x",
               (unsigned)(size_t)tgt, (unsigned)(size_t)h);
    else
        logmsg("count-setter detour FAILED");
}

/* ---- emitter helpers for the hand-written handlers -------------------------
 * Each helper writes a fixed encoding and returns the next offset, so a layout
 * mistake shows up as the "p != RESUME" assert instead of as broken code. */
static int em(unsigned char *h, int p, const void *bytes, int n)
{
    memcpy(h + p, bytes, n);
    return p + n;
}
static int em_ff05(unsigned char *h, int p, unsigned addr)   /* inc [m32]     6 */
{
    memcpy(h + p, "\xFF\x05", 2); *(DWORD *)(h + p + 2) = addr; return p + 6;
}
static int em_89xx(unsigned char *h, int p, int r, unsigned addr)
{
    /* mov [m32],<r> : 89 /r with mod=00 rm=101 -> 89 05|r<<3, m32   (6 bytes) */
    h[p] = 0x89; h[p + 1] = (unsigned char)(5 | (r << 3));
    *(DWORD *)(h + p + 2) = addr;
    return p + 6;
}
static int em_3305(unsigned char *h, int p, unsigned addr)   /* xor eax,[m32] 6 */
{
    memcpy(h + p, "\x33\x05", 2); *(DWORD *)(h + p + 2) = addr; return p + 6;
}
static int em_c705(unsigned char *h, int p, unsigned addr, unsigned v)
{
    /* mov [m32],imm32 : C7 05 m32 imm32                             (10 bytes) */
    memcpy(h + p, "\xC7\x05", 2);
    *(DWORD *)(h + p + 2) = addr;
    *(DWORD *)(h + p + 6) = v;
    return p + 10;
}
static int em_cmp_a_imm(unsigned char *h, int p, unsigned v)  /* cmp eax,imm32 5 */
{
    h[p] = 0x3D; *(DWORD *)(h + p + 1) = v; return p + 5;
}
#define EMA(x) ((unsigned)(size_t)&(x))

/* ---- the masked counter adders, and the "Infinite items" block -------------
 * An item record carries FOUR masked numbers (each one a value + its mirror):
 *   +0x04 / +0x30  quantity shown on the icon   get 0x422510 set 0x422520 add 0x422540
 *   +0x0C / +0x34  shots / uses left            get 0x422630 set 0x422640 add 0x422660
 *   +0x14 / +0x38  third counter                get 0x4225D0 set 0x4225E0 add 0x422600
 *   +0x20 / +0x3C  fourth counter               get 0x422570 set 0x422580 add 0x4225A0
 * "Infinite items" used to detour only 0x423480 (consume), which takes one away
 * from +0x04 - that is why firing a bow still spent a shot: a shot is
 *
 *   0x4a043e  mov  ecx,[ebp+0x1240]   ; the EQUIPPED record
 *   0x4a0444  push 0xffffffff         ; delta = -1
 *   0x4a045a  call 0x422660           ; add -1 to +0x0C / +0x34
 *
 * and 0x4a2776 does the same for use-type 0xb (ranged) weapons, both bypassing
 * 0x423480 entirely.  Captured live: while shooting, the equipped record's +0x0C
 * walked 48->42 and 12->7 and none of 0x422520/0x422540/0x4225a0/0x423480 saw a
 * bag write.
 *
 * So: hook the adders.  All four share this 9-byte prologue and calling shape
 * (ecx = record, [esp] = caller, [esp+4] = delta):
 *
 *   A1 2C 23 67 00     mov  eax,ds:0x67232c
 *   8B 54 24 04        mov  edx,[esp+4]
 *
 * With block=1 the handler also refuses the decrease when the record belongs to
 * the player's own bag and [cheats] nocons is on - it writes 0 over the delta,
 * so the record, the mirror and every other side effect (sound, animation, the
 * shot counter at ent+0x704) stay exactly as the game makes them.  Ground and
 * chest records are never touched, so picking things up still works normally.
 * eax/edx are the only scratch registers; the re-executed prologue reloads both,
 * ecx/esi/edi and the stack frame are left as found.
 */
static void patch_count_adder(unsigned tgt, int field_off, volatile LONG *which,
                              const char *tag, int block)
{
    static const unsigned char PRO[] = {
        0xA1,0x2C,0x23,0x67,0x00, 0x8B,0x54,0x24,0x04 };
    unsigned char *t = (unsigned char *)(size_t)tgt;
    unsigned char *h;
    DWORD mask1;
    int p = 0, jge_at, je_at, k;
    int jr[16], njr = 0;
    int RESUME;

    if (memcmp(t, PRO, sizeof PRO) != 0) {
        logmsg("%s prologue mismatch at 0x%08x: %02x %02x %02x %02x %02x %02x %02x %02x %02x",
               tag, tgt, t[0],t[1],t[2],t[3],t[4],t[5],t[6],t[7],t[8]);
        return;
    }
    mask1 = *(DWORD *)(t + 1);            /* the mask global it reads */
    h = alloc_exec(256);
    if (!h) { logmsg("VirtualAlloc failed"); return; }

#define JGE_RESUME32() do { h[p] = 0x0F; h[p + 1] = 0x8D; p += 6; } while (0)
#define JR(op)         do { h[p] = (op); if (njr >= 16) { logmsg("%s: too many branches", tag); return; } jr[njr++] = p; p += 2; } while (0)

    p = em_ff05(h, p, EMA(g_mu_calls));
    p = em_ff05(h, p, (unsigned)(size_t)which);
    p = em_89xx(h, p, 1, EMA(g_mu_slot));                 /* mov [slot],ecx  */
    p = em(h, p, "\x8B\x04\x24", 3);                      /* mov eax,[esp]   */
    p = em_89xx(h, p, 0, EMA(g_mu_ret));                  /* mov [ret],eax   */
    p = em(h, p, "\x8B\x44\x24\x04", 4);                  /* mov eax,[esp+4] */
    p = em_89xx(h, p, 0, EMA(g_mu_delta));
    p = em(h, p, "\x85\xC0", 2);                          /* test eax,eax    */
    jge_at = p; JGE_RESUME32();                            /* jge RESUME      */
    p = em_ff05(h, p, EMA(g_mu_dec));
    h[p] = 0x8B; h[p + 1] = 0x41; h[p + 2] = (unsigned char)field_off; p += 3;
    p = em_3305(h, p, mask1);                              /* xor eax,[m1]    */
    p = em_89xx(h, p, 0, EMA(g_mu_old));
    p = em(h, p, "\x03\x44\x24\x04", 4);                  /* add eax,[esp+4] */
    p = em_89xx(h, p, 0, EMA(g_mu_new));
    p = em(h, p, "\x8B\x01", 2);                          /* mov eax,[ecx]   */
    p = em(h, p, "\x85\xC0", 2);
    je_at = p; p = em(h, p, "\x74\x00", 2);               /* je  skip type   */
    p = em(h, p, "\x8B\x00", 2);                          /* mov eax,[eax]   */
    p = em_89xx(h, p, 0, EMA(g_mu_type));
    h[je_at + 1] = (unsigned char)(p - (je_at + 2));      /* rel8, forward   */
    p = em_c705(h, p, EMA(g_mu_src), tgt);                /* mov [src],id    */

    if (block) {
        /* Only the two known shot sites.  A blanket "refuse every negative delta
         * on a bag record" is what makes this dangerous: 0x4229A0's own merge and
         * the transfer path use the same adder, and refusing the source half of a
         * move leaves the game's bookkeeping inconsistent.  So compare the return
         * address instead:
         *   0x4a0444 push 0xffffffff / 0x4a045a call 0x422660  -> returns 0x4a045f
         *   0x4a2775 push ebx       / 0x4a2776 call 0x422660  -> returns 0x4a277b
         */
        int je_do_at, do_at;
        p = em(h, p, "\x8B\x04\x24", 3);                  /* mov eax,[esp]   */
        p = em_cmp_a_imm(h, p, 0x004A045Fu);
        je_do_at = p; h[p] = 0x74; h[p + 1] = 0; p += 2;  /* je  DO_BLOCK    */
        p = em_cmp_a_imm(h, p, 0x004A277Bu);
        JR(0x75);                                         /* jne RESUME      */
        do_at = p;
        h[je_do_at + 1] = (unsigned char)(do_at - (je_do_at + 2));
        memcpy(h + p, "\x83\x3D", 2); *(DWORD *)(h + p + 2) = EMA(g_nocons); h[p + 6] = 0;
        p += 7;                                           /* cmp [nocons],0  */
        JR(0x74);
        memcpy(h + p, "\x83\x3D", 2); *(DWORD *)(h + p + 2) = EMA(g_nogun); h[p + 6] = 0;
        p += 7;                                           /* cmp [nogun],0   */
        JR(0x74);
        p = em_ff05(h, p, EMA(g_mu_blocked));
        p = em(h, p, "\xC7\x44\x24\x04\x00\x00\x00\x00", 8); /* [esp+4] = 0   */
    }
    RESUME = p;
    *(DWORD *)(h + jge_at + 2) = (DWORD)(RESUME - (jge_at + 6));
    for (k = 0; k < njr; k++) {
        int rel = RESUME - (jr[k] + 2);
        if (rel < -128 || rel > 127) {
            logmsg("%s: rel8 branch does not fit (%d) - handler not installed", tag, rel);
            return;
        }
        h[jr[k] + 1] = (unsigned char)rel;
    }
    for (k = 0; k < njr; k++) {
        int rel = RESUME - (jr[k] + 2);
        if (rel < -128 || rel > 127) {
            logmsg("%s: rel8 branch does not fit (%d) - handler not installed", tag, rel);
            return;
        }
        h[jr[k] + 1] = (unsigned char)rel;
    }
    p = em(h, p, t, 9);                                   /* original bytes  */
    memcpy(h + p, "\xFF\x25", 2);  *(DWORD *)(h + p + 2) = (DWORD)(h + p + 6);
    *(DWORD *)(h + p + 6) = (DWORD)(size_t)(t + 9);
    p += 10;
#undef JGE_RESUME32
#undef JR
    dump_hex(tag, h, p);
    /* Overwrite only the first instruction (5 bytes: A1 disp32).  These adders run
     * hundreds of times per second (0x422660 is called by the ammo-refill loop at
     * 0x42f7f9 every frame), and install_jmp nops everything past the jmp - a
     * thread caught mid-instruction executes garbage.  That is what crashed the
     * game on 2026-10-22 when this was install_jmp(t, 9, h): bytes 5..8 belong to
     * "mov edx,[esp+4]" and must stay untouched.  The handler replays all 9 bytes
     * and resumes at t+9, so the shorter overwrite is enough. */
    if (install_jmp(t, 5, h))
        logmsg("%s detour ok (%s): tgt=0x%08x handler=0x%08x size=%d",
               tag, block ? "blocks the 2 shot sites" : "capture only",
               tgt, (unsigned)(size_t)h, p);
    else
        logmsg("%s detour FAILED", tag);
}

/* ---- detour 2: entity::add_hp (0x00445A40), instrumented -------------------
 * entry: ecx = entity, [esp+4] = float delta, [esp] = return address.
 * Original first 12 bytes:
 *   83 EC 08 | 57 | 8B F9 | D9 87 74 04 00 00
 *   sub esp,8 / push edi / mov edi,ecx / fld dword[edi+0x474]
 *
 *  0x00 FF 05 <&g_hp_calls>    inc dword[calls]
 *  0x06 8B C1 / A3 <lastthis>  save this
 *  0x0D 8B 14 24 / 89 15 <..>  save return address
 *  0x16 83 3D <&g_god> 00      cmp dword[flag],0
 *  0x1D 74 rel                 je  KEEP(0x54)
 *  0x1F 8B 05 <0x71C5C0>       mov eax,[stats ptr global]
 *  0x25 85 C0                  test eax,eax
 *  0x27 74 rel                 je  KEEP
 *  0x29 8B 80 78 F7 17 00      mov eax,[eax+0x17F78]      ; player entity
 *  0x2F 3B C1                  cmp eax,ecx
 *  0x31 75 rel                 jne KEEP                   ; not the player
 *  0x33 FF 05 <&g_hp_player>   inc dword[player]
 *  0x39 8B 44 24 04            mov eax,[esp+4]            ; delta (float bits)
 *  0x3D A3  <&g_hp_lastdelta>  mov [lastdelta],eax
 *  0x42 85 C0                  test eax,eax
 *  0x44 79 rel                 jns KEEP                   ; delta >= 0 (heal) -> allow
 *  0x46 FF 05 <&g_hp_neg>      inc dword[neg]
 *  0x4C C7 44 24 04 00 00 00 00  mov dword[esp+4],0       ; refuse the damage
 *  0x54 <original 12 bytes>                KEEP
 *  0x60 FF 25 <slot>           jmp [slot]                 ; -> tgt+12
 *  0x70 <slot>
 */
static void patch_add_hp(void)
{
    static const unsigned char PRO[] = {
        0x83,0xEC,0x08, 0x57, 0x8B,0xF9, 0xD9,0x87,0x74,0x04,0x00,0x00 };
    const int KEEP = 0x54;
    unsigned char *tgt = (unsigned char *)FN_ADDHP;
    if (memcmp(tgt, PRO, sizeof PRO) != 0) {
        logmsg("add_hp prologue mismatch at 0x%08x: %02x %02x %02x %02x %02x %02x %02x %02x"
               " %02x %02x %02x %02x", FN_ADDHP,
               tgt[0],tgt[1],tgt[2],tgt[3],tgt[4],tgt[5],tgt[6],tgt[7],
               tgt[8],tgt[9],tgt[10],tgt[11]);
        return;
    }
    unsigned char *h = alloc_exec(160);
    if (!h) { logmsg("VirtualAlloc failed"); return; }
    memcpy(h + 0x00, "\xFF\x05", 2);  *(DWORD *)(h + 0x02) = (DWORD)&g_hp_calls;
    memcpy(h + 0x06, "\x8B\xC1", 2);
    h[0x08] = 0xA3;                  *(DWORD *)(h + 0x09) = (DWORD)&g_hp_lastthis;
    memcpy(h + 0x0D, "\x8B\x14\x24", 3);
    memcpy(h + 0x10, "\x89\x15", 2); *(DWORD *)(h + 0x12) = (DWORD)&g_hp_lastret;
    memcpy(h + 0x16, "\x83\x3D", 2); *(DWORD *)(h + 0x18) = (DWORD)&g_god; h[0x1C] = 0x00;
    h[0x1D] = 0x74; h[0x1E] = (unsigned char)(KEEP - 0x1F);
    memcpy(h + 0x1F, "\x8B\x05", 2); *(DWORD *)(h + 0x21) = ADDR_STATS_PTR;
    memcpy(h + 0x25, "\x85\xC0", 2);
    h[0x27] = 0x74; h[0x28] = (unsigned char)(KEEP - 0x29);
    memcpy(h + 0x29, "\x8B\x80", 2); *(DWORD *)(h + 0x2B) = OFF_ENTITY;
    memcpy(h + 0x2F, "\x3B\xC1", 2);
    h[0x31] = 0x75; h[0x32] = (unsigned char)(KEEP - 0x33);
    memcpy(h + 0x33, "\xFF\x05", 2); *(DWORD *)(h + 0x35) = (DWORD)&g_hp_player;
    memcpy(h + 0x39, "\x8B\x44\x24\x04", 4);
    h[0x3D] = 0xA3;                  *(DWORD *)(h + 0x3E) = (DWORD)&g_hp_lastdelta;
    memcpy(h + 0x42, "\x85\xC0", 2);
    h[0x44] = 0x79; h[0x45] = (unsigned char)(KEEP - 0x46);
    memcpy(h + 0x46, "\xFF\x05", 2); *(DWORD *)(h + 0x48) = (DWORD)&g_hp_neg;
    memcpy(h + 0x4C, "\xC7\x44\x24\x04\x00\x00\x00\x00", 8);
    memcpy(h + KEEP, tgt, 12);
    memcpy(h + 0x60, "\xFF\x25", 2); *(DWORD *)(h + 0x62) = (DWORD)(h + 0x70);
    *(DWORD *)(h + 0x70) = (DWORD)(tgt + 12);
    dump_hex("add_hp handler", h, 0x74);

    if (install_jmp(tgt, 12, h))
        logmsg("add_hp detour ok: tgt=0x%08x handler=0x%08x", (unsigned)(size_t)tgt,
               (unsigned)(size_t)h);
    else
        logmsg("add_hp detour FAILED");
}

/* ---- detour 3: entity::hurt (0x0049F730) -- THE damage path ----------------
 * 0x0049f730  sub esp,8 / push esi / mov esi,ecx      (83 EC 08 56 8B F1)
 * 0x0049f736  cmp dword[esi+0x540],0x76 ; dead/immune state -> skip
 * 0x0049f765  fld [esi+0x474] ... 0x0049f776 fsub [esp+0x14]  ; HP -= amount
 * entry: ecx = victim, [esp+4] = float damage amount (positive).
 * Zeroing that amount keeps every other effect (knockback, sound, i-frame
 * start) intact and simply makes the damage nil.
 *
 *  0x00 FF 05 <&g_dm_calls>    inc [calls]
 *  0x06 8B C1 / A3 <lastthis>  save this
 *  0x0D 8B 14 24 / 89 15 <..>  save return address
 *  0x16 83 3D <&g_god> 00      cmp [god],0
 *  0x1D 74 rel                 je  KEEP(0x50)
 *  0x1F 8B 05 <0x71C5C0>       mov eax,[stats ptr]
 *  0x25 85 C0 / 74 rel         test eax,eax / je KEEP
 *  0x29 8B 80 <0x17F78>        mov eax,[eax+0x17F78]  ; player entity
 *  0x2F 3B C1 / 75 rel         cmp eax,ecx / jne KEEP
 *  0x33 FF 05 <&g_dm_player>   inc [player]
 *  0x39 8B 44 24 04            mov eax,[esp+4]        ; damage float bits
 *  0x3D A3 <&g_dm_lastamt>     mov [lastamt],eax
 *  0x42 C7 44 24 04 00 00 00 00  mov dword[esp+4],0   ; damage = 0
 *  0x4A FF 05 <&g_dm_blocked>  inc [blocked]
 *  0x50 <original 6 bytes>              KEEP
 *  0x56 FF 25 <slot>           jmp [slot]             ; -> tgt+6
 *  0x60 <slot>
 */
static void patch_hurt(void)
{
    static const unsigned char PRO[] = { 0x83,0xEC,0x08, 0x56, 0x8B,0xF1 };
    const int KEEP = 0x50;
    unsigned char *tgt = (unsigned char *)FN_HURT;
    if (memcmp(tgt, PRO, sizeof PRO) != 0) {
        logmsg("hurt prologue mismatch at 0x%08x: %02x %02x %02x %02x %02x %02x",
               FN_HURT, tgt[0],tgt[1],tgt[2],tgt[3],tgt[4],tgt[5]);
        return;
    }
    unsigned char *h = alloc_exec(160);
    if (!h) { logmsg("VirtualAlloc failed"); return; }
    memcpy(h + 0x00, "\xFF\x05", 2);  *(DWORD *)(h + 0x02) = (DWORD)&g_dm_calls;
    memcpy(h + 0x06, "\x8B\xC1", 2);
    h[0x08] = 0xA3;                   *(DWORD *)(h + 0x09) = (DWORD)&g_dm_lastthis;
    memcpy(h + 0x0D, "\x8B\x14\x24", 3);
    memcpy(h + 0x10, "\x89\x15", 2);  *(DWORD *)(h + 0x12) = (DWORD)&g_dm_lastret;
    memcpy(h + 0x16, "\x83\x3D", 2);  *(DWORD *)(h + 0x18) = (DWORD)&g_god; h[0x1C] = 0x00;
    h[0x1D] = 0x74; h[0x1E] = (unsigned char)(KEEP - 0x1F);
    memcpy(h + 0x1F, "\x8B\x05", 2);  *(DWORD *)(h + 0x21) = ADDR_STATS_PTR;
    memcpy(h + 0x25, "\x85\xC0", 2);
    h[0x27] = 0x74; h[0x28] = (unsigned char)(KEEP - 0x29);
    memcpy(h + 0x29, "\x8B\x80", 2);  *(DWORD *)(h + 0x2B) = OFF_ENTITY;
    memcpy(h + 0x2F, "\x3B\xC1", 2);
    h[0x31] = 0x75; h[0x32] = (unsigned char)(KEEP - 0x33);
    memcpy(h + 0x33, "\xFF\x05", 2);  *(DWORD *)(h + 0x35) = (DWORD)&g_dm_player;
    memcpy(h + 0x39, "\x8B\x44\x24\x04", 4);
    h[0x3D] = 0xA3;                   *(DWORD *)(h + 0x3E) = (DWORD)&g_dm_lastamt;
    memcpy(h + 0x42, "\xC7\x44\x24\x04\x00\x00\x00\x00", 8);
    memcpy(h + 0x4A, "\xFF\x05", 2);  *(DWORD *)(h + 0x4C) = (DWORD)&g_dm_blocked;
    memcpy(h + KEEP, tgt, 6);
    memcpy(h + 0x56, "\xFF\x25", 2);  *(DWORD *)(h + 0x58) = (DWORD)(h + 0x60);
    *(DWORD *)(h + 0x60) = (DWORD)(tgt + 6);
    dump_hex("hurt handler", h, 0x64);

    if (install_jmp(tgt, 6, h))
        logmsg("hurt detour ok: tgt=0x%08x handler=0x%08x", (unsigned)(size_t)tgt,
               (unsigned)(size_t)h);
    else
        logmsg("hurt detour FAILED");
}

/* ---- detour 4: entity::damage_int (0x0049F490) -- the path that really hits
 * 0x0049f490  push ecx / mov eax,[esp+0x8] / push esi / mov esi,ecx   (8 bytes)
 * 0x0049f498  test eax,eax / jle return      ; amount <= 0 -> nothing happens
 * 0x0049f4d0  fld [esi+0x474] / fisub [esp+0x14] / fst [esi+0x474]
 * entry: ecx = victim, [esp+4] = int damage amount.
 *
 *  0x00 FF 05 <&g_d2_calls>    inc [calls]
 *  0x06 8B C1 / A3 <lastthis>
 *  0x0D 8B 14 24 / 89 15 <..>  save return address
 *  0x16 83 3D <&g_god> 00      cmp [god],0
 *  0x1D 74 rel                 je  KEEP(0x50)
 *  0x1F 8B 05 <0x71C5C0>
 *  0x25 85 C0 / 74 rel         test / je KEEP
 *  0x29 8B 80 <0x17F78>        ; player entity
 *  0x2F 3B C1 / 75 rel         cmp / jne KEEP
 *  0x33 FF 05 <&g_d2_player>   inc [player]
 *  0x39 8B 44 24 04            mov eax,[esp+4]        ; int amount
 *  0x3D A3 <&g_d2_lastamt>     mov [lastamt],eax
 *  0x42 C7 44 24 04 00 00 00 00  mov dword[esp+4],0   ; amount = 0
 *  0x4A FF 05 <&g_d2_blocked>  inc [blocked]
 *  0x50 <original 8 bytes>              KEEP
 *  0x58 FF 25 <slot>           jmp [slot]             ; -> tgt+8
 *  0x60 <slot>
 */
static void patch_hurt_int(void)
{
    static const unsigned char PRO[] = { 0x51, 0x8B,0x44,0x24,0x08, 0x56, 0x8B,0xF1 };
    const int KEEP = 0x50;
    unsigned char *tgt = (unsigned char *)FN_HURT2;
    if (memcmp(tgt, PRO, sizeof PRO) != 0) {
        logmsg("hurt_int prologue mismatch at 0x%08x: %02x %02x %02x %02x %02x %02x %02x %02x",
               FN_HURT2, tgt[0],tgt[1],tgt[2],tgt[3],tgt[4],tgt[5],tgt[6],tgt[7]);
        return;
    }
    unsigned char *h = alloc_exec(160);
    if (!h) { logmsg("VirtualAlloc failed"); return; }
    memcpy(h + 0x00, "\xFF\x05", 2);  *(DWORD *)(h + 0x02) = (DWORD)&g_d2_calls;
    memcpy(h + 0x06, "\x8B\xC1", 2);
    h[0x08] = 0xA3;                   *(DWORD *)(h + 0x09) = (DWORD)&g_d2_lastthis;
    memcpy(h + 0x0D, "\x8B\x14\x24", 3);
    memcpy(h + 0x10, "\x89\x15", 2);  *(DWORD *)(h + 0x12) = (DWORD)&g_d2_lastret;
    memcpy(h + 0x16, "\x83\x3D", 2);  *(DWORD *)(h + 0x18) = (DWORD)&g_god; h[0x1C] = 0x00;
    h[0x1D] = 0x74; h[0x1E] = (unsigned char)(KEEP - 0x1F);
    memcpy(h + 0x1F, "\x8B\x05", 2);  *(DWORD *)(h + 0x21) = ADDR_STATS_PTR;
    memcpy(h + 0x25, "\x85\xC0", 2);
    h[0x27] = 0x74; h[0x28] = (unsigned char)(KEEP - 0x29);
    memcpy(h + 0x29, "\x8B\x80", 2);  *(DWORD *)(h + 0x2B) = OFF_ENTITY;
    memcpy(h + 0x2F, "\x3B\xC1", 2);
    h[0x31] = 0x75; h[0x32] = (unsigned char)(KEEP - 0x33);
    memcpy(h + 0x33, "\xFF\x05", 2);  *(DWORD *)(h + 0x35) = (DWORD)&g_d2_player;
    memcpy(h + 0x39, "\x8B\x44\x24\x04", 4);
    h[0x3D] = 0xA3;                   *(DWORD *)(h + 0x3E) = (DWORD)&g_d2_lastamt;
    memcpy(h + 0x42, "\xC7\x44\x24\x04\x00\x00\x00\x00", 8);
    memcpy(h + 0x4A, "\xFF\x05", 2);  *(DWORD *)(h + 0x4C) = (DWORD)&g_d2_blocked;
    memcpy(h + KEEP, tgt, 8);
    memcpy(h + 0x58, "\xFF\x25", 2);  *(DWORD *)(h + 0x5A) = (DWORD)(h + 0x60);
    *(DWORD *)(h + 0x60) = (DWORD)(tgt + 8);
    dump_hex("hurt_int handler", h, 0x64);

    if (install_jmp(tgt, 8, h))
        logmsg("hurt_int detour ok: tgt=0x%08x handler=0x%08x", (unsigned)(size_t)tgt,
               (unsigned)(size_t)h);
    else
        logmsg("hurt_int detour FAILED");
}

/* ---- detour 5: stack consume (0x00423480) -- the real item-use path -------
 * 0x00423480  mov eax,[esp+8] / push esi / neg eax / mov esi,ecx /
 *             mov ecx,[esp+8] / push eax / call count_add(-n)
 *             [item+0x74] = -1 ; if (count < 1) remove item from the container
 *             returns 1 if items remain, 0 if the stack was removed
 * entry: ecx = container, [esp+4] = item object, [esp+8] = n, [esp+0xc] = ?
 * -> ret 0xc (caller cleans 12 bytes)
 *
 * With "infinite items" (g_nocons) on, any item except the currency types (9101
 * coin, 9102 UN points) returns "1 item left" without touching the count, so the
 * stack is never consumed or removed.
 *
 *  0x00 83 3D <&g_nocons> 00   cmp [nocons],0
 *  0x07 74 rel                 je  KEEP(0x4D)
 *  0x09 FF 05 <&g_it_calls>    inc [calls]
 *  0x0F 8B 44 24 04            mov eax,[esp+4]        ; item object
 *  0x13 3D 00010000 / 72 rel   cmp 0x10000 / jb KEEP
 *  0x1A 3D 08000000 / 73 rel   cmp 0x08000000 / jae KEEP
 *  0x21 8B 00                  mov eax,[eax]          ; type descriptor
 *  0x23 3D 00010000 / 72 rel   cmp 0x10000 / jb KEEP
 *  0x2A 8B 00                  mov eax,[eax]          ; type id
 *  0x2C 3D <9101> / 74 rel     cmp 9101 / je KEEP
 *  0x33 3D <9102> / 74 rel     cmp 9102 / je KEEP
 *  0x3A A3 <&g_it_lasttype>    mov [lasttype],eax
 *  0x3F FF 05 <&g_it_blocked>  inc [blocked]
 *  0x45 B8 01000000            mov eax,1
 *  0x4A C2 0C 00               ret 0x0c
 *  0x4D <original 6 bytes>              KEEP
 *  0x53 FF 25 <slot>           jmp [slot]             ; -> tgt+6
 *  0x60 <slot>
 */
static void patch_consume(void)
{
    static const unsigned char PRO[] = { 0x8B,0x44,0x24,0x08, 0x56, 0xF7,0xD8 };
    const int KEEP = 0x7E;
    unsigned char *tgt = (unsigned char *)FN_CONSUME;
    if (memcmp(tgt, PRO, sizeof PRO) != 0) {
        logmsg("consume prologue mismatch at 0x%08x: %02x %02x %02x %02x %02x %02x %02x",
               FN_CONSUME, tgt[0],tgt[1],tgt[2],tgt[3],tgt[4],tgt[5],tgt[6]);
        return;
    }
    unsigned char *h = alloc_exec(256);
    if (!h) { logmsg("VirtualAlloc failed"); return; }
    memcpy(h + 0x00, "\x83\x3D", 2);  *(DWORD *)(h + 0x02) = (DWORD)&g_nocons; h[6] = 0x00;
    h[0x07] = 0x74; h[0x08] = (unsigned char)(KEEP - 0x09);
    memcpy(h + 0x09, "\xFF\x05", 2);  *(DWORD *)(h + 0x0B) = (DWORD)&g_it_calls;
    memcpy(h + 0x0F, "\x8B\x44\x24\x04", 4);
    memcpy(h + 0x13, "\x3D\x00\x00\x01\x00", 5); h[0x18] = 0x72;
    h[0x19] = (unsigned char)(KEEP - 0x1A);
    memcpy(h + 0x1A, "\x3D\x00\x00\x00\x08", 5); h[0x1F] = 0x73;
    h[0x20] = (unsigned char)(KEEP - 0x21);
    memcpy(h + 0x21, "\x8B\x00", 2);
    memcpy(h + 0x23, "\x3D\x00\x00\x01\x00", 5); h[0x28] = 0x72;
    h[0x29] = (unsigned char)(KEEP - 0x2A);
    memcpy(h + 0x2A, "\x8B\x00", 2);
    h[0x2C] = 0xA3; *(DWORD *)(h + 0x2D) = (DWORD)&g_it_lasttype;
    memcpy(h + 0x31, "\x8B\x14\x24", 3);
    memcpy(h + 0x34, "\x89\x15", 2);  *(DWORD *)(h + 0x36) = (DWORD)&g_it_lastret;
    memcpy(h + 0x3A, "\x8B\x54\x24\x08", 4);
    memcpy(h + 0x3E, "\x89\x15", 2);  *(DWORD *)(h + 0x40) = (DWORD)&g_it_lastn;
    /* only the player's inventory container gets protected; shop stocks and
       chests must be allowed to change or the trade logic breaks */
    memcpy(h + 0x44, "\x8B\x15", 2);  *(DWORD *)(h + 0x46) = ADDR_STATS_PTR;
    memcpy(h + 0x4A, "\x85\xD2", 2);
    h[0x4C] = 0x74; h[0x4D] = (unsigned char)(KEEP - 0x4E);
    memcpy(h + 0x4E, "\x8B\x92", 2);  *(DWORD *)(h + 0x50) = OFF_ENTITY;
    memcpy(h + 0x54, "\x85\xD2", 2);
    h[0x56] = 0x74; h[0x57] = (unsigned char)(KEEP - 0x58);
    memcpy(h + 0x58, "\x8B\x92", 2);  *(DWORD *)(h + 0x5A) = 0x1038;
    memcpy(h + 0x5E, "\x3B\xD1", 2);          /* cmp edx,ecx */
    h[0x60] = 0x75; h[0x61] = (unsigned char)(KEEP - 0x62);
    h[0x62] = 0x3D; *(DWORD *)(h + 0x63) = 9101;  h[0x67] = 0x74;
    h[0x68] = (unsigned char)(KEEP - 0x69);
    h[0x69] = 0x3D; *(DWORD *)(h + 0x6A) = 9102;  h[0x6E] = 0x74;
    h[0x6F] = (unsigned char)(KEEP - 0x70);
    memcpy(h + 0x70, "\xFF\x05", 2);  *(DWORD *)(h + 0x72) = (DWORD)&g_it_blocked;
    memcpy(h + 0x76, "\xB8\x01\x00\x00\x00", 5);
    memcpy(h + 0x7B, "\xC2\x0C\x00", 3);
    memcpy(h + KEEP, tgt, 7);
    memcpy(h + 0x85, "\xFF\x25", 2);  *(DWORD *)(h + 0x87) = (DWORD)(h + 0x90);
    *(DWORD *)(h + 0x90) = (DWORD)(tgt + 7);
    dump_hex("consume handler", h, 0x94);

    if (install_jmp(tgt, 7, h))
        logmsg("consume detour ok (player inventory only): tgt=0x%08x handler=0x%08x",
               (unsigned)(size_t)tgt, (unsigned)(size_t)h);
    else
        logmsg("consume detour FAILED");
}

/* ---- instant-death traps (spikes): skip the HP=0 store while god is on -----
 * 0x42B335 is the tail of the rectangle trap check that walks the room's traps:
 *
 *   42b323  cmp [ecx+0x540],0x76 / je ...   already dead -> skip
 *   42b32c  cmp [ecx+0xcc],0x1f  / je ...   death pose  -> skip
 *   42b335  fldz
 *   42b337  fstps [ecx+0x474]              HP = 0      <- the kill
 *   42b33d  (loop continues)
 *
 * The eight bytes of fldz+fstps are replaced by a jump to a stub that redoes
 * them only while [g_god] is 0 and otherwise falls through to 0x42B33D.  God
 * mode therefore skips the kill completely - no HP loss, no death state, no
 * knock-back - while the trap behaves exactly as before when god is off.
 * Found with the HP write watchpoint on 2026-10-24 (EIP 0x42B33D, value 0.0). */
static int patch_trap_site(unsigned tgt_addr, unsigned char modrm, unsigned resume,
                           const char *tag)
{
    unsigned char *tgt = (unsigned char *)(size_t)tgt_addr;
    unsigned char *stub, *p, *jmp_at;
    unsigned *rel;
    /* fldz; fstps [reg+0x474] - the register differs per site (ecx single
     * player, edx multiplayer), the rest of the loop is identical. */
    if (tgt[0] != 0xD9 || tgt[1] != 0xEE || tgt[2] != 0xD9 || tgt[3] != modrm ||
        *(unsigned *)(tgt + 4) != 0x474u) {
        logmsg("trap patch %s: prologue mismatch %02x %02x %02x %02x %08x",
               tag, tgt[0], tgt[1], tgt[2], tgt[3], *(unsigned *)(tgt + 4));
        return 0;
    }
    stub = alloc_exec(48);
    if (!stub) return 0;
    p = stub;
    *p++ = 0x83; *p++ = 0x3D;                /* cmp dword [g_god], 0 */
    *(DWORD *)p = (DWORD)&g_god; p += 4;
    *p++ = 0x00;
    *p++ = 0x75; *p++ = 0x08;                /* jne +8: skip the two FP ops */
    *p++ = 0xD9; *p++ = 0xEE;                /* fldz */
    *p++ = 0xD9; *p++ = modrm;               /* fstps [reg+0x474] */
    *(DWORD *)p = 0x474u; p += 4;
    jmp_at = p;
    *p++ = 0xE9;                             /* jmp back after the two ops */
    rel = (unsigned *)p;
    *rel = resume - (DWORD)(size_t)(jmp_at + 5);
    p += 4;
    FlushInstructionCache(GetCurrentProcess(), stub, (SIZE_T)(p - stub));
    if (install_jmp(tgt, 8, stub)) {
        logmsg("trap patch ok (%s): HP kill skipped while god is on", tag);
        return 1;
    }
    logmsg("trap patch FAILED (%s)", tag);
    return 0;
}

static int patch_trap_spike(void)
{
    int n = 0;
    /* 0x42B335 is the single-player trap loop.  0x48F039 is the one that
     * actually runs on spikes in multiplayer - caught with the HP write
     * watchpoint on 2026-10-24 (EIP 0x48F041, value 0.0); the old single-site
     * patch left the multiplayer game dying there. */
    n += patch_trap_site(0x0042B335u, 0x99, 0x0042B33Du, "single-player");
    n += patch_trap_site(0x0048F039u, 0x9A, 0x0048F041u, "multiplayer");
    return n;
}

static void apply_patch(void)
{
    if (NEUTRALISE_FLAG) neutralise_tamper_flag();
    patch_hurt_int();
    patch_hurt();
    patch_add_hp();
    patch_consume();
    patch_count_setter();
    patch_count_adder(0x00422540u, 0x04, &g_mu_422540, "count-add(+0x04)", 0);
    patch_count_adder(0x00422660u, 0x0C, &g_mu_422660, "count-add(+0x0C)", 1);
    patch_count_adder(0x004225A0u, 0x20, &g_mu_4225a0, "count-add(+0x20)", 0);
    patch_trap_spike();
}

/* ---------------- old overlay panel (disabled, kept for reference) --------- */
#if 0
static const wchar_t *WCHECK1 = L"god mode (no damage)";
static const wchar_t *WCHECK2 = L"max XP (auto level-up)";
static const wchar_t *WCHECK3 = L"infinite consumables";
static const wchar_t *WHINT   = L"press ` to show/hide this window";
static const wchar_t *WTITLE  = L"Unepic trainer panel";

static HWND  g_hwnd, g_cb1, g_cb2, g_cb3, g_status, g_hint;
static HFONT g_font;
static HBRUSH g_back;
static volatile LONG g_panel_visible = 0;

static void utf8_to_w(const char *s, wchar_t *out, int n)
{
    int w = MultiByteToWideChar(CP_UTF8, 0, s, -1, out, n);
    if (w <= 0 && n > 0) out[0] = 0;
}

static LRESULT CALLBACK panel_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT: {
        HDC dc = (HDC)w;
        SetTextColor(dc, RGB(235, 235, 235));
        SetBkColor(dc, RGB(24, 24, 28));
        return (LRESULT)g_back;
    }
    case WM_CTLCOLORBTN:
        return (LRESULT)g_back;
    case WM_ERASEBKGND: {
        HDC dc = (HDC)w;
        RECT r; GetClientRect(h, &r);
        FillRect(dc, &r, g_back);
        return 1;
    }
    case WM_NCLBUTTONDOWN:
        if (w == HTCLIENT) {           /* let the whole bar act as a drag handle */
            DefWindowProcW(h, WM_NCLBUTTONDOWN, HTCAPTION, 0);
            return 0;
        }
        break;
    case WM_COMMAND:
        if (HIWORD(w) == BN_CLICKED) {
            if ((HWND)l == g_cb1) g_god    = (SendMessageW(g_cb1, BM_GETCHECK, 0, 0) == BST_CHECKED);
            if ((HWND)l == g_cb2) g_maxxp = (SendMessageW(g_cb2, BM_GETCHECK, 0, 0) == BST_CHECKED);
            if ((HWND)l == g_cb3) {
                g_nocons = (SendMessageW(g_cb3, BM_GETCHECK, 0, 0) == BST_CHECKED);
            }
            logmsg("panel: god=%ld maxxp=%ld nocons=%ld", g_god, g_maxxp, g_nocons);
        }
        return 0;
    case WM_CLOSE:
        ShowWindow(h, SW_HIDE);
        g_panel_visible = 0;
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static void refresh_panel(void)
{
    unsigned s = stats_addr(), e = entity_addr();
    wchar_t buf[256];
    if (!s || !readable((void *)(s + OFF_LEVEL), 4)) return;
    {
        int lvl  = stats_get(s, OFF_LEVEL);
        int xp   = stats_get(s, OFF_XP);
        int need = stats_get(s, OFF_NEED);
        int pts  = stats_get(s, OFF_POINTS);
        int mx = 0, flag = 0;
        float cur = 0.0f;
        if (e) {
            mx  = *(int *)(e + ENT_MAXHP);
            cur = *(float *)(e + ENT_CURHP);
        }
        if (readable((void *)(s + OFF_TAMPER), 4)) flag = *(int *)(s + OFF_TAMPER);
        if (g_status) {
            wsprintfW(buf, L"level %d   HP %.0f / %d   xp %d / %d   points %d",
                      lvl, cur, mx, xp, need, pts);
            SetWindowTextW(g_status, buf);
        }
        if (g_hint) {
            wsprintfW(buf, L"infinite items: %s   god: %s   max xp: %s   tamper flag %d",
                      g_nocons ? L"on" : L"off",
                      g_god ? L"on" : L"off",
                      g_maxxp ? L"on" : L"off", flag);
            SetWindowTextW(g_hint, buf);
        }
    }
}

static int create_panel(void)
{
    WNDCLASSW wc;
    RECT rc;
    int x, y, W = 400, H = 196;

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = panel_proc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = L"UnepicPanelCls";
    wc.hbrBackground = NULL;
    if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        logmsg("RegisterClassW failed err=%lu", GetLastError());
        return 0;
    }
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &rc, 0);
    x = rc.left + 24;
    y = rc.top + 24;

    g_back = CreateSolidBrush(RGB(24, 24, 28));
    g_font = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                         DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                         CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Yu Gothic UI");

    g_hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                             L"UnepicPanelCls", WTITLE, WS_POPUP | WS_VISIBLE,
                             x, y, W, H, NULL, NULL, wc.hInstance, NULL);
    if (!g_hwnd) { logmsg("CreateWindowExW failed err=%lu", GetLastError()); return 0; }

#define MKCHK(txt, cy) CreateWindowExW(0, L"BUTTON", txt, \
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 12, cy, W - 24, 24, \
        g_hwnd, NULL, wc.hInstance, NULL)
    g_cb1 = MKCHK(WCHECK1, 40);
    g_cb2 = MKCHK(WCHECK2, 70);
    g_cb3 = MKCHK(WCHECK3, 100);
    g_status = CreateWindowExW(0, L"STATIC", L"-", WS_CHILD | WS_VISIBLE | SS_LEFT,
                               12, 134, W - 24, 22, g_hwnd, NULL, wc.hInstance, NULL);
    g_hint = CreateWindowExW(0, L"STATIC", WHINT, WS_CHILD | WS_VISIBLE | SS_LEFT,
                             12, 162, W - 24, 22, g_hwnd, NULL, wc.hInstance, NULL);
#undef MKCHK
    SendMessageW(g_cb1, BM_SETCHECK, g_god   ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(g_cb2, BM_SETCHECK, g_maxxp ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(g_cb3, BM_SETCHECK, g_nocons ? BST_CHECKED : BST_UNCHECKED, 0);
    if (g_font) {
        SendMessageW(g_cb1, WM_SETFONT, (WPARAM)g_font, TRUE);
        SendMessageW(g_cb2, WM_SETFONT, (WPARAM)g_font, TRUE);
        SendMessageW(g_cb3, WM_SETFONT, (WPARAM)g_font, TRUE);
        SendMessageW(g_status, WM_SETFONT, (WPARAM)g_font, TRUE);
        SendMessageW(g_hint, WM_SETFONT, (WPARAM)g_font, TRUE);
    }
    ShowWindow(g_hwnd, SW_HIDE);
    g_panel_visible = 0;
    logmsg("panel created hwnd=%p", (void *)g_hwnd);
    return 1;
}

#endif /* old overlay panel */

/* ---------------- ini (video settings live in the loader) ---------------- */
#if 0
static int g_cfg_resize = 1, g_cfg_w = 1920, g_cfg_h = 1440;

static void load_config(void)
{
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s\\unepic_mod.ini", g_dir);
    g_cfg_resize = GetPrivateProfileIntA("video", "resize", 1, path);
    g_cfg_w = GetPrivateProfileIntA("video", "width", 1920, path);
    g_cfg_h = GetPrivateProfileIntA("video", "height", 1440, path);
    if (g_cfg_w < 640 || g_cfg_w > 7680) g_cfg_w = 1920;
    if (g_cfg_h < 480 || g_cfg_h > 4320) g_cfg_h = 1440;
    logmsg("config: resize=%d  %dx%d", g_cfg_resize, g_cfg_w, g_cfg_h);
}

/* ---- window size: the exe picks 1024x768 for screen mode 2 ----------------
 *   0x401491  BE 00 04 00 00   mov esi,1024
 *   0x401496  BF 00 03 00 00   mov edi,768
 *   0x40149b  cmp ebx,2 / je  -> uses esi:edi for the window, else the desktop
 * Rewriting the two immediates is enough; must run before the window exists,
 * so it is called straight from DllMain.
 */
static void patch_window_size(void)
{
    unsigned char *p = (unsigned char *)GetModuleHandleA(NULL) + 0x1491;
    static const unsigned char PRO[] = {
        0xBE,0x00,0x04,0x00,0x00, 0xBF,0x00,0x03,0x00,0x00 };
    if (memcmp(p, PRO, sizeof PRO) != 0) {
        logmsg("window-size pattern differs: %02x %02x %02x %02x %02x | %02x %02x %02x %02x %02x",
               p[0],p[1],p[2],p[3],p[4], p[5],p[6],p[7],p[8],p[9]);
        return;
    }
    DWORD old;
    if (!VirtualProtect(p, sizeof PRO, PAGE_EXECUTE_READWRITE, &old)) {
        logmsg("window-size VirtualProtect failed: %lu", GetLastError());
        return;
    }
    *(unsigned *)(p + 1) = (unsigned)g_cfg_w;
    *(unsigned *)(p + 6) = (unsigned)g_cfg_h;
    VirtualProtect(p, sizeof PRO, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, sizeof PRO);
    logmsg("window size patched to %dx%d", g_cfg_w, g_cfg_h);
}

#endif /* video config moved to the loader */

/* ---------------- give / drop an item through the game's own code ----------
 * 0x422C40 (the game's "create item" routine, used by chest loot) is really
 * three calls, and we mirror them exactly instead of touching container fields:
 *
 *   0x4735E0(this = &slotTemplate, desc, x, y)   fill a 0xAC slot from the item
 *   0x422520(this = &slotTemplate, count)        masked count pair
 *   0x4229A0(this = container, &slotTemplate, x, y, -1)   merge / append
 *
 * Descriptors live in a fixed array: 0x13BF7F8 + index*0x1A4, [+0x00] = type id.
 * Containers: player bag = *(entity+0x1038); ground = *(stats+0x17F7C)+0xBCA4
 * (see ADDR_GROUND_PTR below - the global 0x146C008 is only a transient copy).
 */
/* The bag grows past 64 slots in a normal playthrough (observed 70 while the
 * game was running), so the walk limit has to be generous - each record is
 * checked with readable() anyway.  The old hard cap of 64 made Money read 0
 * and made set_item_count() fail silently once the bag had more than 64 slots. */
#define BAG_MAX_SLOTS 512
#define DESC_ARRAY   0x013BF7F8u
#define DESC_STRIDE  0x1A4u
#define DESC_COUNT   1664u        /* slots in the exe's array; 1603 hold an item */
/* Where the game keeps the pile of items lying on the floor.
 * The level object is *(stats + 0x17F7C) - the same way the player entity is
 * *(stats + 0x17F78); its ground pile is that object + 0xBCA4.  Proven from the
 * game's own drop code at 0x434712:
 *     mov  eax,[ebx+0x17f7c]  / add eax,0xbca4 / mov ds:0x146c008,eax
 *     ... push slot,x,y / call 0x4229A0
 *     mov  ds:0x146c008,0     (0x434761)
 * so 0x0146C008 is only a TRANSIENT copy, set inside that one function - it read
 * 0 while the player was walking around and taking damage, which is exactly why
 * building the record against it fails.  The old GROUND_CONT 0x0087800C was worse
 * still: it is nowhere in the exe, it was a heap value from one earlier session.
 * A ground pile has a non-zero [+0x00] (it is the non-stacking kind), so the
 * header test below only checks [+8] == [+0xC] and the slot count range. */
#define ADDR_GROUND_PTR 0x0146C008u
static int g_ground_src;          /* 1 = level object, 2 = transient global, 0 = none */
typedef void (__attribute__((thiscall)) *slot_mk_t)(void *self, void *desc, int x, int y);
typedef void (__attribute__((thiscall)) *slot_cnt_t)(void *self, int v);
typedef void *(__attribute__((thiscall)) *slot_add_t)(void *self, void *slot, int x, int y, int n);

static unsigned char g_slot_tmpl[0xAC];
static volatile LONG g_give_req;          /* 0 idle, 1 pending */
static volatile LONG g_give_type, g_give_count, g_give_ground;
static volatile LONG g_give_done, g_give_result;
static volatile LONG g_give_post_tick;   /* GetTickCount() when the request was sent */
static volatile LONG g_money_after = -1;  /* count to write after the coin is created */

static unsigned desc_of_type(unsigned type)
{
    unsigned i;
    for (i = 0; i < DESC_COUNT; i++) {
        unsigned d = DESC_ARRAY + i * DESC_STRIDE;
        if (IsBadReadPtr((void *)(size_t)d, 4)) continue;
        if (*(unsigned *)(size_t)d == type) return d;
    }
    return 0;
}

static int bag_give(unsigned type, int count, int to_ground)
{
    unsigned d = desc_of_type(type);
    unsigned C;
    unsigned e = entity_addr();
    unsigned m1 = *(unsigned *)ADDR_MASK1;
    unsigned m2 = *(unsigned *)ADDR_MASK2;
    slot_mk_t mk = (slot_mk_t)0x004735E0u;
    slot_cnt_t setc = (slot_cnt_t)0x00422520u;
    slot_add_t add = (slot_add_t)0x004229A0u;
    void *res = NULL;
    int guard, left, gx = 0, gy = 0;
    int pre4, pre30, iter = 0, n0, n1;

    if (!d || !e) return -1;
    g_ground_src = 0;
    if (to_ground) {
        unsigned s = stats_addr(), lvl = 0;
        if (s && readable((void *)(size_t)(s + OFF_LEVEL_OBJ), 4))
            lvl = *(unsigned *)(s + OFF_LEVEL_OBJ);
        if (lvl) {
            C = lvl + LEVEL_GROUND_OFF;
            g_ground_src = 1;
        } else {
            C = readable((void *)(size_t)ADDR_GROUND_PTR, 4)
                ? *(unsigned *)ADDR_GROUND_PTR : 0;
            if (C) g_ground_src = 2;
        }
        if (!C) {
            logmsg("give: no ground container (stats+%08x=0, *0x%08x=0) - not in a level",
                   OFF_LEVEL_OBJ, ADDR_GROUND_PTR);
            return -3;             /* the game has no level loaded */
        }
    } else {
        C = *(unsigned *)(e + 0x1038);
    }
    if (!C || IsBadReadPtr((void *)(size_t)C, 0x14)) {
        logmsg("give: container %08x unreadable (src=%d)", C, g_ground_src);
        return -2;
    }
    if (count < 1) count = 1;
    if (count > 999) count = 999;   /* the panel allows 1..999; givetest must not exceed it */
    n0 = *(int *)(C + 8);
    /* A header whose slot count is nonsense means C is not a container at all
     * (stale heap address, wrong map object).  Refuse instead of letting
     * 0x4229A0 write through it. */
    if (n0 < 0 || n0 > BAG_MAX_SLOTS || (*(int *)(C + 0xC) != n0)) {
        logmsg("give: container %08x header looks wrong: [+8]=%d [+0xC]=%d (src=%d)",
               C, n0, *(int *)(C + 0xC), g_ground_src);
        return -2;
    }

    memset(g_slot_tmpl, 0, sizeof g_slot_tmpl);
    if (to_ground) {
        gx = (int)*(float *)(e + 0x520);
        gy = (int)*(float *)(e + 0x524);
    }
    mk((void *)g_slot_tmpl, (void *)(size_t)d, gx, gy);
    setc((void *)g_slot_tmpl, count);
    pre4  = (int)(*(unsigned *)(g_slot_tmpl + 0x04) ^ m1);
    pre30 = (int)(*(unsigned *)(g_slot_tmpl + 0x30) ^ m2);
    for (guard = 0; guard < 16; guard++) {
        left = (int)(*(unsigned *)(g_slot_tmpl + 4) ^ m1);
        if (left <= 0) break;
        res = add((void *)(size_t)C, (void *)g_slot_tmpl, gx, gy, -1);
        iter++;
        if (!res) break;
    }
    left = (int)(*(unsigned *)(g_slot_tmpl + 4) ^ m1);
    n1 = *(int *)(C + 8);
    logmsg("give: type=%u desc=%08x cont=%08x src=%d req=%d pre(+4)=%d pre(+0x30)=%d "
           "iter=%d left=%d slots=%d->%d at(%d,%d) slot=%p",
           type, d, C, g_ground_src, count, pre4, pre30, iter, left, n0, n1, gx, gy, res);
    return res ? (left == 0 ? 1 : 2) : 0;
}

static int set_item_count(int type, int v);   /* defined below; used by the money path */

/* runs on the game window's thread, called from game_wndproc */
static void give_do_pending(void)
{
    int r = bag_give((unsigned)g_give_type, (int)g_give_count, (int)g_give_ground);
    if (g_money_after >= 0) {
        LONG v = g_money_after;
        g_money_after = -1;
        if (r) {
            int ok = set_item_count(ITEM_COIN, (int)v);
            logmsg("money create: item 9101 give=%d, set %ld %s",
                   r, v, ok ? "ok" : "FAILED");
        } else {
            logmsg("money create: item 9101 give failed (%d)", r);
        }
    }
    g_give_result = r;
    g_give_done = 1;
    g_give_req = 0;
}

/* 1 = request handed to the game thread, 0 = no game window */
static int give_request(unsigned type, int count, int to_ground)
{
    HWND gw = find_game_window();
    g_give_type = (LONG)type;
    g_give_count = count;
    g_give_ground = to_ground;
    g_give_done = 0;
    g_give_result = 0;
    g_give_req = 1;
    if (!gw) { g_give_req = 0; return 0; }
    g_give_post_tick = (LONG)GetTickCount();
    PostMessageW(gw, WM_APP + 10, 0, 0);
    return 1;
}

/* ---------------- item catalogue (mod\items.tsv + mod\dict\itemNames_XX.tsv)
 * Names are exported once (the dictionaries on disk are encrypted; we dumped
 * them through the game's own loader), so the panel does not have to scan
 * 400 MB of memory.  Category codes must match gen_items_tsv2.py.
 */
#define MAX_ITEMS   2200
#define NAME_MAX    96
#define LANG_COUNT  20
#define LANG_INTERNAL 19

static const char *g_lang_codes[LANG_COUNT] = {
    "en", "sp", "fr", "ge", "it", "pt", "br", "cz", "po", "hu", "nl", "gr",
    "ca", "eu", "ru", "jp", "kr", "ch", "tw", "internal" };
static const wchar_t *g_lang_labels[LANG_COUNT] = {
    L"English", L"Espanol", L"Francais", L"Deutsch", L"Italiano", L"Portugues",
    L"Portugues BR", L"Cestina", L"Polski", L"Magyar", L"Nederlands", L"Greek",
    L"Catala", L"Euskara", L"Russian", L"Japanese", L"Korean", L"CN simplified",
    L"CN traditional", L"(internal name)" };
/* Category axis = the game's own descriptor flag (+0x4C, 19 distinct values).
 * Flag 16384 is a grab-bag (623 of the 1603 items), so gen_items_tsv2.py splits
 * it by the developer name in the descriptor ("edit bicho*" -> creature
 * spawner, other "edit *" -> editor object, *receta* -> recipe, tomo/libro ->
 * book, pocion -> potion, llave/key -> key, 7201..7243 -> craft material);
 * the 13 zone keys that the game files under flag 8192 (essence) are moved to
 * "key" by name; "quest item" comes from the game's own name-table tag.  These labels are
 * ours, the grouping is the game's.  Order must match CATS in
 * gen_items_tsv2.py and the category column of mod\items.tsv.
 */
#define CAT_COUNT 24
static const wchar_t *g_cat_names[CAT_COUNT] = {
    L"weapon (hand)", L"great sword", L"polearm", L"bow / crossbow / wand", L"light source",
    L"armor / clothing", L"ring", L"familiar", L"currency", L"magic essence",
    L"spell", L"buff / cure", L"recipe", L"projectile / ammo", L"loot table / creator",
    L"spell book / scroll", L"potion / vial", L"key", L"creature spawner",
    L"editor object", L"craft material", L"quest item", L"usable / misc",
    L"special / misc" };

typedef struct { unsigned id; int cat; int nidx; unsigned char named;
                 wchar_t name[NAME_MAX]; wchar_t internal[NAME_MAX]; } ItemRec;
static ItemRec g_items[MAX_ITEMS];
static int g_nitems;
static int g_sel[MAX_ITEMS], g_nsel;
static int g_lang;                       /* index into g_lang_codes */
static int g_cat = -1;                   /* -1 = all categories */
static int g_hidenamed;                  /* hide rows with no dictionary name */

static void utf8w(const char *src, wchar_t *dst, int cap)
{
    int n;
    dst[0] = 0;
    if (!src || !*src) return;
    n = MultiByteToWideChar(CP_UTF8, 0, src, -1, dst, cap - 1);
    if (n <= 0) { int i; for (i = 0; src[i] && i < cap - 1; i++) dst[i] = (wchar_t)(unsigned char)src[i];
                  dst[src[i] ? cap - 1 : i] = 0; }
}

/* The shipped dictionaries are not complete: some entries are placeholders
 * ("x", "xxx", ".", "-") and some indexes have no entry at all for the chosen
 * language.  Those are unusable as display names. */
static int name_usable(const wchar_t *s)
{
    int i;
    if (!s || !s[0]) return 0;
    for (i = 0; s[i]; i++) {
        wchar_t c = s[i];
        if (c == L'x' || c == L'X' || c == L'.' || c == L'-' || c == L'_' ||
            c == L'*' || c == L' ' || c == 0x00B7 || c == 0x2022) continue;
        return 1;
    }
    return 0;
}

/* read mod\dict\itemNames_<code>.tsv into tbl[]/seen[]; 1 = file was read */
static int names_load(const char *code, wchar_t (*tbl)[NAME_MAX], char *seen)
{
    char path[MAX_PATH], line[512];
    FILE *f;
    memset(tbl, 0, 2048 * sizeof *tbl);
    memset(seen, 0, 2048);
    _snprintf(path, sizeof path, "%s\\mod\\dict\\itemNames_%s.tsv", g_dir, code);
    f = fopen(path, "rb");
    if (!f) { logmsg("items: missing %s", path); return 0; }
    {
        int first = 1;
        while (fgets(line, sizeof line, f)) {
            char *tab, *v;
            long idx;
            int len = (int)strlen(line);
            while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = 0;
            if (first) {
                unsigned char *q = (unsigned char *)line;
                if (len >= 3 && q[0] == 0xEF && q[1] == 0xBB && q[2] == 0xBF)
                    memmove(line, line + 3, strlen(line + 2) + 1);
                first = 0;
            }
            tab = strchr(line, '\t');
            if (!tab) continue;
            *tab = 0;
            idx = strtol(line, NULL, 10);
            if (idx < 0 || idx >= 2048) continue;
            v = tab + 1;
            {                                  /* "primary|alt ...||tag" */
                char shortbuf[NAME_MAX];
                int k = 0;
                while (*v && *v != '|' && *v != '\r' && *v != '\n' && k < (int)sizeof shortbuf - 1)
                    shortbuf[k++] = *v++;
                shortbuf[k] = 0;
                utf8w(shortbuf, tbl[idx], NAME_MAX);
            }
            seen[idx] = 1;
        }
        fclose(f);
    }
    return 1;
}

/* resolve g_items[].name: this language, else English, else the dev name */
static void items_apply_lang(void)
{
    static wchar_t tbl[2048][NAME_MAX];
    static char    seen[2048];
    static wchar_t tbl_en[2048][NAME_MAX];
    static char    seen_en[2048];
    static int     en_loaded;
    int i, n_self = 0, n_en = 0, n_dev = 0;

    if (g_lang == LANG_INTERNAL) {
        for (i = 0; i < g_nitems; i++) {
            wcscpy(g_items[i].name, g_items[i].internal);
            g_items[i].named = 1;        /* that is what this mode is for */
        }
        logmsg("items: names = internal developer name for all %d rows", g_nitems);
        return;
    }
    names_load(g_lang_codes[g_lang], tbl, seen);
    if (g_lang != 0 && !en_loaded) { names_load("en", tbl_en, seen_en); en_loaded = 1; }
    for (i = 0; i < g_nitems; i++) {
        int n = g_items[i].nidx;
        g_items[i].name[0] = 0;
        if (n >= 0 && n < 2048 && seen[n] && name_usable(tbl[n])) {
            wcscpy(g_items[i].name, tbl[n]);
            g_items[i].named = 1;
            n_self++;
        } else if (g_lang != 0 && n >= 0 && n < 2048 && seen_en[n] && name_usable(tbl_en[n])) {
            wcscpy(g_items[i].name, tbl_en[n]);
            g_items[i].named = 1;
            n_en++;
        } else {
            wcscpy(g_items[i].name, g_items[i].internal);
            g_items[i].named = 0;        /* the game itself has no name here */
            n_dev++;
        }
    }
    logmsg("items: lang=%s names in this language=%d english fallback=%d dev-name fallback=%d",
           g_lang_codes[g_lang], n_self, n_en, n_dev);
}

/* one-time log sample: proves the name table really resolved into g_items[] */
static void items_log_sample(void)
{
    char u8[NAME_MAX * 3];
    int k;
    for (k = 0; k < g_nitems && k < 3; k++) {
        u8[0] = 0;
        WideCharToMultiByte(CP_UTF8, 0, g_items[k].name, -1, u8, sizeof u8, NULL, NULL);
        logmsg("items[%d] lang=%s id=%u name=%s", k, g_lang_codes[g_lang],
               g_items[k].id, u8);
    }
}

static void items_load(void)
{
    char path[MAX_PATH], line[512];
    FILE *f;
    g_nitems = 0;
    _snprintf(path, sizeof path, "%s\\mod\\items.tsv", g_dir);
    f = fopen(path, "rb");
    if (!f) { logmsg("items: no %s - picker disabled", path); return; }
    while (fgets(line, sizeof line, f) && g_nitems < MAX_ITEMS) {
        char *p = line, *toks[4];
        int nt = 0, len;
        wchar_t *dst;
        while (nt < 4) {
            char *t = strchr(p, '\t');
            toks[nt++] = p;
            if (!t) break;
            *t = 0;
            p = t + 1;
        }
        if (nt < 4) continue;
        len = (int)strlen(toks[3]);
        while (len && (toks[3][len - 1] == '\n' || toks[3][len - 1] == '\r')) toks[3][--len] = 0;
        if (toks[0][0] < '0' || toks[0][0] > '9') continue;      /* comment line */
        dst = g_items[g_nitems].internal;
        utf8w(toks[3], dst, NAME_MAX);
        g_items[g_nitems].id = (unsigned)strtoul(toks[0], NULL, 10);
        g_items[g_nitems].cat = atoi(toks[1]);
        g_items[g_nitems].nidx = atoi(toks[2]);
        g_nitems++;
    }
    fclose(f);
    {
        char p[MAX_PATH];
        cfg_path(p, sizeof p);
        g_lang = 0;
        g_hidenamed = GetPrivateProfileIntA("items", "hidenamed", 0, p);
        {
            char code[16];
            int i;
            GetPrivateProfileStringA("items", "lang", "en", code, sizeof code, p);
            for (i = 0; i < LANG_COUNT; i++)
                if (_stricmp(code, g_lang_codes[i]) == 0) { g_lang = i; break; }
        }
        items_apply_lang();
        logmsg("items: loaded %d entries, lang=%s", g_nitems, g_lang_codes[g_lang]);
        items_log_sample();
    }
}

static int ci_wstristr(const wchar_t *hay, const wchar_t *needle)
{
    wchar_t h[NAME_MAX], n[NAME_MAX];
    int i;
    if (!needle[0]) return 1;
    for (i = 0; hay[i] && i < NAME_MAX - 1; i++) h[i] = (wchar_t)towupper(hay[i]);
    h[i] = 0;
    for (i = 0; needle[i] && i < NAME_MAX - 1; i++) n[i] = (wchar_t)towupper(needle[i]);
    n[i] = 0;
    return wcsstr(h, n) != NULL || wcsstr(hay, needle) != NULL;
}

static void items_filter(const wchar_t *needle)
{
    int i;
    g_nsel = 0;
    for (i = 0; i < g_nitems; i++) {
        if (g_cat >= 0 && g_items[i].cat != g_cat) continue;
        if (g_hidenamed && !g_items[i].named) continue;
        if (needle && needle[0] &&
            !ci_wstristr(g_items[i].name, needle) && !ci_wstristr(g_items[i].internal, needle))
            continue;
        g_sel[g_nsel++] = i;
    }
}

/* ---------------- overlay panel (native Win32 controls) ---------------- */
#define PANEL_W 640
#define PANEL_H 1148
#define ID_CB_GOD    101
#define ID_CB_XP     102
#define ID_CB_ITEM   103
#define ID_CB_JUMP   106
#define ID_CB_TP     107
#define ID_BT_REVEAL 408
#define ID_BT_BUFFS  409
#define ID_BT_MPUNLOCK 410
#define ID_BT_SAVES_MP 411
#define ID_BT_LIGHTS 412
#define ID_BT_LIGHTS_ALL 413
#define ID_CM_ASPECT 140
#define ID_CM_SCALE  150
#define ID_CM_SPEED  160
#define ID_CM_ATK    161
#define ID_BT_SCALE  151
#define ID_ED_PTS    201
#define ID_ED_GOLD   202
#define ID_ED_UN     203
#define ID_BT_PTS    301
#define ID_BT_GOLD   302
#define ID_BT_UN     303
#define ID_BT_PTS_MAX  311
#define ID_BT_GOLD_MAX 312
#define ID_BT_UN_MAX   313
#define ID_BT_FORCE  403
#define ID_BT_RETO   404
#define ID_BT_ALLACH 405
#define ID_BT_SAVES  406
#define ID_CM_LANG   180
#define ID_CM_CAT    181
#define ID_ED_FIND   182
#define ID_ED_QTY    183
#define ID_LB_ITEMS  184
#define ID_BT_TOBAG  185
#define ID_BT_TOGRND 186
#define ID_ST_ITEM   187
#define ID_ST_COUNT  188
#define ID_CB_HIDEN  189
#define ID_ST_DEV    190

static HWND  g_hwnd;
static HWND  g_cb_god, g_cb_xp, g_cb_item, g_cb_jump;
static HWND  g_cm_aspect, g_cm_scale, g_bt_scale, g_st_scale;
static HWND  g_cm_speed;
static HWND  g_bt_mpunlock;          /* enabled live, only when the map table is loaded */
static HWND  g_bt_force;             /* Force save: single-player only */
static HWND  g_cm_atk;
static HWND  g_ed_pts, g_ed_gold, g_ed_un;
static HWND  g_v_hp, g_v_lvl, g_v_xp, g_v_gold, g_v_un, g_v_pts;
static HWND  g_cm_lang, g_cm_cat, g_ed_find, g_ed_qty;
static HWND  g_lb_items, g_st_item, g_st_count;
static HWND  g_ck_hiden, g_st_dev, g_desc;
static HWND  g_bt_bag, g_bt_grnd;
static HWND  g_hint;       /* bottom line, reused for short status messages */
#define MOD_NAME    L"Unepic Better Mod"
#define MOD_TITLE   L"Unepic Better Mod  v1.0.1"
#define HINT_DEFAULT L"(C) 2026 Quiet Game Archive"
static HFONT g_ui_font, g_ui_bold;
static int   g_panel_visible;

static int item_count_of_type(int type)
{
    unsigned s = stats_addr();
    unsigned e, cont;
    int n;
    if (!s) return -1;
    e = *(unsigned *)(s + OFF_ENTITY);
    if (!readable((void *)e, 4)) return -1;
    cont = *(unsigned *)(e + 0x1038);
    if (!readable((void *)cont, 0x18)) return -1;
    n = *(int *)(cont + 8);
    if (n < 0 || n > BAG_MAX_SLOTS) return -1;
    for (int i = 0; i < n; i++) {
        unsigned it = cont + 0x10 + i * 0xAC;
        unsigned desc;
        if (!readable((void *)it, 8)) break;
        desc = *(unsigned *)it;
        if (!readable((void *)desc, 4)) continue;
        if ((int)*(unsigned *)desc == type)
            return *(int *)(it + 4) ^ *(int *)ADDR_MASK1;
    }
    return -1;
}

/* ---------------- bag field change detector (capture only) ------------------
 * A ranged shot clearly takes something away, yet 0x422520 / 0x422540 / 0x4225a0
 * / 0x423480 all report no call for it.  So instead of hooking, compare the two
 * masked numbers of every bag record with the previous pass: +0x04/+0x30 is the
 * quantity the icon shows, +0x20/+0x3C is the second counter.  Same record, same
 * type, different value -> one log line with the record address, which tells us
 * what to hook.  With [debug] wpauto=1 the first decrease also arms a hardware
 * write watchpoint on that exact dword, so the same shot prints
 * "WATCH: write by EIP ..." through the VEH handler.  Reads only.
 */
typedef struct { unsigned addr; LONG type; LONG q4; LONG s0c; LONG u20; } BagRec;
static BagRec g_bag_prev[BAG_MAX_SLOTS];
static int    g_bag_prev_n = -1;
static LONG   g_wp_auto_done;
static int    g_wpauto;
static unsigned g_watch_addr;   /* [debug] watch=DA8730 (hex, no 0x) */
static float g_nudge_x, g_nudge_y;   /* [debug] nudge=dx,dy: one-shot entity shift */
static int   g_nudge_wait;           /* ticks left before it fires (0 = nothing) */

static void bag_diff(void)
{
    unsigned s = stats_addr(), e, C, m1, m2;
    int n, i;

    if (!s) return;
    e = *(unsigned *)(s + OFF_ENTITY);
    if (!readable((void *)e, 4)) return;
    C = *(unsigned *)(e + 0x1038);
    if (!readable((void *)C, 0x18)) return;
    n = *(int *)(C + 8);
    if (n < 0 || n > BAG_MAX_SLOTS) return;
    m1 = *(unsigned *)ADDR_MASK1;
    m2 = *(unsigned *)ADDR_MASK2;

    for (i = 0; i < n; i++) {
        unsigned rec = C + 0x10 + (unsigned)i * 0xACu, desc;
        LONG t, q4, q30, s0c, s34, u20, u3c;
        int moved;
        if (!readable((void *)rec, 0x40)) break;
        desc = *(unsigned *)rec;
        t = readable((void *)desc, 4) ? (LONG)*(unsigned *)desc : -1;
        q4  = (LONG)(*(unsigned *)(rec + 0x04) ^ m1);
        q30 = (LONG)(*(unsigned *)(rec + 0x30) ^ m2);
        s0c = (LONG)(*(unsigned *)(rec + 0x0C) ^ m1);
        s34 = (LONG)(*(unsigned *)(rec + 0x34) ^ m2);
        u20 = (LONG)(*(unsigned *)(rec + 0x20) ^ m1);
        u3c = (LONG)(*(unsigned *)(rec + 0x3C) ^ m2);
        moved = (g_bag_prev_n != n || g_bag_prev[i].addr != rec || g_bag_prev[i].type != t);
        if (!moved && (g_bag_prev[i].q4 != q4 || g_bag_prev[i].s0c != s0c ||
                       g_bag_prev[i].u20 != u20)) {
            int dec = (q4 < g_bag_prev[i].q4) || (s0c < g_bag_prev[i].s0c) ||
                      (u20 < g_bag_prev[i].u20);
            logmsg("bagdiff: slot=%d rec=%08x type=%ld qty %ld->%ld shots %ld->%ld"
                   " uses %ld->%ld | now mirrors +0x30=%ld +0x34=%ld +0x3C=%ld%s",
                   i, rec, t, g_bag_prev[i].q4, q4, g_bag_prev[i].s0c, s0c,
                   g_bag_prev[i].u20, u20, q30, s34, u3c,
                   (q30 != q4 || u3c != u20 || s34 != s0c) ? " MIRROR-MISMATCH" : "");
            if (g_wpauto && dec && !g_wp_auto_done) {
                unsigned a = (s0c < g_bag_prev[i].s0c) ? rec + 0x0Cu
                         : (q4 < g_bag_prev[i].q4) ? rec + 0x04u : rec + 0x20u;
                g_wp_auto_done = 1;
                logmsg("bagdiff: arming write watchpoint on %08x (wpauto)", a);
                wp_arm(a);
            }
        }
        g_bag_prev[i].addr = rec;
        g_bag_prev[i].type = t;
        g_bag_prev[i].q4 = q4;
        g_bag_prev[i].s0c = s0c;
        g_bag_prev[i].u20 = u20;
    }
    g_bag_prev_n = n;
}

static int set_item_count(int type, int v)
{
    unsigned s = stats_addr();
    unsigned e, cont;
    int n, m1, m2;
    if (!s) return 0;
    e = *(unsigned *)(s + OFF_ENTITY);
    if (!readable((void *)e, 4)) return 0;
    cont = *(unsigned *)(e + 0x1038);
    if (!readable((void *)cont, 0x18)) return 0;
    n = *(int *)(cont + 8);
    if (n < 0 || n > BAG_MAX_SLOTS) return 0;
    m1 = *(int *)ADDR_MASK1;
    m2 = *(int *)ADDR_MASK2;
    for (int i = 0; i < n; i++) {
        unsigned it = cont + 0x10 + i * 0xAC;
        unsigned desc;
        if (!readable((void *)it, 0x34)) break;
        desc = *(unsigned *)it;
        if (!readable((void *)desc, 4)) continue;
        if ((int)*(unsigned *)desc == type) {
            *(int *)(it + 0x04) = v ^ m1;
            *(int *)(it + 0x30) = v ^ m2;
            return 1;
        }
    }
    return 0;
}

/* Money is item 9101 in both modes.  Rewrite it when the record exists; if an
 * MP bag has no coin at all, ask the game thread to create one through the same
 * item-add path the Items panel uses, then write the requested count there. */
static int money_set(int v)
{
    int ok = set_item_count(ITEM_COIN, v);
    if (ok) {
        logmsg("set money = %d ok (item 9101, mp=%d)", v, game_is_mp());
        return 1;
    }
    if (!game_is_mp()) {
        logmsg("set money = %d FAILED - no coin record (item 9101, mp=0)", v);
        return 0;
    }
    if (g_give_req) {
        logmsg("set money = %d deferred - another give is pending", v);
        return 0;
    }
    g_money_after = (LONG)v;
    if (!give_request(ITEM_COIN, 1, 0)) {
        g_money_after = -1;
        logmsg("set money = %d FAILED - no game window", v);
        return 0;
    }
    logmsg("set money = %d: no coin record, creating item 9101 on the game thread", v);
    return 2;      /* pending; the 250 ms panel refresh will show the result */
}

/* UN is derived every frame: sum(ledger[i] where flag[i]==2) - stats+0x170 */
static int un_total(void)
{
    const int *a = (const int *)0x54DE18;
    const int *b = (const int *)0x1B010F8;
    int sum = 0;
    if (IsBadReadPtr(a, 60) || IsBadReadPtr(b, 60)) return -1;
    for (int i = 0; i < 15; i++) if (b[i] == 2) sum += a[i];
    return sum;
}

static int set_un(int v)
{
    unsigned s = stats_addr();
    int total = un_total();
    if (!s || total < 0) return 0;
    *(int *)(s + 0x170) = total - v;
    return 1;
}

static int un_current(void)
{
    unsigned s = stats_addr();
    int total = un_total();
    if (!s || total < 0) return -1;
    return total - *(int *)(s + 0x170);
}

/* ---------------- challenges ("retos") ------------------------------------
 * State: DWORD[15] at RETO_STATES, 0 = open, 2 = completed.  The game's own
 * update at 0x428002 copies that into each challenge record (rec+0x14), which
 * is why simply writing the array already ticks the list.
 *
 * Writing the array is NOT enough for the achievement, though.  The game's own
 * completion routine is 0x454DD0 (thiscall, ecx = RETO_STATES, arg = index):
 *
 *   0x454dee  call 0x454b10        ; the real condition test for this challenge
 *   0x454df7  lea  eax,[index+0x64] ; achievement id = 100 + index
 *   0x454dfb  mov  ecx,0xda7270
 *   0x454e00  call 0x432760        ; unlock_achievement(id)
 *   0x454e05  mov  [flags+index*4],2
 *   0x454e0c  call [ds:0x523304]   ; SteamUserStats()
 *   0x454e19  call [vtable+0x28]   ; StoreStats()
 *
 * The condition test at the top is why we cannot just call 0x454DD0: it would
 * only complete challenges that were genuinely earned.  So we replay the part
 * after the gate: unlock_achievement(100+i), then the flag, then StoreStats().
 * unlock_achievement (0x432760) itself does the in-house record
 * (0x4305d0/0x439ee0 through the save object at 0x71c5c0) and, for the plain
 * achievements (id < 100), the Steam SetAchievement + StoreStats pair.
 * It returns early when the gate dword at 0x672334 is non zero - logged.
 *
 * Must run on the game thread (Steam API + the game's own save object), so the
 * button posts WM_APP+12 and the worker picks the result up like the give path.
 *
 * The ledger at 0x0054DE18 holds each challenge's UN reward (that is what
 * un_total() sums), which is why the UN readout jumps when challenges close.
 * Order follows dictio_challenges titles (entries 0,3,6...42): A GOOD COMMENCE,
 * WET FLOOR WATCH YOUR STEP, THE POTION COLLECTOR, RUNNING OVER TRAPS, A HAND
 * WITH VAMPIRES, DRY LAW IN ARRAKIS, THE DEVILS NUMBER, DEATH LEVEL 1, THE KING
 * OF DODGE, EYE FOR AN EYE, DIE HARD, MISSION NEARLY IMPOSSIBLE, THE CROW,
 * A LONG WAY TO MINERARY, THE TOY HAMMER.  (Names from the dumped dictionary;
 * the one-by-one point mapping is inferred from array order and was not
 * individually verified; the Steam keys RETO_* sit at 0x54dd48+4*id.)
 */
#define RETO_STATES   0x01B010F8u
#define RETO_COUNT    15
#define FN_RETO_DONE  0x00454DD0u      /* reference only: gated by the real test */
#define FN_ACH_UNLOCK 0x00432760u      /* thiscall(self, int id) */
#define ACH_SELF      0x00DA7270u      /* the exe's static stats object */
#define ACH_GATE      0x00672334u      /* non zero -> unlock_achievement no-ops */
#define IAT_USERSTATS 0x00523304u      /* call [ds:0x523304] = SteamUserStats() */
#define RETO_ACH_FIRST 100             /* challenge i owns achievement 100+i */
#define RETO_DESC_BASE 0x0054E690u     /* 15 challenge descriptors, stride 0x190 */
#define RETO_DESC_STRIDE 0x190u
#define RETO_DESC_NAME 0x04u           /* +0x04 = Steam key, "RETO_INICIO" etc. */

static volatile LONG g_reto_req, g_reto_done, g_reto_result, g_reto_unlocked;
static volatile LONG g_reto_post_tick;

/* runs on the game window's thread, called from game_wndproc */
static int retos_complete_all(void)
{
    typedef int (__attribute__((thiscall)) *ach_t)(void *self, int id);
    typedef unsigned char (__attribute__((thiscall)) *seta_t)(void *self, const char *key);
    typedef void (__attribute__((thiscall)) *store_t)(void *self);
    typedef void *(*fn0_t)(void);
    typedef void *(__attribute__((thiscall)) *rec_t)(void *self, void *arg);
    volatile unsigned *f = (volatile unsigned *)RETO_STATES;
    ach_t unlock = (ach_t)FN_ACH_UNLOCK;
    int gate = *(int *)ACH_GATE;
    int i, changed = 0, ok = 0, steam = 0, idx[RETO_COUNT];
    void *rec = ((rec_t)0x004305D0u)((void *)0x0071C5C0u, (void *)ACH_SELF);

    /* unlock_achievement returns 1 even when this record object is missing (it
     * just skips the marking), so log it - it is the only hard evidence that
     * the achievement really went through the game's own bookkeeping. */
    logmsg("retos: achievement record from 0x4305D0 = %p%s", rec,
           rec ? "" : " - unlocks will be counted but not recorded");

    if (IsBadWritePtr((void *)f, RETO_COUNT * 4)) {
        logmsg("retos: state array not writable");
        return -1;
    }
    for (i = 0; i < RETO_COUNT; i++) {
        if (f[i] == 2) continue;
        if (!gate) {
            if (unlock((void *)ACH_SELF, RETO_ACH_FIRST + i) == 1) ok++;
            else logmsg("retos: unlock_achievement(%d) did not report success",
                        RETO_ACH_FIRST + i);
        }
        f[i] = 2;
        idx[changed++] = i;
    }
    /* unlock_achievement only marks the internal record for ids >= 100; the
     * SetAchievement call it makes from its own code path lives in the id < 100
     * branch (0x4327d0: name = [id*4 + 0x54dd48]).  Challenge keys are not in
     * that table - each challenge descriptor keeps its own at +0x04:
     *   0x54e694 RETO_INICIO, 0x54e824 RETO_VOLAR_1, 0x54e9b4 RETO_POCIONES ...
     * (stride 0x190), which is why marking the challenges alone never reached
     * Steam.  Push them here the same way the game does, then StoreStats once. */
    if (changed) {
        fn0_t get_us = *(fn0_t *)(size_t)IAT_USERSTATS;
        void *us = get_us();
        if (us) {
            void **vt = *(void ***)us;
            seta_t seta = (seta_t)vt[0x1C / sizeof(void *)];
            store_t store = (store_t)vt[0x28 / sizeof(void *)];
            for (i = 0; i < changed; i++) {
                const char *key = *(const char **)(RETO_DESC_BASE +
                        RETO_DESC_STRIDE * (unsigned)idx[i] + RETO_DESC_NAME);
                int r;
                if (!key || IsBadReadPtr((void *)key, 4)) {
                    logmsg("retos: no Steam key in descriptor %d", idx[i]);
                    continue;
                }
                r = seta(us, key);
                logmsg("retos: SetAchievement(\"%s\") -> %d", key, r);
                if (r) steam++;
            }
            store(us);
        } else {
            logmsg("retos: SteamUserStats() returned null - no Steam, achievements skipped");
        }
    }
    logmsg("retos: %d challenge(s) closed, %d internal mark(s), %d Steam SetAchievement ok,"
           " gate(0x%08x)=%d", changed, ok, steam, ACH_GATE, gate);
    g_reto_unlocked = steam;
    return changed;
}

/* The bottom line is a fixed copyright notice now; button results used to
 * flash there, but the user wants the notice only.  Keep the function so every
 * call site still reports, just send the text to the log instead of the UI. */
static void hint_flash(const wchar_t *s)
{
    char b[256];
    if (!s) return;
    b[0] = 0;
    WideCharToMultiByte(CP_UTF8, 0, s, -1, b, sizeof b, NULL, NULL);
    b[sizeof b - 1] = 0;
    logmsg("status: %s", b);
}

static void retos_report(int result, int unlocked)
{
    wchar_t b[96];
    if (result < 0) hint_flash(L"the state array is not writable");
    else if (result == 0 && unlocked == 0) hint_flash(L"all 15 were already done");
    else {
        wsprintfW(b, L"%d challenge(s) closed, %d Steam unlock(s)", result, unlocked);
        hint_flash(b);
    }
}

/* panel thread: confirm first (Steam pops one notification per challenge), then
 * hand the work to the game thread and wait for the tick */
static void retos_button(void)
{
    volatile unsigned *f = (volatile unsigned *)RETO_STATES;
    static const wchar_t TITLE[] = L"Complete all challenges";
    wchar_t msg[256];
    HWND gw;
    int i, open = 0;

    if (IsBadReadPtr((void *)f, RETO_COUNT * 4)) {
        hint_flash(L"the state array is not readable");
        return;
    }
    for (i = 0; i < RETO_COUNT; i++) if (f[i] != 2) open++;
    if (!open) { hint_flash(L"all 15 challenges are already done"); return; }

    wsprintfW(msg, L"%d challenge(s) will be marked done.\r\n\r\n"
                   L"They are closed through the game's own path, so Steam pops "
                   L"%d achievement notification(s) and pushes them to your "
                   L"account.\r\nUnlocked achievements cannot be taken back.\r\n\r\n"
                   L"Save first if you want to be safe (Force save).", open, open);
    if (MessageBoxW(g_hwnd, msg, TITLE,
                    MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2) != IDOK) {
        logmsg("retos: cancelled at the confirmation dialog");
        hint_flash(L"cancelled - nothing was changed");
        return;
    }
    gw = find_game_window();
    if (!gw) { hint_flash(L"game window not found"); return; }
    g_reto_req = 1;
    g_reto_done = 0;
    g_reto_result = 0;
    g_reto_unlocked = 0;
    g_reto_post_tick = (LONG)GetTickCount();
    hint_flash(L"asking the game thread...");
    PostMessageW(gw, WM_APP + 12, 0, 0);
}

/* ---------------- the 52 plain achievements (logros) -----------------------
 * ids 0..51, unlocked-state bytes at stats+0xE0 (verified: all readable, 52 of
 * them).  0x432760's id < 100 branch does the whole job itself: mark the byte,
 * SetAchievement(name from [id*4+0x54DD48]) and StoreStats - unlike the challenge
 * ids >= 100, which only get the internal record.  So all we do is loop.
 */
#define ACH_COUNT 52
#define ACH_BYTES (ACH_SELF + 0xE0u)
#define ACH_NAMES 0x0054DD48u   /* char*[52], the LOGRO_* Steam keys */
static volatile LONG g_ach_req, g_ach_done, g_ach_result, g_ach_ok, g_ach_post_tick;

static int ach_unlock_all(void)          /* runs on the game thread */
{
    typedef unsigned char (__attribute__((thiscall)) *seta_t)(void *self, const char *key);
    typedef void (__attribute__((thiscall)) *store_t)(void *self);
    typedef void *(*fn0_t)(void);
    unsigned char *b = (unsigned char *)(size_t)ACH_BYTES;
    int gate = *(int *)ACH_GATE;
    void *us;
    void **vt;
    seta_t seta;
    store_t store;
    int i, locked = 0, ok = 0;

    if (IsBadReadPtr(b, ACH_COUNT) || IsBadWritePtr(b, ACH_COUNT)) {
        logmsg("ach: unlocked bytes at 0x%08x are not accessible", ACH_BYTES);
        return -1;
    }
    if (gate) {
        logmsg("ach: gate(0x%08x)=%d - the game blocks achievements", ACH_GATE, gate);
        return -2;
    }
    us = ((fn0_t)(size_t)*(void **)(size_t)IAT_USERSTATS)();
    if (!us) { logmsg("ach: SteamUserStats() returned null - no Steam"); return -3; }
    vt = *(void ***)us;
    seta = (seta_t)vt[0x1C / sizeof(void *)];
    store = (store_t)vt[0x28 / sizeof(void *)];

    /* unlock_achievement(0x432760) is NOT used here on purpose: for ids < 100 it
     * first appends an (id, type) pair to a fixed 16-slot record queue at
     * record+8 (count at +0xC0) and 0x439EE0 refuses once that queue is full,
     * so a batch of 33 stopped after 16 (live log: "33 locked, 16 unlocked",
     * ids 47-49 "did not report success").  We do exactly what its tail does:
     * SetAchievement(name) -> mark the byte -> StoreStats once at the end. */
    for (i = 0; i < ACH_COUNT; i++) {
        const char *name;
        unsigned char r;
        if (b[i]) continue;
        locked++;
        name = *(const char **)(size_t)(ACH_NAMES + (unsigned)i * 4u);
        if (!name || IsBadReadPtr((void *)name, 4)) {
            logmsg("ach: id %d has no readable name pointer", i);
            continue;
        }
        r = seta(us, name);
        logmsg("ach: SetAchievement(\"%s\") -> %d", name, r);
        if (r) { b[i] = 1; ok++; }
    }
    store(us);
    logmsg("ach: %d logro were locked, %d unlocked through SetAchievement", locked, ok);
    {
        int ok_logro = ok;
        int closed = retos_complete_all();      /* flags + RETO_* + its own StoreStats */
        int reto_ok = (int)g_reto_unlocked;
        if (closed < 0) closed = 0;
        else ok += reto_ok;
        logmsg("ach: %d challenge(s) closed, %d RETO* pushed; logro=%d total=%d",
               closed, reto_ok, ok_logro, ok);
        g_ach_ok = ok;
        return locked + closed;
    }
}

static void ach_report(int result, int ok)
{
    wchar_t b[128];
    if (result == -1) hint_flash(L"achievement bytes not accessible");
    else if (result == -2) hint_flash(L"the game blocks achievements right now");
    else if (result == 0) hint_flash(L"all 57 achievements are already unlocked");
    else {
        wsprintfW(b, L"%d achievement(s) unlocked", ok);
        hint_flash(b);
    }
}

static void ach_button(void)
{
    const unsigned char *b = (const unsigned char *)(size_t)ACH_BYTES;
    volatile unsigned *f = (volatile unsigned *)RETO_STATES;
    static const wchar_t TITLE[] = L"Unlock all achievements";
    wchar_t msg[340];
    HWND gw;
    int i, locked = 0, open_ch = 0;

    if (IsBadReadPtr((void *)b, ACH_COUNT) || IsBadReadPtr((void *)f, RETO_COUNT * 4)) {
        hint_flash(L"achievement data not readable");
        return;
    }
    for (i = 0; i < ACH_COUNT; i++) {
        const char *nm;
        if (b[i]) continue;
        nm = *(const char **)(size_t)(ACH_NAMES + (unsigned)i * 4u);
        if (!nm || IsBadReadPtr((void *)nm, 4) || !nm[0]) continue;   /* no Steam name */
        locked++;
    }
    for (i = 0; i < RETO_COUNT; i++) if (f[i] != 2) open_ch++;
    if (!locked && !open_ch) {
        hint_flash(L"all 57 achievements are already unlocked");
        return;
    }
    wsprintfW(msg, L"%d logro achievement(s) and %d challenge(s) are still locked.\r\n\r\n"
                   L"This unlocks all 57: the 42 logros, and it also marks the challenge "
                   L"list complete and pushes the 15 RETO_* achievements.\r\n\r\n"
                   L"Expect up to %d Steam notification(s). Unlocked achievements cannot "
                   L"be taken back.\r\n\r\n"
                   L"Save first if you want to be safe (Force save).",
                   locked, open_ch, locked + open_ch);
    if (MessageBoxW(g_hwnd, msg, TITLE,
                    MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2) != IDOK) {
        logmsg("ach: cancelled at the confirmation dialog");
        hint_flash(L"cancelled - nothing was changed");
        return;
    }
    gw = find_game_window();
    if (!gw) { hint_flash(L"game window not found"); return; }
    g_ach_req = 1;
    g_ach_done = 0;
    g_ach_result = 0;
    g_ach_ok = 0;
    g_ach_post_tick = (LONG)GetTickCount();
    hint_flash(L"asking the game thread...");
    PostMessageW(gw, WM_APP + 13, 0, 0);
}

/* ---- display helpers ---- */
static void aspect_base(int asp, int *bw, int *bh)
{
    if (asp == 1)      { *bw = 1280; *bh = 720; }
    else if (asp == 2) { *bw = 1280; *bh = 800; }
    else               { *bw = 1024; *bh = 768; }
}

/* largest multiplier in tenths (10 = 1.0x), rounded down to the 0.2 step */
static int max_scale10_for(int asp)
{
    int bw, bh, sw, sh, m;
    aspect_base(asp, &bw, &bh);
    sw = GetSystemMetrics(SM_CXSCREEN);
    sh = GetSystemMetrics(SM_CYSCREEN);
    m = sw * 10 / bw;
    if (sh * 10 / bh < m) m = sh * 10 / bh;
    m = m / 2 * 2;
    if (m < 10) m = 10;
    if (m > 50) m = 50;
    return m;
}

static void scale_items(void)
{
    int asp = (int)SendMessageW(g_cm_aspect, CB_GETCURSEL, 0, 0);
    int max10, s10, bw, bh;
    wchar_t item[64];
    if (asp < 0) asp = 0;
    aspect_base(asp, &bw, &bh);
    max10 = max_scale10_for(asp);
    SendMessageW(g_cm_scale, CB_RESETCONTENT, 0, 0);
    for (s10 = 10; s10 <= max10; s10 += 2) {
        wsprintfW(item, L"%d.%dx   %d x %d", s10 / 10, s10 % 10,
                  (bw * s10 + 5) / 10, (bh * s10 + 5) / 10);
        SendMessageW(g_cm_scale, CB_ADDSTRING, 0, (LPARAM)item);
    }
    SendMessageW(g_cm_scale, CB_SETCURSEL, 0, 0);
}

static HWND mk_ctl(const wchar_t *cls, const wchar_t *text, DWORD style,
                   int x, int y, int w, int h, int id, HFONT f)
{
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style,
                             x, y, w, h, g_hwnd, (HMENU)(INT_PTR)id,
                             (HINSTANCE)GetModuleHandleA(NULL), NULL);
    if (c && f) SendMessageW(c, WM_SETFONT, (WPARAM)f, TRUE);
    return c;
}

static void panel_sync_checks(void)
{
    SendMessageW(g_cb_god,  BM_SETCHECK, g_god    ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(g_cb_xp,   BM_SETCHECK, g_maxxp  ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(g_cb_item, BM_SETCHECK, g_nocons ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(g_cb_jump, BM_SETCHECK, g_airjump ? BST_CHECKED : BST_UNCHECKED, 0);
    if (g_cb_tp) SendMessageW(g_cb_tp, BM_SETCHECK, g_teleport ? BST_CHECKED : BST_UNCHECKED, 0);
}

static void panel_refresh(void)
{
    unsigned s = stats_addr(), e = entity_addr();
    wchar_t b[64];
    int lvl = 0, xp = 0, need = 0, pts = 0, mx = 0, gold, un;
    int hp = 0;
    if (s) {
        lvl = stats_get(s, OFF_LEVEL); xp = stats_get(s, OFF_XP);
        need = stats_get(s, OFF_NEED); pts = stats_get(s, OFF_POINTS);
    }
    if (e) { mx = *(int *)(e + ENT_MAXHP); hp = (int)*(float *)(e + ENT_CURHP); }
    gold = item_count_of_type(ITEM_COIN);
    un   = un_current();
    wsprintfW(b, L"%d / %d", hp, mx);              SetWindowTextW(g_v_hp, b);
    wsprintfW(b, L"%d", lvl);                      SetWindowTextW(g_v_lvl, b);
    wsprintfW(b, L"%d / %d", xp, need);            SetWindowTextW(g_v_xp, b);
    wsprintfW(b, L"%d", gold < 0 ? 0 : gold);      SetWindowTextW(g_v_gold, b);
    wsprintfW(b, L"%d", un < 0 ? 0 : un);          SetWindowTextW(g_v_un, b);
    wsprintfW(b, L"%d", pts);                      SetWindowTextW(g_v_pts, b);
}

static void panel_fill_edits(void)
{
    unsigned s = stats_addr();
    wchar_t b[32];
    int pts = s ? stats_get(s, OFF_POINTS) : 0;
    int gold = item_count_of_type(ITEM_COIN), un = un_current();
    char ini[MAX_PATH];
    int asp, s10, max10, idx;
    wsprintfW(b, L"%d", pts);   SetWindowTextW(g_ed_pts, b);
    wsprintfW(b, L"%d", gold < 0 ? 0 : gold); SetWindowTextW(g_ed_gold, b);
    wsprintfW(b, L"%d", un < 0 ? 0 : un);     SetWindowTextW(g_ed_un, b);

    cfg_path(ini, sizeof ini);
    asp = GetPrivateProfileIntA("video", "aspect", 0, ini);
    if (asp < 0 || asp > 2) asp = 0;
    SendMessageW(g_cm_aspect, CB_SETCURSEL, asp, 0);
    scale_items();
    s10 = GetPrivateProfileIntA("video", "scale", 20, ini);
    if (s10 < 10) s10 *= 10;          /* old whole number */
    max10 = max_scale10_for(asp);
    if (s10 > max10) s10 = max10;
    if (s10 < 10) s10 = 10;
    idx = (s10 - 10) / 2;
    SendMessageW(g_cm_scale, CB_SETCURSEL, idx, 0);
    {
        int sp = GetPrivateProfileIntA("cheats", "speed", 100, ini);
        int si = sp / 100 - 1;
        if (si < 0) si = 0;
        if (si > 9) si = 9;
        SendMessageW(g_cm_speed, CB_SETCURSEL, si, 0);
    }
    {
        int a10 = GetPrivateProfileIntA("cheats", "atkspd", 10, ini);
        int ai = a10 / 10 - 1;
        if (ai < 0) ai = 0;
        if (ai > 9) ai = 9;
        SendMessageW(g_cm_atk, CB_SETCURSEL, ai, 0);
    }
}

static void panel_display_state(void)
{
    int win = game_windowed();
    EnableWindow(g_cm_aspect, win);
    EnableWindow(g_cm_scale, win);
    EnableWindow(g_bt_scale, win);
    SetWindowTextW(g_st_scale, win ? L"applies on next launch"
                                   : L"switch the game to windowed mode first");
}

static int edit_int(HWND h, int def)
{
    wchar_t b[32];
    GetWindowTextW(h, b, 32);
    if (!b[0]) return def;      /* empty box: use the caller's documented default */
    return (int)wcstol(b, NULL, 10);
}

static void force_save(void)
{
    HWND gw = find_game_window();
    unsigned e = entity_addr();
    if (game_is_mp()) {
        MessageBoxW(g_hwnd,
                    L"Force save is single-player only.\r\n\r\n"
                    L"In multiplayer, save from the game's own menu.",
                    L"Force save", MB_OK | MB_ICONINFORMATION);
        logmsg("force save: refused in multiplayer (SP only)");
        return;
    }
    if (!gw) {
        MessageBoxW(g_hwnd, L"Game window not found.", MOD_NAME, MB_OK | MB_ICONWARNING);
        return;
    }
    if (!e) {
        MessageBoxW(g_hwnd, L"Load into a level first.", MOD_NAME, MB_OK | MB_ICONWARNING);
        return;
    }
    logmsg("force save: requested (entity %08x)", e);
    PostMessageW(gw, WM_APP + 8, 0, 0);
}

/* [debug] warptest=<mapId> - one-shot map load, on the game thread.
 * Calls the routine the game itself uses for its "load map" action
 * (0x00471CA0, entry at 0x0041B6E6: this=0x17CA610, arg1=map id, arg2=that
 * caller's buffer - passed as 0 here).  It resets the stats object and builds
 * a new level, so it can leave the session broken: save first.
 */
static int g_warp_id = -1;

/* [debug] roomwarp=<cell> - one-shot: load the room a map cell belongs to.
 * 0x0042E030 is what the game itself calls on a door transition
 * (this=0x7E10D8, zone, room, 0); the 16x16 cell records at 0x00B03008 hold
 * zone at +0x2B8 and room at +0x2B0 (verified against a live transition:
 * zone 1 / room 12 -> call (1, 12, 0)).
 */
static int g_roomwarp_cell = -1;
static int g_wb_zone = -1, g_wb_room = -1;   /* [debug] warpbyid=zone,room */

static void roomwarp_fire(void)
{
    HWND gw = find_game_window();
    if (!gw) { logmsg("roomwarp: no game window"); return; }
    logmsg("roomwarp: posting cell=%d", g_roomwarp_cell);
    PostMessageW(gw, WM_APP + 15, 0, 0);
}

static void warpbyid_fire(void)
{
    HWND gw = find_game_window();
    if (!gw) { logmsg("warpbyid: no game window"); return; }
    logmsg("warpbyid: posting zone=%d room=%d", g_wb_zone, g_wb_room);
    PostMessageW(gw, WM_APP + 16, 0, 0);
}

static void warp_test_fire(void)
{
    HWND gw = find_game_window();
    if (!gw) { logmsg("warptest: no game window"); return; }
    logmsg("warptest: posting mapId=%d", g_warp_id);
    PostMessageW(gw, WM_APP + 14, 0, 0);
}

static void panel_apply(int which)
{
    unsigned s = stats_addr();
    if (which == 0 && s) {
        int v = edit_int(g_ed_pts, 0);
        if (v < 0) v = 0;
        if (v > 999) v = 999;
        stats_set(s, OFF_POINTS, v);
        logmsg("set points = %d", v);
    } else if (which == 1) {
        int v = edit_int(g_ed_gold, 2);
        if (v < 2) v = 2;
        if (v > 999999) v = 999999;
        if (money_set(v) == 2) return;   /* coin creation is pending on the game thread */
    } else if (which == 2) {
        int v = edit_int(g_ed_un, 0);
        if (v < 0) v = 0;
        if (v > 999999) v = 999999;
        set_un(v);
        logmsg("set UN = %d (total %d)", v, un_total());
    }
    panel_fill_edits();
    panel_refresh();
}

static void panel_apply_max(int which)
{
    if (which == 0)      SetWindowTextW(g_ed_pts,  L"999");
    else if (which == 1) SetWindowTextW(g_ed_gold, L"999999");
    else                 SetWindowTextW(g_ed_un,   L"999999");
    panel_apply(which);
}

/* ---- item picker ---- */
/* ---- item descriptions ----------------------------------------------------
 * descriptor +0x0C0 indexes the itemDesc dictionary (0 = none).  Verified by
 * reading all 1604 descriptors: every value is < 344 (the en dictionary size),
 * the maximum is exactly 343, and the text matches the item ("onda arcana" ->
 * entry 2 "Creates a wave...", every arrow variant -> entry 182 "It is shot with
 * a bow...").  The dictionaries on disk are dumped as mod\dict\itemDesc_<lang>.tsv
 * (19 languages; en has 344 entries, ch 332, so a language can lack an index).
 */
#define DESC_MAX  400
#define DESC_POOL 52000
static wchar_t g_desc_pool[DESC_POOL];
static int     g_desc_off[DESC_MAX];      /* value = pool offset + 1, 0 = absent */

static void desc_load(void)
{
    char path[MAX_PATH], line[4096];
    FILE *f;
    int off = 0, have = 0;

    memset(g_desc_off, 0, sizeof g_desc_off);
    g_desc_pool[0] = 0;
    _snprintf(path, sizeof path, "%s\\mod\\dict\\itemDesc_%s.tsv", g_dir, g_lang_codes[g_lang]);
    f = fopen(path, "rb");
    if (!f) {
        _snprintf(path, sizeof path, "%s\\mod\\dict\\itemDesc_en.tsv", g_dir);
        f = fopen(path, "rb");
        if (!f) { logmsg("desc: no itemDesc_*.tsv under mod\\dict"); return; }
    }
    while (fgets(line, sizeof line, f)) {
        char *tab, *s, *d2;
        int idx, n;
        wchar_t w[1200];
        if ((unsigned char)line[0] == 0xEF && (unsigned char)line[1] == 0xBB) {
            memmove(line, line + 3, strlen(line + 3) + 1);   /* UTF-8 BOM */
        }
        idx = atoi(line);
        if (idx <= 0 || idx >= DESC_MAX) continue;
        tab = strchr(line, '\t');
        if (!tab) continue;
        s = tab + 1;
        d2 = s + strlen(s);
        while (d2 > s && (d2[-1] == '\n' || d2[-1] == '\r')) *--d2 = 0;
        for (d2 = s; *s; ) {                       /* literal backslash-n -> space */
            if (s[0] == '\\' && s[1] == 'n') { *d2++ = ' '; s += 2; }
            else *d2++ = *s++;
        }
        *d2 = 0;
        if (MultiByteToWideChar(CP_UTF8, 0, tab + 1, -1, w, 1200) <= 0) continue;
        n = (int)wcslen(w);
        if (off + n + 1 >= DESC_POOL) break;
        memcpy(g_desc_pool + off, w, (size_t)(n + 1) * sizeof(wchar_t));
        g_desc_off[idx] = off + 1;
        off += n + 1;
        have++;
    }
    fclose(f);
    logmsg("desc: %d entries loaded for lang=%s", have, g_lang_codes[g_lang]);
}

/* CJK text has no spaces, and a read-only EDIT only wraps at spaces, so a long
 * Chinese description stayed on one clipped line.  Wrap it ourselves with the
 * control's own font and width, then hand the edit text that already fits. */
static void desc_wrap_into(const wchar_t *src, wchar_t *dst, int cap, int maxw)
{
    HDC hdc;
    HFONT old;
    int n = 0, i = 0;

    if (!g_desc || maxw < 40) maxw = 40;
    hdc = GetDC(g_desc);
    old = (HFONT)SelectObject(hdc, g_ui_font);
    while (src[i] && n < cap - 3) {
        int start = i, last_space = -1, w = 0, e, k;
        while (src[i]) {
            SIZE sz;
            GetTextExtentPoint32W(hdc, src + i, 1, &sz);
            if (w + sz.cx > maxw && i > start) break;
            if (src[i] == L' ') last_space = i;
            w += sz.cx;
            i++;
        }
        if (src[i] && last_space > start) i = last_space + 1;
        e = i;
        while (e > start && src[e - 1] == L' ') e--;
        for (k = start; k < e && n < cap - 3; k++) dst[n++] = src[k];
        if (src[i]) { dst[n++] = (wchar_t)13; dst[n++] = (wchar_t)10; }
        while (src[i] == L' ') i++;
    }
    dst[n] = 0;
    SelectObject(hdc, old);
    ReleaseDC(g_desc, hdc);
}

static void desc_show(unsigned type)
{
    static wchar_t wrapped[4096];
    unsigned d;
    int idx = -1;
    const wchar_t *txt = L"";
    RECT cr;

    if (!g_desc) return;
    d = desc_of_type(type);
    if (d) idx = (int)*(unsigned *)(size_t)(d + 0x0C0u);
    if (d) {
        /* candidate stat fields, for calibrating against the game's own tooltip */
        logmsg("stats: type=%u +5C=%d +60=%d +64=%d +68=%d +6C=%d +74=%d"
               " +138=%d +140=%d +148=%d",
               type, *(int *)(size_t)(d + 0x5Cu), *(int *)(size_t)(d + 0x60u),
               *(int *)(size_t)(d + 0x64u), *(int *)(size_t)(d + 0x68u),
               *(int *)(size_t)(d + 0x6Cu), *(int *)(size_t)(d + 0x74u),
               *(int *)(size_t)(d + 0x138u), *(int *)(size_t)(d + 0x140u),
               *(int *)(size_t)(d + 0x148u));
    }
    if (idx > 0 && idx < DESC_MAX && g_desc_off[idx])
        txt = g_desc_pool + (g_desc_off[idx] - 1);
    else if (idx > 0)
        txt = L"(this language has no entry for this description)";
    else
        txt = L"(this item has no description in the game's dictionaries)";
    if (!txt[0]) { SetWindowTextW(g_desc, L""); return; }
    GetClientRect(g_desc, &cr);
    desc_wrap_into(txt, wrapped, 4096, (int)cr.right - 20);
    SetWindowTextW(g_desc, wrapped);
    {
        int lines = 1, k;
        for (k = 0; wrapped[k]; k++) if (wrapped[k] == (wchar_t)10) lines++;
        logmsg("desc: type=%u idx=%d width=%d len=%d lines=%d", type, idx,
               (int)cr.right - 20, (int)wcslen(wrapped), lines);
    }
}

static void items_show_sel(void)
{
    int r = g_lb_items ? (int)SendMessageW(g_lb_items, LB_GETCURSEL, 0, 0) : -1;
    wchar_t b[NAME_MAX + 96];
    if (r < 0 || r >= g_nsel) {
        SetWindowTextW(g_st_item, L"no selection");
        SetWindowTextW(g_st_dev, L"");
        if (g_desc) SetWindowTextW(g_desc, L"");
        EnableWindow(g_bt_bag, FALSE);
        EnableWindow(g_bt_grnd, FALSE);
        return;
    }
    {
        ItemRec *it = &g_items[g_sel[r]];
        const wchar_t *cat = (it->cat >= 0 && it->cat < CAT_COUNT) ? g_cat_names[it->cat] : L"?";
        wchar_t d[NAME_MAX + 48];
        wsprintfW(b, L"id %lu - %s", it->id, cat);
        SetWindowTextW(g_st_item, b);
        if (it->named) wsprintfW(d, L"dev name in the game files: %s", it->internal);
        else wsprintfW(d, L"no name in the dictionary - dev name: %s", it->internal);
        SetWindowTextW(g_st_dev, d);
        desc_show(it->id);
    }
    EnableWindow(g_bt_bag, TRUE);
    EnableWindow(g_bt_grnd, TRUE);
}

/* re-run the filter and refill the list (row r always maps to g_sel[r]) */
static void items_rebuild(void)
{
    wchar_t needle[NAME_MAX];
    wchar_t b[48];
    int i;
    if (!g_lb_items) return;
    GetWindowTextW(g_ed_find, needle, NAME_MAX);
    items_filter(needle);
    SendMessageW(g_lb_items, WM_SETREDRAW, FALSE, 0);
    SendMessageW(g_lb_items, LB_RESETCONTENT, 0, 0);
    for (i = 0; i < g_nsel; i++)
        SendMessageW(g_lb_items, LB_ADDSTRING, 0, (LPARAM)g_items[g_sel[i]].name);
    SendMessageW(g_lb_items, WM_SETREDRAW, TRUE, 0);
    wsprintfW(b, L"%d / %d", g_nsel, g_nitems);
    SetWindowTextW(g_st_count, b);
    if (g_nsel) SendMessageW(g_lb_items, LB_SETCURSEL, 0, 0);
    items_show_sel();
}

static void panel_pick_lang(void)
{
    int i = (int)SendMessageW(g_cm_lang, CB_GETCURSEL, 0, 0);
    char ini[MAX_PATH];
    if (i < 0 || i >= LANG_COUNT) return;
    g_lang = i;
    cfg_path(ini, sizeof ini);
    WritePrivateProfileStringA("items", "lang", g_lang_codes[g_lang], ini);
    items_apply_lang();
    items_log_sample();
    desc_load();
    items_rebuild();
    items_show_sel();
    logmsg("items: language -> %s", g_lang_codes[g_lang]);
}

static void panel_toggle_hiden(void)
{
    char ini[MAX_PATH], b[8];
    g_hidenamed = SendMessageW(g_ck_hiden, BM_GETCHECK, 0, 0) == BST_CHECKED;
    cfg_path(ini, sizeof ini);
    _snprintf(b, sizeof b, "%d", g_hidenamed);
    WritePrivateProfileStringA("items", "hidenamed", b, ini);
    items_rebuild();
    logmsg("items: hide rows without a dictionary name = %d", g_hidenamed);
}

static void give_from_panel(int to_ground)
{
    int r = (int)SendMessageW(g_lb_items, LB_GETCURSEL, 0, 0);
    int qty = edit_int(g_ed_qty, 1);
    wchar_t b[128];
    unsigned id;
    if (r < 0 || r >= g_nsel) { SetWindowTextW(g_st_item, L"select an item first"); return; }
    if (qty < 1) qty = 1;
    if (qty > 999) qty = 999;
    id = g_items[g_sel[r]].id;
    if (!give_request(id, qty, to_ground)) {
        SetWindowTextW(g_st_item, L"game window not found");
        return;
    }
    EnableWindow(g_bt_bag, FALSE);
    EnableWindow(g_bt_grnd, FALSE);
    wsprintfW(b, L"waiting for the game (id %lu x %d)...", id, qty);
    SetWindowTextW(g_st_item, b);
}

static void give_report(int res)
{
    const wchar_t *t;
    int ground = (int)g_give_ground;
    switch (res) {
    case  1: t = ground ? L"dropped next to the character" : L"added to the bag"; break;
    case  2: t = ground ? L"partially dropped" : L"partially added (bag full?)"; break;
    case  0: t = L"the game refused the item"; break;
    case -1: t = L"failed - load into a level first"; break;
    case -2: t = L"failed - container header looks wrong"; break;
    case -3: t = L"failed - no ground pile here (enter a level)"; break;
    default: t = L"no answer from the game (unpause it)"; break;
    }
    SetWindowTextW(g_st_item, t);
    EnableWindow(g_bt_bag, TRUE);
    EnableWindow(g_bt_grnd, TRUE);
    logmsg("give: reported %d", res);
}

/* The game keeps config.cfg and save\game_N.sav under Steam userdata:
 *   <steam>\userdata\<account>\233980\remote\
 * The fitRatio fix and the save-slot window must use that folder. */
static int remote_dir(char *out, size_t n)
{
    char steam[MAX_PATH] = {0};
    DWORD sz = sizeof steam;
    HKEY h;
    char pat[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE f;

    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Valve\\Steam", 0, KEY_QUERY_VALUE, &h)
        == ERROR_SUCCESS) {
        RegQueryValueExA(h, "SteamPath", NULL, NULL, (BYTE *)steam, &sz);
        RegCloseKey(h);
    }
    if (!steam[0]) {
        const char *cand[] = { "C:\\Program Files (x86)\\Steam",
                               "C:\\Program Files\\Steam", "D:\\Steam" };
        for (unsigned i = 0; i < sizeof cand / sizeof cand[0]; i++) {
            char t[MAX_PATH];
            _snprintf(t, sizeof t, "%s\\userdata", cand[i]);
            if (GetFileAttributesA(t) != INVALID_FILE_ATTRIBUTES) {
                _snprintf(steam, sizeof steam, "%s", cand[i]);
                break;
            }
        }
    }
    if (!steam[0]) return 0;
    _snprintf(pat, sizeof pat, "%s\\userdata\\*", steam);
    f = FindFirstFileA(pat, &fd);
    if (f == INVALID_HANDLE_VALUE) return 0;
    do {
        char cfg[MAX_PATH];
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        _snprintf(cfg, sizeof cfg, "%s\\userdata\\%s\\233980\\remote\\config.cfg",
                  steam, fd.cFileName);
        if (GetFileAttributesA(cfg) != INVALID_FILE_ATTRIBUTES) {
            _snprintf(out, n, "%s\\userdata\\%s\\233980\\remote", steam, fd.cFileName);
            FindClose(f);
            return 1;
        }
    } while (FindNextFileA(f, &fd));
    FindClose(f);
    return 0;
}

/* the game letterboxes by its own "fitRatio"; our aspect choice needs it off */
static void force_fitratio_zero(void)
{
    char path[MAX_PATH], tmp[MAX_PATH], *buf;
    FILE *f;
    long n;
    char remote[MAX_PATH];
    if (remote_dir(remote, sizeof remote))
        _snprintf(path, sizeof path, "%s\\config.cfg", remote);
    else
        _snprintf(path, sizeof path, "%s\\config.cfg", g_dir);
    f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > (1 << 20)) { fclose(f); return; }
    buf = (char *)malloc(n + 1);
    if (!buf) { fclose(f); return; }
    if (fread(buf, 1, n, f) != (size_t)n) { free(buf); fclose(f); return; }
    buf[n] = 0;
    fclose(f);
    {
        char *p = strstr(buf, "<fitRatio>");
        if (p) {
            char *q = p + 10;
            char *e = strstr(q, "</fitRatio>");
            if (e && !(e == q + 1 && q[0] == '0')) {
                size_t tail = strlen(e);
                memmove(q + 1, e, tail + 1);
                q[0] = '0';
                _snprintf(tmp, sizeof tmp, "%s\\config.cfg.new", g_dir);
                f = fopen(tmp, "wb");
                if (f) {
                    size_t len = strlen(buf);
                    if (fwrite(buf, 1, len, f) == len) {
                        fclose(f);
                        if (MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING))
                            logmsg("config.cfg: fitRatio -> 0");
                    } else {
                        fclose(f);
                    }
                }
            }
        }
    }
    free(buf);
}


/* keep the newest 30 timestamp folders */
static void panel_apply_speed(void)
{
    int sel = (int)SendMessageW(g_cm_speed, CB_GETCURSEL, 0, 0);
    char ini[MAX_PATH], b[16];
    if (sel < 0) sel = 0;
    g_speed_pct = (sel + 1) * 100;
    g_speed_mul = (float)g_speed_pct / 100.0f;
    cfg_path(ini, sizeof ini);
    _snprintf(b, sizeof b, "%ld", g_speed_pct);
    WritePrivateProfileStringA("cheats", "speed", b, ini);
    logmsg("speed set to %ld%% (live)", g_speed_pct);
}

static void panel_apply_atkspd(void)
{
    int sel = (int)SendMessageW(g_cm_atk, CB_GETCURSEL, 0, 0);
    char ini[MAX_PATH], b[16];
    if (sel < 0) sel = 0;
    g_atkspd = (sel + 1) * 10;
    g_atk_div = (float)g_atkspd / 10.0f;
    cfg_path(ini, sizeof ini);
    _snprintf(b, sizeof b, "%ld", g_atkspd);
    WritePrivateProfileStringA("cheats", "atkspd", b, ini);
    logmsg("attack speed set to %ld%% (live)", g_atkspd);
}

static void panel_apply_scale(void)
{
    int asp = (int)SendMessageW(g_cm_aspect, CB_GETCURSEL, 0, 0);
    int sel = (int)SendMessageW(g_cm_scale, CB_GETCURSEL, 0, 0);
    char ini[MAX_PATH], b[16];
    int s10;
    if (asp < 0) asp = 0;
    if (sel < 0) sel = 0;
    s10 = 10 + 2 * sel;
    if (s10 > max_scale10_for(asp)) s10 = max_scale10_for(asp);
    cfg_path(ini, sizeof ini);
    _snprintf(b, sizeof b, "%d", asp);
    WritePrivateProfileStringA("video", "aspect", b, ini);
    _snprintf(b, sizeof b, "%d", s10);
    WritePrivateProfileStringA("video", "scale", b, ini);
    WritePrivateProfileStringA("video", "resize", "1", ini);
    force_fitratio_zero();
    logmsg("display: aspect=%d scale=%d.%dx (applies next launch)", asp, s10 / 10, s10 % 10);
    MessageBoxW(g_hwnd,
                L"Saved.\n\nThe window size is applied the next time the game starts.",
                MOD_NAME, MB_OK | MB_ICONINFORMATION);
}

static void panel_show(void)
{
    RECT wa;
    RECT wr;
    HMONITOR mon;
    MONITORINFO mi;
    HWND gw = find_game_window();
    int x, y, ww, wh;

    mon = MonitorFromWindow(gw ? gw : GetForegroundWindow(), MONITOR_DEFAULTTONEAREST);
    mi.cbSize = sizeof mi;
    if (!GetMonitorInfoW(mon, &mi)) {
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
        mi.rcWork = wa;
    }
    GetWindowRect(g_hwnd, &wr);
    ww = wr.right - wr.left;
    wh = wr.bottom - wr.top;
    x = mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - ww) / 2;
    y = mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - wh) / 2;
    panel_sync_checks();
    panel_fill_edits();
    panel_display_state();
    panel_refresh();
    SetWindowPos(g_hwnd, HWND_TOPMOST, x, y, 0, 0,
                 SWP_NOSIZE | SWP_SHOWWINDOW | SWP_NOACTIVATE);
    g_panel_visible = 1;
}

static void panel_hide(void)
{
    ShowWindow(g_hwnd, SW_HIDE);
    g_panel_visible = 0;
}

/* ---------------- save slots window (modal to the panel) -------------------
 * One file per slot: <remote>\save\game_N.sav.  Every slot 0..15 always has a
 * row; the row text is refreshed once a second (only when something changed, so
 * the selection does not flicker).  The window is owner-modal: the panel is
 * disabled while it is open, and it is centered on the panel.
 * All work is plain file access - the running game only reads these files when
 * its save/load menu opens.
 */
#define SAVE_MAX_SLOTS 16
#define SV_ID_REFRESH  501
#define SV_ID_COPY     502
#define SV_ID_DELETE   503
#define SV_ID_FOLDER   504
#define SV_ID_CLOSE    505
#define SV_ID_LOAD     506

static HWND g_saves, g_lb_slot, g_cm_target, g_saves_st;
static wchar_t g_saves_cls[64];   /* per-core class name: a reused name would keep the OLD wndproc (old cores stay mapped) */
static char g_save_dir[MAX_PATH];
static int g_saves_mp;            /* 1 = the window manages game_mp_N.sav */
static unsigned g_slot_sig[SAVE_MAX_SLOTS];   /* used<<20 | size, plus mtime mix */

/* Single-player and multiplayer keep independent save files: the game picks
 * save\game_N.sav when 0x672334 is 0 and save\game_mp_N.sav otherwise (0x43C8DB). */
static int saves_slot_exists(int slot, char *out, size_t n)
{
    _snprintf(out, n, g_saves_mp ? "%s\\save\\game_mp_%d.sav" : "%s\\save\\game_%d.sav",
              g_save_dir, slot);
    return GetFileAttributesA(out) != INVALID_FILE_ATTRIBUTES;
}

static void saves_row_text(int slot, wchar_t *w, int n)
{
    char path[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    SYSTEMTIME st;

    if (!g_save_dir[0] || !saves_slot_exists(slot, path, sizeof path)) {
        wsprintfW(w, L"slot %2d    empty", slot);
        return;
    }
    h = FindFirstFileA(path, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        wsprintfW(w, L"slot %2d    empty", slot);
        return;
    }
    FindClose(h);
    memset(&st, 0, sizeof st);
    FileTimeToSystemTime(&fd.ftLastWriteTime, &st);
    {
        /* 0x675174 is shared by both namespaces, so only call it "(current)"
         * in the window that matches the mode the game is actually in. */
        int cur = !IsBadReadPtr((void *)0x00675174u, 4) &&
                  slot == *(int *)0x00675174u &&
                  (g_saves_mp ? game_is_mp() : !game_is_mp());
        wsprintfW(w, L"slot %2d    %04d-%02d-%02d %02d:%02d    %6u B%s",
                  slot, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
                  (unsigned)fd.nFileSizeLow, cur ? L"    (current)" : L"");
    }
    (void)n;
}

/* rebuild the rows only when the on-disk state changed */
static void saves_refresh(int force)
{
    unsigned sig[SAVE_MAX_SLOTS];
    char path[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    int i, sel, changed = 0, cur;
    static int last_mode = -1;

    if (!g_lb_slot) return;
    if (!g_save_dir[0]) {
        if (!remote_dir(g_save_dir, sizeof g_save_dir)) {
            SetWindowTextW(g_saves_st, L"Steam save folder not found");
            return;
        }
    }
    cur = *(int *)0x00675174u;
    {
        /* the "(current)" marker depends on the mode as well (0x675174 is
         * shared by both namespaces), so refresh when SP <-> MP changes */
        int mode = game_is_mp() ? 1 : 0;
        if (mode != last_mode) { last_mode = mode; changed = 1; }
    }
    for (i = 0; i < SAVE_MAX_SLOTS; i++) {
        sig[i] = 0;
        if (saves_slot_exists(i, path, sizeof path)) {
            h = FindFirstFileA(path, &fd);
            if (h != INVALID_HANDLE_VALUE) {
                FindClose(h);
                sig[i] = (0x80000000u | (fd.nFileSizeLow & 0x7FFFFFu)) ^
                         (fd.ftLastWriteTime.dwLowDateTime * 2654435761u) ^
                         (unsigned)cur;
            }
        }
        if (sig[i] != g_slot_sig[i]) changed = 1;
    }
    if (!force && !changed) return;
    memcpy(g_slot_sig, sig, sizeof sig);

    sel = (int)SendMessageW(g_lb_slot, LB_GETCURSEL, 0, 0);
    SendMessageW(g_lb_slot, LB_RESETCONTENT, 0, 0);
    for (i = 0; i < SAVE_MAX_SLOTS; i++) {
        wchar_t w[160];
        saves_row_text(i, w, 160);
        SendMessageW(g_lb_slot, LB_ADDSTRING, 0, (LPARAM)w);
    }
    if (sel >= 0) SendMessageW(g_lb_slot, LB_SETCURSEL, sel, 0);
    {
        char pat[MAX_PATH];
        wchar_t w[200];
        int n = 0;
        _snprintf(pat, sizeof pat, g_saves_mp ? "%s\\save\\game_mp_*.sav"
                                              : "%s\\save\\game_*.sav", g_save_dir);
        h = FindFirstFileA(pat, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                int slot = -1;
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                if (sscanf(fd.cFileName, g_saves_mp ? "game_mp_%d.sav" : "game_%d.sav",
                           &slot) == 1 && slot >= 0) n++;
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
        wsprintfW(w, L"%d of %d slots used", n, SAVE_MAX_SLOTS);
        SetWindowTextW(g_saves_st, w);
    }
}

static int saves_selected_slot(void)
{
    int sel;
    if (!g_save_dir[0]) { SetWindowTextW(g_saves_st, L"no save folder"); return -1; }
    sel = (int)SendMessageW(g_lb_slot, LB_GETCURSEL, 0, 0);
    if (sel < 0) { SetWindowTextW(g_saves_st, L"select a slot first"); return -1; }
    return sel;                       /* row index == slot number by construction */
}

static void saves_copy(void)
{
    wchar_t msg[320];
    char src[MAX_PATH], dst[MAX_PATH];
    int cur, target;
    int slot = saves_selected_slot();

    if (slot < 0) return;
    target = (int)SendMessageW(g_cm_target, CB_GETCURSEL, 0, 0);
    if (target < 0) { SetWindowTextW(g_saves_st, L"choose the target slot"); return; }
    if (target == slot) { SetWindowTextW(g_saves_st, L"source and target are the same slot"); return; }
    if (!saves_slot_exists(slot, src, sizeof src)) {
        SetWindowTextW(g_saves_st, L"the selected slot is empty - nothing to copy");
        return;
    }
    if (saves_slot_exists(target, dst, sizeof dst)) {
        wsprintfW(msg, L"Slot %d already has a save file.\r\nOverwrite it with slot %d?",
                  target, slot);
        if (MessageBoxW(g_saves, msg, L"Copy save slot",
                        MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2) != IDOK) {
            SetWindowTextW(g_saves_st, L"copy cancelled");
            return;
        }
    }
    cur = (g_saves_mp ? game_is_mp() : !game_is_mp())
          ? *(int *)0x00675174u : -1;
    if (target == cur) {
        wsprintfW(msg, L"Slot %d is the slot the game is using right now.\r\n"
                       L"It may be overwritten on the next save.\r\nCopy anyway?", target);
        if (MessageBoxW(g_saves, msg, L"Copy save slot",
                        MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2) != IDOK) {
            SetWindowTextW(g_saves_st, L"copy cancelled");
            return;
        }
    }
    if (!CopyFileA(src, dst, FALSE)) {
        DWORD e = GetLastError();
        wsprintfW(msg, L"copy failed (error %lu)", e);
        SetWindowTextW(g_saves_st, msg);
        logmsg("saves: copy %s -> %s failed (%lu)", src, dst, e);
        return;
    }
    logmsg("saves: copied game_%d.sav -> game_%d.sav", slot, target);
    saves_refresh(1);
    wsprintfW(msg, L"slot %d copied to slot %d - reopen the game's load menu", slot, target);
    SetWindowTextW(g_saves_st, msg);
}

/* delete through the shell so the file lands in the recycle bin */
typedef struct {
    HWND hwnd;
    UINT wFunc;
    const char *pFrom;
    const char *pTo;
    WORD fFlags;
    BOOL fAnyOperationsAborted;
    void *hNameMappings;
    const char *lpszProgressTitle;
} shfileop_t;

static int shell_delete(const char *path)
{
    typedef int (WINAPI *sfo_t)(const shfileop_t *);
    HMODULE sh = LoadLibraryA("shell32.dll");
    char from[MAX_PATH * 2];
    shfileop_t op;
    sfo_t sfo;
    int n;
    if (!sh) return 0;
    sfo = (sfo_t)(size_t)GetProcAddress(sh, "SHFileOperationA");
    if (!sfo) return 0;
    n = (int)strlen(path);
    if (n < 3 || n + 2 > (int)sizeof from) return 0;
    memcpy(from, path, n);
    from[n] = 0;
    from[n + 1] = 0;
    memset(&op, 0, sizeof op);
    op.wFunc = 3;                                  /* FO_DELETE */
    op.pFrom = from;
    op.fFlags = 0x0040 | 0x0010 | 0x0004 | 0x0400; /* ALLOWUNDO NOCONFIRMATION SILENT NOERRORUI */
    return sfo(&op) == 0;
}

static void saves_delete(void)
{
    wchar_t msg[320];
    char path[MAX_PATH];
    int cur;
    int slot = saves_selected_slot();

    if (slot < 0) return;
    if (!saves_slot_exists(slot, path, sizeof path)) {
        SetWindowTextW(g_saves_st, L"that slot is already empty");
        return;
    }
    cur = (g_saves_mp ? game_is_mp() : !game_is_mp())
          ? *(int *)0x00675174u : -1;
    wsprintfW(msg, L"Delete the save in slot %d?\r\n\r\nThe file goes to the recycle bin, "
                   L"so it can be restored.%s", slot,
              slot == cur ? L"\r\n\r\nThis is the slot the game is using right now." : L"");
    if (MessageBoxW(g_saves, msg, L"Delete save slot",
                    MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2) != IDOK) {
        logmsg("saves: delete slot %d cancelled", slot);
        SetWindowTextW(g_saves_st, L"delete cancelled");
        return;
    }
    if (!shell_delete(path)) {
        SetWindowTextW(g_saves_st, L"delete failed");
        logmsg("saves: delete %s failed", path);
        return;
    }
    logmsg("saves: deleted game_%d.sav (recycle bin)", slot);
    saves_refresh(1);
    wsprintfW(msg, L"slot %d deleted (recycle bin)", slot);
    SetWindowTextW(g_saves_st, msg);
}

static void saves_open_folder(void)
{
    typedef HINSTANCE (WINAPI *se_t)(HWND, LPCSTR, LPCSTR, LPCSTR, LPCSTR, int);
    HMODULE sh = LoadLibraryA("shell32.dll");
    if (sh) {
        se_t se = (se_t)(size_t)GetProcAddress(sh, "ShellExecuteA");
        if (se && g_save_dir[0])
            se(NULL, "open", g_save_dir, NULL, NULL, SW_SHOWNORMAL);
    }
}

/* Load a slot without leaving the game.  Single-player window only: the MP
 * saves window deliberately has no Load button, because the game loads
 * game_mp_N.sav itself from its multiplayer menu.
 * The game has its own queued-load path: its main loop checks 0x00639894 and,
 * when set, calls the save loader 0x0043C940(this=0x00674EF8, slot, 0, 0, 0)
 * with the slot from 0x00675174, then clears the flag.  We only fill in those
 * two globals on the game thread (WM_APP+18); the loader itself runs in the
 * game's own context, which is why this is safe to use mid-game.  Verified
 * 2026-10-23: setting slot 9 + the flag loaded game_9.sav within one frame. */
static void saves_close(void);

static void saves_load(void)
{
    wchar_t msg[420];
    char path[MAX_PATH];
    int slot = saves_selected_slot();
    HWND gw;

    if (slot < 0) return;
    if (!saves_slot_exists(slot, path, sizeof path)) {
        SetWindowTextW(g_saves_st, L"that slot is empty");
        return;
    }
    /* Safety net only - the MP window does not create a Load button.  The
     * game's own multiplayer menu is the only supported way to load
     * game_mp_N.sav, so never drive the loader from the MP save manager. */
    if (g_saves_mp) {
        MessageBoxW(g_saves,
                    L"This window only manages multiplayer save files.\r\n\r\n"
                    L"Load a multiplayer save from the game's own multiplayer\r\n"
                    L"menu - the mod does not drive the loader for those slots.",
                    L"Load multiplayer save", MB_OK | MB_ICONINFORMATION);
        SetWindowTextW(g_saves_st, L"load multiplayer saves from the game's menu");
        logmsg("saves: refused load from the mp window");
        return;
    }
    wsprintfW(msg,
              L"Load slot %d now?\r\n\r\n"
              L"The game's own loader runs on the next frame.\r\n"
              L"Unsaved progress is lost and the game returns to the\r\n"
              L"saved room.\r\n\r\n"
              L"Force save first if you want to keep the current state.", slot);
    if (MessageBoxW(g_saves, msg, L"Load save slot",
                    MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2) != IDOK) {
        logmsg("saves: load slot %d cancelled", slot);
        SetWindowTextW(g_saves_st, L"load cancelled");
        return;
    }
    gw = find_game_window();
    if (!gw) {
        SetWindowTextW(g_saves_st, L"game window not found");
        return;
    }
    g_load_slot = slot;
    PostMessageW(gw, WM_APP + 18, 0, 0);
    logmsg("saves: load slot %d requested", slot);
    hint_flash(L"loading the selected save ...");
    saves_close();
}

static BOOL CALLBACK saves_font_cb(HWND c, LPARAM p)
{
    SendMessageW(c, WM_SETFONT, (WPARAM)p, TRUE);
    return TRUE;
}

static void saves_close(void)
{
    if (g_saves && IsWindow(g_saves)) DestroyWindow(g_saves);
}

static LRESULT CALLBACK saves_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_COMMAND:
        switch (LOWORD(w)) {
        case SV_ID_REFRESH: saves_refresh(1); return 0;
        case SV_ID_COPY:    saves_copy(); return 0;
        case SV_ID_LOAD:    if (!g_saves_mp) saves_load(); return 0;
        case SV_ID_DELETE:  saves_delete(); return 0;
        case SV_ID_FOLDER:  saves_open_folder(); return 0;
        case SV_ID_CLOSE:   saves_close(); return 0;
        }
        return 0;
    case WM_TIMER:
        if (w == 1) saves_refresh(0);
        return 0;
    case WM_KEYDOWN:
        if (w == VK_ESCAPE) { saves_close(); return 0; }
        break;
    case WM_CLOSE:
        saves_close();
        return 0;
    case WM_DESTROY:
        KillTimer(h, 1);
        g_saves = NULL;
        if (g_hwnd) {
            EnableWindow(g_hwnd, TRUE);
            SetActiveWindow(g_hwnd);
        }
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static void saves_show(int mp)
{
    WNDCLASSEXW wc;
    RECT r = { 0, 0, 560, 584 }, pr;
    HINSTANCE hi = GetModuleHandleA(NULL);

    if (g_saves && IsWindow(g_saves) && g_saves_mp != mp) saves_close();
    g_saves_mp = mp ? 1 : 0;
    if (g_saves && IsWindow(g_saves)) {
        ShowWindow(g_saves, SW_SHOW);
        SetForegroundWindow(g_saves);
        saves_refresh(1);
        return;
    }
    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = saves_proc;
    wc.hInstance = hi;
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wsprintfW(g_saves_cls, L"UnepicSaves%08X", (unsigned)(size_t)&saves_proc);
    wc.lpszClassName = g_saves_cls;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        logmsg("saves: RegisterClassExW failed: %lu", GetLastError());
        hint_flash(L"could not register the saves window class");
        return;
    }
    AdjustWindowRect(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE);
    g_saves = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, g_saves_cls,
                              g_saves_mp ? L"Unepic Better Mod - multiplayer save slots"
                                         : L"Unepic Better Mod - save slots",
                              WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                              CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
                              g_hwnd, NULL, hi, NULL);
    if (!g_saves) { hint_flash(L"could not create the saves window"); return; }

    CreateWindowExW(0, L"BUTTON",
                    g_saves_mp ? L"Multiplayer saves (game_mp_N.sav; the game reads them from the multiplayer menu)"
                               : L"Save slots (one file per slot; the game reads them at its load menu)",
                    WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 12, 4, 536, 392, g_saves, NULL, hi, NULL);
    g_lb_slot = CreateWindowExW(0, L"LISTBOX", L"", WS_CHILD | WS_VISIBLE | WS_BORDER |
                                WS_VSCROLL | LBS_NOTIFY, 24, 26, 512, 356, g_saves, NULL, hi, NULL);
    CreateWindowExW(0, L"BUTTON", L"Actions", WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                    12, 400, 536, 112, g_saves, NULL, hi, NULL);
    CreateWindowExW(0, L"STATIC", L"Copy selected to", WS_CHILD | WS_VISIBLE,
                    24, 422, 120, 18, g_saves, NULL, hi, NULL);
    g_cm_target = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE |
                                  CBS_DROPDOWNLIST | WS_VSCROLL, 148, 418, 90, 300,
                                  g_saves, NULL, hi, NULL);
    CreateWindowExW(0, L"BUTTON", L"Copy", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                    244, 416, 74, 28, g_saves, (HMENU)SV_ID_COPY, hi, NULL);
    CreateWindowExW(0, L"BUTTON", L"Delete selected", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                    324, 416, 116, 28, g_saves, (HMENU)SV_ID_DELETE, hi, NULL);
    CreateWindowExW(0, L"BUTTON", L"Open folder", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                    446, 416, 96, 28, g_saves, (HMENU)SV_ID_FOLDER, hi, NULL);
    if (g_saves_mp) {
        /* Multiplayer slots are loaded by the game itself from its own
         * multiplayer menu; offering a load button here only invites the
         * "which save did it actually read" problem, so there is none. */
        CreateWindowExW(0, L"STATIC",
                        L"Load multiplayer saves from the game's own multiplayer menu.\r\n"
                        L"This window only manages the files.",
                        WS_CHILD | WS_VISIBLE | SS_LEFT, 24, 452, 512, 32,
                        g_saves, NULL, hi, NULL);
    } else {
        CreateWindowExW(0, L"BUTTON", L"Load selected", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                        24, 452, 140, 28, g_saves, (HMENU)SV_ID_LOAD, hi, NULL);
        CreateWindowExW(0, L"STATIC", L"uses the game's loader; unsaved progress is lost",
                        WS_CHILD | WS_VISIBLE | SS_LEFT, 176, 457, 368, 18,
                        g_saves, NULL, hi, NULL);
    }
    g_saves_st = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT,
                                 14, 520, 532, 18, g_saves, NULL, hi, NULL);
    CreateWindowExW(0, L"BUTTON", L"Refresh", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                    12, 544, 100, 28, g_saves, (HMENU)SV_ID_REFRESH, hi, NULL);
    CreateWindowExW(0, L"BUTTON", L"Close", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                    448, 544, 100, 28, g_saves, (HMENU)SV_ID_CLOSE, hi, NULL);
    EnumChildWindows(g_saves, saves_font_cb, (LPARAM)g_ui_font);

    {
        int i;
        wchar_t w[48];
        for (i = 0; i < SAVE_MAX_SLOTS; i++) {
            wsprintfW(w, L"slot %d", i);
            SendMessageW(g_cm_target, CB_ADDSTRING, 0, (LPARAM)w);
        }
        SendMessageW(g_cm_target, CB_SETCURSEL, 0, 0);
    }
    memset(g_slot_sig, 0xFF, sizeof g_slot_sig);   /* force the first build */
    saves_refresh(1);

    if (g_hwnd) {
        GetWindowRect(g_hwnd, &pr);
        GetWindowRect(g_saves, &r);
        SetWindowPos(g_saves, HWND_TOP,
                     pr.left + ((pr.right - pr.left) - (r.right - r.left)) / 2,
                     pr.top + ((pr.bottom - pr.top) - (r.bottom - r.top)) / 2,
                     0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
        EnableWindow(g_hwnd, FALSE);              /* owner-modal */
    }
    ShowWindow(g_saves, SW_SHOW);
    SetForegroundWindow(g_saves);
    SetTimer(g_saves, 1, 1000, NULL);
}

static int map_reveal_apply(void);
static int mp_unlock_all_apply(void);
static int mp_unlock_state_ok(void);
static int light_request(int all);
static void buffs_show(void);

static LRESULT CALLBACK panel_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_CTLCOLORSTATIC: {   /* the bottom line is blue, centred, on the dialog face */
        HDC dc = (HDC)w;
        if ((HWND)l == g_hint) {
            SetTextColor(dc, RGB(0, 102, 204));
            SetBkMode(dc, TRANSPARENT);
            return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
        }
        break;
    }
    case WM_COMMAND: {
        int id = LOWORD(w);
        if (id == ID_BT_PTS)   { panel_apply(0); return 0; }
        if (id == ID_BT_GOLD)  { panel_apply(1); return 0; }
        if (id == ID_BT_UN)    { panel_apply(2); return 0; }
        if (id == ID_BT_PTS_MAX)  { panel_apply_max(0); return 0; }
        if (id == ID_BT_GOLD_MAX) { panel_apply_max(1); return 0; }
        if (id == ID_BT_UN_MAX)   { panel_apply_max(2); return 0; }
        if (id == ID_BT_SCALE) { panel_apply_scale(); return 0; }
        if (id == ID_BT_FORCE) { force_save(); return 0; }
        if (id == ID_BT_RETO)  { retos_button(); return 0; }
        if (id == ID_BT_ALLACH) { ach_button(); return 0; }
        if (id == ID_BT_SAVES)  { saves_show(0); return 0; }
        if (id == ID_BT_SAVES_MP) { saves_show(1); return 0; }
        if (id == ID_CM_ASPECT && HIWORD(w) == CBN_SELCHANGE) { scale_items(); return 0; }
        if (id == ID_CM_SPEED && HIWORD(w) == CBN_SELCHANGE) { panel_apply_speed(); return 0; }
        if (id == ID_CM_ATK && HIWORD(w) == CBN_SELCHANGE) { panel_apply_atkspd(); return 0; }
        if (id == ID_CM_LANG && HIWORD(w) == CBN_SELCHANGE) { panel_pick_lang(); return 0; }
        if (id == ID_CM_CAT && HIWORD(w) == CBN_SELCHANGE) {
            int c = (int)SendMessageW(g_cm_cat, CB_GETCURSEL, 0, 0) - 1;
            g_cat = c;
            items_rebuild();
            return 0;
        }
        if (id == ID_ED_FIND && HIWORD(w) == EN_CHANGE) { items_rebuild(); return 0; }
        if (id == ID_LB_ITEMS && HIWORD(w) == LBN_SELCHANGE) { items_show_sel(); return 0; }
        if (id == ID_BT_TOBAG)  { give_from_panel(0); return 0; }
        if (id == ID_BT_TOGRND) { give_from_panel(1); return 0; }
        if (id == ID_CB_HIDEN && HIWORD(w) == BN_CLICKED) { panel_toggle_hiden(); return 0; }
        if (id == ID_BT_REVEAL && HIWORD(w) == BN_CLICKED) {
            if (MessageBoxW(g_hwnd,
                            L"Mark every room of the current map as explored?\n\n"
                            L"The whole map becomes visible and every room can be\n"
                            L"selected and right-click warped to.  If you save the\n"
                            L"game afterwards, the explored state is stored in that\n"
                            L"save.",
                            MOD_NAME, MB_OKCANCEL | MB_ICONQUESTION) == IDOK) {
                logmsg("panel: reveal map confirmed");
                map_reveal_apply();
            }
            return 0;
        }
        if (id == ID_BT_MPUNLOCK && HIWORD(w) == BN_CLICKED) {
            if (MessageBoxW(g_hwnd,
                            L"Open every co-op map in the multiplayer list?\n\n"
                            L"Each map normally needs the previous dungeon cleared.\n"
                            L"This is SESSION ONLY: it opens the list now but is not\n"
                            L"saved. Loading a save rebuilds the list from real\n"
                            L"progress, so maps you have not completed are locked\n"
                            L"again.\n\n"
                            L"Entering and completing a map records that completion\n"
                            L"normally, so that map stays unlocked afterwards.",
                            L"Unlock all MP maps", MB_OKCANCEL | MB_ICONQUESTION) == IDOK) {
                logmsg("panel: unlock mp maps confirmed");
                if (mp_unlock_all_apply() > 0)
                    hint_flash(L"maps opened for this session - complete a map to keep it");
            }
            return 0;
        }
        if (id == ID_BT_LIGHTS && HIWORD(w) == BN_CLICKED) {
            /* the real work runs on the game thread (WM_APP+20) and only once
             * the game is running in a room again - the panel takes the game
             * out of the foreground, and the native ignition effect needs the
             * room's own effect list.  So this only queues the request. */
            if (!light_request(0))
                MessageBoxW(g_hwnd,
                            L"Could not read this room's light table.\n\n"
                            L"The table only exists while a room is loaded.\n"
                            L"Nothing was changed.",
                            MOD_NAME, MB_OK | MB_ICONWARNING);
            return 0;
        }
        if (id == ID_BT_LIGHTS_ALL && HIWORD(w) == BN_CLICKED) {
            if (MessageBoxW(g_hwnd,
                            L"Light every room of the current zone?\n\n"
                            L"This turns on every torch, brazier and lamp in all\n"
                            L"loaded room tables of this zone - not just this room.\n\n"
                            L"Rooms whose puzzle is \"light every torch\" are opened\n"
                            L"without their script running, so a door or event may\n"
                            L"not fire.  It is local memory only (no achievements are\n"
                            L"touched), it is not undone by this button, and in\n"
                            L"multiplayer the other player will not see it.",
                            L"Light all rooms", MB_OKCANCEL | MB_ICONWARNING) == IDOK) {
                logmsg("panel: light all rooms confirmed");
                if (!light_request(1))
                    MessageBoxW(g_hwnd,
                                L"Could not read the room table array.\n\n"
                                L"It only exists while a zone is loaded.\n"
                                L"Nothing was changed.",
                                MOD_NAME, MB_OK | MB_ICONWARNING);
            }
            return 0;
        }
        if (id == ID_BT_BUFFS) { buffs_show(); return 0; }
        if (HIWORD(w) == BN_CLICKED) {
            g_god    = SendMessageW(g_cb_god,  BM_GETCHECK, 0, 0) == BST_CHECKED;
            g_maxxp  = SendMessageW(g_cb_xp,   BM_GETCHECK, 0, 0) == BST_CHECKED;
            g_nocons = SendMessageW(g_cb_item, BM_GETCHECK, 0, 0) == BST_CHECKED;
            g_airjump = SendMessageW(g_cb_jump, BM_GETCHECK, 0, 0) == BST_CHECKED;
            g_teleport = SendMessageW(g_cb_tp, BM_GETCHECK, 0, 0) == BST_CHECKED;
            cfg_save_cheats();
            logmsg("panel: god=%ld maxxp=%ld nocons=%ld teleport=%ld (saved)",
                   g_god, g_maxxp, g_nocons, g_teleport);
        }
        return 0;
    }
    case WM_APP + 9:
        if (w) {
            wchar_t name[64], msg[160];
            int slot = IsBadReadPtr((void *)0x00675174u, 4) ? -1 : *(int *)0x00675174u;
            wsprintfW(name, game_is_mp() ? L"game_mp_%d.sav" : L"game_%d.sav", slot);
            wsprintfW(msg, L"Saved to %s.", name);
            MessageBoxW(g_hwnd, msg, MOD_NAME, MB_OK | MB_ICONINFORMATION);
        } else {
            MessageBoxW(g_hwnd, L"Save FAILED - see unepic_mod.log.",
                        MOD_NAME, MB_OK | MB_ICONWARNING);
        }
        return 0;
    case WM_APP + 7:
        if (g_panel_visible) panel_hide(); else panel_show();
        logmsg("panel %s", g_panel_visible ? "shown" : "hidden");
        return 0;
    case WM_CLOSE:
        panel_hide();
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static int panel_create(void)
{
    WNDCLASSW wc;
    int asp, i;

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = panel_proc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = L"UnepicPanel";
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        logmsg("RegisterClassW failed: %lu", GetLastError());
        return 0;
    }
    g_ui_font = CreateFontW(-16, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                            DEFAULT_PITCH | FF_DONTCARE, L"Yu Gothic UI");
    g_ui_bold = CreateFontW(-16, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET,
                            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                            DEFAULT_PITCH | FF_DONTCARE, L"Yu Gothic UI");
    {
        RECT rc = { 0, 0, PANEL_W, PANEL_H };
        DWORD st = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
        AdjustWindowRectEx(&rc, st, FALSE, WS_EX_TOPMOST | WS_EX_TOOLWINDOW);
        g_hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"UnepicPanel",
                                 MOD_TITLE, st,
                                 CW_USEDEFAULT, CW_USEDEFAULT,
                                 rc.right - rc.left, rc.bottom - rc.top,
                                 NULL, NULL, wc.hInstance, NULL);
    }
    if (!g_hwnd) { logmsg("CreateWindowExW failed: %lu", GetLastError()); return 0; }

    /* display */
    mk_ctl(L"BUTTON", L"Display", BS_GROUPBOX, 12, 4, PANEL_W - 24, 100, 0, g_ui_bold);
    mk_ctl(L"STATIC", L"Aspect", SS_LEFT, 26, 26, 62, 20, 0, g_ui_font);
    g_cm_aspect = mk_ctl(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL,
                         92, 22, 120, 180, ID_CM_ASPECT, g_ui_font);
    SendMessageW(g_cm_aspect, CB_ADDSTRING, 0, (LPARAM)L"4:3");
    SendMessageW(g_cm_aspect, CB_ADDSTRING, 0, (LPARAM)L"16:9");
    SendMessageW(g_cm_aspect, CB_ADDSTRING, 0, (LPARAM)L"16:10");
    mk_ctl(L"STATIC", L"Size", SS_LEFT, 236, 26, 40, 20, 0, g_ui_font);
    g_cm_scale = mk_ctl(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL,
                        282, 22, 150, 220, ID_CM_SCALE, g_ui_font);
    g_bt_scale = mk_ctl(L"BUTTON", L"Apply", BS_PUSHBUTTON, 26, 58, 100, 28, ID_BT_SCALE, g_ui_font);
    g_st_scale = mk_ctl(L"STATIC", L"", SS_LEFT, 138, 62, 466, 20, 0, g_ui_font);
    asp = 0;

    /* cheats */
    mk_ctl(L"BUTTON", L"Cheats", BS_GROUPBOX, 12, 110, PANEL_W - 24, 244, 0, g_ui_bold);
    g_cb_god  = mk_ctl(L"BUTTON", L"God mode", BS_AUTOCHECKBOX, 26, 134, 150, 24, ID_CB_GOD, g_ui_font);
    g_cb_xp   = mk_ctl(L"BUTTON", L"Max XP", BS_AUTOCHECKBOX, 200, 134, 130, 24, ID_CB_XP, g_ui_font);
    mk_ctl(L"BUTTON", L"Buffs...", BS_PUSHBUTTON,
           350, 134, 120, 24, ID_BT_BUFFS, g_ui_font);
    g_cb_item = mk_ctl(L"BUTTON", L"Infinite items", BS_AUTOCHECKBOX, 26, 164, 150, 24, ID_CB_ITEM, g_ui_font);
    g_cb_jump = mk_ctl(L"BUTTON", L"Infinite jump", BS_AUTOCHECKBOX, 200, 164, 150, 24, ID_CB_JUMP, g_ui_font);
    g_cb_tp   = mk_ctl(L"BUTTON", L"Right-click teleport", BS_AUTOCHECKBOX, 380, 164, 200, 24, ID_CB_TP, g_ui_font);
    mk_ctl(L"STATIC", L"Move speed", SS_LEFT, 26, 200, 96, 20, 0, g_ui_font);
    g_cm_speed = mk_ctl(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL,
                        126, 196, 90, 180, ID_CM_SPEED, g_ui_font);
    mk_ctl(L"STATIC", L"Atk speed", SS_LEFT, 246, 200, 84, 20, 0, g_ui_font);
    g_cm_atk = mk_ctl(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL,
                      334, 196, 90, 180, ID_CM_ATK, g_ui_font);
    {
        wchar_t sp[8];
        for (i = 1; i <= 10; i++) {
            wsprintfW(sp, L"%dx", i);
            SendMessageW(g_cm_speed, CB_ADDSTRING, 0, (LPARAM)sp);
            SendMessageW(g_cm_atk, CB_ADDSTRING, 0, (LPARAM)sp);
        }
    }
    mk_ctl(L"BUTTON", L"Light this room", BS_PUSHBUTTON,
           26, 232, 220, 28, ID_BT_LIGHTS, g_ui_font);
    mk_ctl(L"BUTTON", L"Light all rooms", BS_PUSHBUTTON,
           258, 232, 220, 28, ID_BT_LIGHTS_ALL, g_ui_font);
    mk_ctl(L"BUTTON", L"Reveal whole map", BS_PUSHBUTTON,
           26, 264, 220, 28, ID_BT_REVEAL, g_ui_font);
    g_bt_mpunlock = mk_ctl(L"BUTTON", L"Unlock all MP maps", BS_PUSHBUTTON,
                           258, 264, 220, 28, ID_BT_MPUNLOCK, g_ui_font);
    EnableWindow(g_bt_mpunlock, mp_unlock_state_ok());
    mk_ctl(L"BUTTON", L"Complete all challenges", BS_PUSHBUTTON,
           26, 296, 220, 28, ID_BT_RETO, g_ui_font);
    mk_ctl(L"BUTTON", L"Unlock all achievements", BS_PUSHBUTTON,
           258, 296, 220, 28, ID_BT_ALLACH, g_ui_font);

    /* values */
    mk_ctl(L"BUTTON", L"Values", BS_GROUPBOX, 12, 360, PANEL_W - 24, 130, 0, g_ui_bold);
    mk_ctl(L"STATIC", L"Skill points", SS_LEFT, 26, 384, 96, 20, 0, g_ui_font);
    g_ed_pts  = mk_ctl(L"EDIT", L"0", WS_BORDER | ES_NUMBER | ES_RIGHT, 122, 382, 110, 26, ID_ED_PTS, g_ui_font);
    mk_ctl(L"BUTTON", L"Set", BS_PUSHBUTTON, 246, 381, 60, 28, ID_BT_PTS, g_ui_font);
    mk_ctl(L"BUTTON", L"Max", BS_PUSHBUTTON, 312, 381, 60, 28, ID_BT_PTS_MAX, g_ui_font);
    mk_ctl(L"STATIC", L"0 - 999", SS_LEFT, 386, 384, 200, 20, 0, g_ui_font);
    mk_ctl(L"STATIC", L"Money", SS_LEFT, 26, 414, 96, 20, 0, g_ui_font);
    g_ed_gold = mk_ctl(L"EDIT", L"2", WS_BORDER | ES_NUMBER | ES_RIGHT, 122, 412, 110, 26, ID_ED_GOLD, g_ui_font);
    mk_ctl(L"BUTTON", L"Set", BS_PUSHBUTTON, 246, 411, 60, 28, ID_BT_GOLD, g_ui_font);
    mk_ctl(L"BUTTON", L"Max", BS_PUSHBUTTON, 312, 411, 60, 28, ID_BT_GOLD_MAX, g_ui_font);
    mk_ctl(L"STATIC", L"2 - 999999", SS_LEFT, 386, 414, 200, 20, 0, g_ui_font);
    mk_ctl(L"STATIC", L"UN", SS_LEFT, 26, 444, 96, 20, 0, g_ui_font);
    g_ed_un   = mk_ctl(L"EDIT", L"0", WS_BORDER | ES_NUMBER | ES_RIGHT, 122, 442, 110, 26, ID_ED_UN, g_ui_font);
    mk_ctl(L"BUTTON", L"Set", BS_PUSHBUTTON, 246, 441, 60, 28, ID_BT_UN, g_ui_font);
    mk_ctl(L"BUTTON", L"Max", BS_PUSHBUTTON, 312, 441, 60, 28, ID_BT_UN_MAX, g_ui_font);
    mk_ctl(L"STATIC", L"0 - 999999", SS_LEFT, 386, 444, 200, 20, 0, g_ui_font);

    /* status */
    mk_ctl(L"BUTTON", L"Status", BS_GROUPBOX, 12, 496, PANEL_W - 24, 110, 0, g_ui_bold);
    mk_ctl(L"STATIC", L"HP", SS_LEFT, 26, 520, 40, 20, 0, g_ui_font);
    g_v_hp   = mk_ctl(L"STATIC", L"-", SS_LEFT, 70, 520, 190, 20, 0, g_ui_font);
    mk_ctl(L"STATIC", L"Level", SS_LEFT, 300, 520, 60, 20, 0, g_ui_font);
    g_v_lvl  = mk_ctl(L"STATIC", L"-", SS_LEFT, 364, 520, 120, 20, 0, g_ui_font);
    mk_ctl(L"STATIC", L"XP", SS_LEFT, 26, 544, 40, 20, 0, g_ui_font);
    g_v_xp   = mk_ctl(L"STATIC", L"-", SS_LEFT, 70, 544, 190, 20, 0, g_ui_font);
    mk_ctl(L"STATIC", L"Points", SS_LEFT, 300, 544, 60, 20, 0, g_ui_font);
    g_v_pts  = mk_ctl(L"STATIC", L"-", SS_LEFT, 364, 544, 120, 20, 0, g_ui_font);
    mk_ctl(L"STATIC", L"Money", SS_LEFT, 26, 568, 60, 20, 0, g_ui_font);
    g_v_gold = mk_ctl(L"STATIC", L"-", SS_LEFT, 90, 568, 140, 20, 0, g_ui_font);
    mk_ctl(L"STATIC", L"UN", SS_LEFT, 300, 568, 30, 20, 0, g_ui_font);
    g_v_un   = mk_ctl(L"STATIC", L"-", SS_LEFT, 334, 568, 150, 20, 0, g_ui_font);

    /* saves */
    mk_ctl(L"BUTTON", L"Saves", BS_GROUPBOX, 12, 612, PANEL_W - 24, 70, 0, g_ui_bold);
    g_bt_force = mk_ctl(L"BUTTON", L"Force save (SP only)", BS_PUSHBUTTON,
                        26, 636, 170, 28, ID_BT_FORCE, g_ui_font);
    EnableWindow(g_bt_force, !game_is_mp());   /* single-player only */
    mk_ctl(L"BUTTON", L"Saves...", BS_PUSHBUTTON, 206, 636, 120, 28, ID_BT_SAVES, g_ui_font);
    mk_ctl(L"BUTTON", L"MP saves...", BS_PUSHBUTTON, 332, 636, 120, 28, ID_BT_SAVES_MP, g_ui_font);

    /* items */
    mk_ctl(L"BUTTON", L"Items", BS_GROUPBOX, 12, 688, PANEL_W - 24, 420, 0, g_ui_bold);
    mk_ctl(L"STATIC", L"Lang", SS_LEFT, 26, 712, 36, 20, 0, g_ui_font);
    g_cm_lang = mk_ctl(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL,
                       66, 708, 150, 520, ID_CM_LANG, g_ui_font);
    mk_ctl(L"STATIC", L"Category", SS_LEFT, 236, 712, 70, 20, 0, g_ui_font);
    g_cm_cat = mk_ctl(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL,
                      310, 708, 200, 500, ID_CM_CAT, g_ui_font);
    mk_ctl(L"STATIC", L"Find", SS_LEFT, 26, 742, 36, 20, 0, g_ui_font);
    g_ed_find = mk_ctl(L"EDIT", L"", WS_BORDER, 66, 738, 190, 26, ID_ED_FIND, g_ui_font);
    mk_ctl(L"STATIC", L"Qty", SS_LEFT, 270, 742, 32, 20, 0, g_ui_font);
    g_ed_qty = mk_ctl(L"EDIT", L"1", WS_BORDER | ES_NUMBER | ES_RIGHT,
                      306, 738, 56, 26, ID_ED_QTY, g_ui_font);
    mk_ctl(L"STATIC", L"1 - 999", SS_LEFT, 368, 742, 56, 20, 0, g_ui_font);
    g_st_count = mk_ctl(L"STATIC", L"", SS_RIGHT, 430, 742, 174, 20, ID_ST_COUNT, g_ui_font);
    g_lb_items = mk_ctl(L"LISTBOX", L"", LBS_NOTIFY | WS_BORDER | WS_VSCROLL
                        | LBS_NOINTEGRALHEIGHT, 26, 770, 588, 146, ID_LB_ITEMS, g_ui_font);
    g_bt_bag   = mk_ctl(L"BUTTON", L"To bag", BS_PUSHBUTTON, 26, 924, 110, 28, ID_BT_TOBAG, g_ui_font);
    g_bt_grnd  = mk_ctl(L"BUTTON", L"To ground", BS_PUSHBUTTON, 146, 924, 110, 28, ID_BT_TOGRND, g_ui_font);
    g_ck_hiden = mk_ctl(L"BUTTON", L"hide rows without a name", BS_AUTOCHECKBOX,
                        270, 926, 344, 24, ID_CB_HIDEN, g_ui_font);
    g_st_item  = mk_ctl(L"STATIC", L"no selection", SS_LEFT, 26, 958, 588, 20, ID_ST_ITEM, g_ui_font);
    g_st_dev   = mk_ctl(L"STATIC", L"", SS_LEFT, 26, 980, 588, 20, ID_ST_DEV, g_ui_font);
    g_desc     = mk_ctl(L"EDIT", L"",
                        WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                        26, 1004, 588, 92, 0, g_ui_font);

    /* fill item pickers */
    {
        for (i = 0; i < LANG_COUNT; i++) {
            SendMessageW(g_cm_lang, CB_ADDSTRING, 0, (LPARAM)g_lang_labels[i]);
            if (i == g_lang) SendMessageW(g_cm_lang, CB_SETCURSEL, i, 0);
        }
        SendMessageW(g_cm_cat, CB_ADDSTRING, 0, (LPARAM)L"(all)");
        for (i = 0; i < CAT_COUNT; i++) {
            SendMessageW(g_cm_cat, CB_ADDSTRING, 0, (LPARAM)g_cat_names[i]);
            if (i == g_cat) SendMessageW(g_cm_cat, CB_SETCURSEL, i + 1, 0);
        }
        if (g_cat < 0) SendMessageW(g_cm_cat, CB_SETCURSEL, 0, 0);
    }
    if (!g_nitems) {
        EnableWindow(g_lb_items, FALSE);
        EnableWindow(g_ed_find, FALSE);
        EnableWindow(g_cm_lang, FALSE);
        EnableWindow(g_cm_cat, FALSE);
        EnableWindow(g_bt_bag, FALSE);
        EnableWindow(g_bt_grnd, FALSE);
        EnableWindow(g_ck_hiden, FALSE);
        SetWindowTextW(g_st_item, L"mod\\items.tsv missing");
    } else {
        items_rebuild();
    }

    g_hint = mk_ctl(L"STATIC", HINT_DEFAULT,
                    SS_CENTER, 14, 1118, PANEL_W - 28, 20, 0, g_ui_font);
    panel_sync_checks();
    panel_fill_edits();
    logmsg("panel created hwnd=%p", (void *)g_hwnd);
    (void)asp;
    return 1;
}

static int is_game_foreground(void)
{
    DWORD pid = 0;
    HWND fg = GetForegroundWindow();
    if (!fg) return 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

/* Periodic dump of who touched the watched field.  The distinct-EIP list alone
 * cannot answer "does this writer run every frame or only once", which is the
 * question when a multiplayer write is being reverted, so the counts are reset
 * after every report and the entity position is printed next to them. */
static void wp_report(void)
{
    LONG n = g_wp_seen, k;
    unsigned e = entity_addr();
    logmsg("watch: %ld hits since the last report on %08x, player at (%.1f,%.1f) vy %.1f",
           g_wp_period, (unsigned)g_wp_addr,
           e ? (double)*(float *)(e + 0x520) : 0.0,
           e ? (double)*(float *)(e + 0x524) : 0.0,
           e ? (double)*(float *)(e + 0x528) : 0.0);
    g_wp_period = 0;
    for (k = 0; k < n && k < 64; k++) {
        unsigned char *ip = (unsigned char *)(size_t)g_wp_eip[k];
        if (ip && !IsBadReadPtr(ip, 4))
            logmsg("   eip=%08x op=%02x %02x %02x %02x hits=%ld",
                   (unsigned)g_wp_eip[k], ip[0], ip[1], ip[2], ip[3], g_wp_cnt[k]);
        else
            logmsg("   eip=%08x hits=%ld", (unsigned)g_wp_eip[k], g_wp_cnt[k]);
        g_wp_cnt[k] = 0;
    }
}

/* [debug] nudge=dx,dy - move the player entity a couple of seconds after the
 * core starts, either in one jump or (with [debug] nudge_ramp=1) in 20 steps of
 * dx/20 every 30 ms, so an external capture can compare "the write landed" with
 * "the picture followed".  The ramp form is what tests whether the multiplayer
 * render only refreshes while the game sees the position changing. */
static int g_nudge_ramp;

static DWORD WINAPI nudge_ramp_thread(LPVOID p)
{
    unsigned e = (unsigned)(size_t)p;
    float stepx = g_nudge_x / 20.0f, stepy = g_nudge_y / 20.0f;
    int i;
    for (i = 0; i < 20; i++) {
        if (!readable((void *)(size_t)(e + 0x520), 8)) break;
        *(float *)(e + 0x520) += stepx;
        *(float *)(e + 0x524) += stepy;
        Sleep(30);
    }
    if (readable((void *)(size_t)(e + 0x520), 8))
        logmsg("nudge: ramp done ent=%08x now (%.1f,%.1f)", e,
               (double)*(float *)(e + 0x520), (double)*(float *)(e + 0x524));
    return 0;
}

/* [debug] poke=ADDR,VALUE - write one float to one absolute address once, a
 * couple of seconds after the core starts, and clear the key.  Used to test
 * whether a given address is the position the renderer reads: if writing it
 * moves the picture while writing the entity does not, that address is the one.
 * VALUE is a plain float; the driving script reads the entity coordinate first
 * and passes it in, so nothing here has to resolve entity offsets. */
static unsigned g_poke_addr;
static float    g_poke_val;
static int      g_poke_wait;

static void poke_fire(void)
{
    char p[MAX_PATH];
    if (!g_poke_addr) return;
    if (IsBadWritePtr((void *)(size_t)g_poke_addr, 4)) {
        logmsg("poke: %08x is not writable, nothing written", g_poke_addr);
    } else {
        float old = *(float *)(size_t)g_poke_addr;
        *(float *)(size_t)g_poke_addr = g_poke_val;
        logmsg("poke: %08x %.4f -> %.4f", g_poke_addr, (double)old, (double)g_poke_val);
    }
    cfg_path(p, sizeof p);
    WritePrivateProfileStringA("debug", "poke", "", p);
}

/* ---- multiplayer net player object (docs/DEVELOPMENT.md section 13.20) ----
 * The network session is a static 1.4 MB object at 0x01477088.  Its +0xB0E00
 * block is the world/player state that 0x488BA0 serialises; +0xC of that block
 * is the local player state object, and 0x4BBEB0 sends +0x1C/+0x20 of it as the
 * position.  Every address here came out of the disassembly, so each step is
 * checked before it is dereferenced.
 *
 * [debug] netwatch=1  - log the net object's position whenever it moves, next
 *                       to the entity position, so the two can be compared.
 * [debug] pokenet=x,y - one-shot: write that position into the net object only
 *                       (the entity is left alone).  If the picture moves, this
 *                       is the store the MP renderer reads. */
#define ADDR_SESSION 0x01477088u

static int   g_netwatch;
static int   g_vprobe;                   /* [debug] vprobe: log the entity's walking speed */
static int   g_hpwatch;                  /* [debug] hpwatch: log HP/state changes */
static int   g_pokenet_wait;
static float g_pokenet_x, g_pokenet_y;
static float g_net_last_x = -99999.0f, g_net_last_y;

static unsigned net_player_obj(void)
{
    unsigned p;
    if (!readable((void *)ADDR_SESSION, 0xB0E10)) return 0;
    p = *(unsigned *)(size_t)(ADDR_SESSION + 0xB0E00 + 0xC);
    if (p < 0x10000 || (p & 3)) return 0;
    if (!readable((void *)(size_t)(p + 0x1C), 8)) return 0;
    return p;
}

/* Write the position the multiplayer side actually draws from.  Measured live
 * on 2026-10-24: writing only entity+0x520/+0x524 leaves the picture behind,
 * writing this object together with the entity moves the character and it
 * stays there.  Returns 0 when the session/player object is not resolvable, so
 * the caller can fall back to the entity write. */
static int net_pos_write(float x, float y)
{
    unsigned p = net_player_obj();
    if (!p || IsBadWritePtr((void *)(size_t)(p + 0x1C), 8)) return 0;
    *(float *)(size_t)(p + 0x1C) = x;
    *(float *)(size_t)(p + 0x20) = y;
    return 1;
}

static void net_player_tick(void)
{
    unsigned o = net_player_obj();
    unsigned e;
    float nx, ny, ex = 0.0f, ey = 0.0f;
    if (!o) return;
    nx = *(float *)(size_t)(o + 0x1C);
    ny = *(float *)(size_t)(o + 0x20);
    e = entity_addr();
    if (e) {
        ex = *(float *)(size_t)(e + 0x520);
        ey = *(float *)(size_t)(e + 0x524);
    }
    if (g_netwatch && (nx != g_net_last_x || ny != g_net_last_y)) {
        logmsg("netpos: obj=%08x (%.2f,%.2f) entity (%.2f,%.2f) net-entity (%+.2f,%+.2f)",
               o, (double)nx, (double)ny, (double)ex, (double)ey,
               (double)(nx - ex), (double)(ny - ey));
        g_net_last_x = nx;
        g_net_last_y = ny;
    }
    if (g_pokenet_wait > 0 && --g_pokenet_wait == 0) {
        char p[MAX_PATH];
        *(float *)(size_t)(o + 0x1C) = g_pokenet_x;
        *(float *)(size_t)(o + 0x20) = g_pokenet_y;
        logmsg("pokenet: obj=%08x (%.2f,%.2f) -> (%.2f,%.2f), entity stays (%.2f,%.2f)",
               o, (double)nx, (double)ny, (double)g_pokenet_x, (double)g_pokenet_y,
               (double)ex, (double)ey);
        g_net_last_x = g_pokenet_x;
        g_net_last_y = g_pokenet_y;
        cfg_path(p, sizeof p);
        WritePrivateProfileStringA("debug", "pokenet", "", p);
    }
}

static void nudge_fire(void)
{
    unsigned e = entity_addr();
    char p[MAX_PATH];
    if (!e || !readable((void *)(size_t)(e + 0x520), 8)) {
        logmsg("nudge: no player entity, nothing written");
    } else if (g_nudge_ramp) {
        HANDLE ht = CreateThread(NULL, 0, nudge_ramp_thread, (LPVOID)(size_t)e, 0, NULL);
        logmsg("nudge: ramp (%.1f,%.1f) over 600 ms from (%.1f,%.1f)%s",
               (double)g_nudge_x, (double)g_nudge_y,
               (double)*(float *)(e + 0x520), (double)*(float *)(e + 0x524),
               ht ? "" : " - CreateThread FAILED");
        if (ht) CloseHandle(ht);
    } else {
        float x = *(float *)(e + 0x520) + g_nudge_x;
        float y = *(float *)(e + 0x524) + g_nudge_y;
        *(float *)(e + 0x520) = x;
        *(float *)(e + 0x524) = y;
        logmsg("nudge: ent=%08x (%.1f,%.1f) -> (%.1f,%.1f)", e,
               (double)(x - g_nudge_x), (double)(y - g_nudge_y), (double)x, (double)y);
    }
    cfg_path(p, sizeof p);
    WritePrivateProfileStringA("debug", "nudge", "", p);
}

static void tick(void)
{
    unsigned s = stats_addr();
    /* A lost keyup would leave the one-press edge set and swallow every later
     * press.  Read the physical key instead of using a time threshold: a time
     * threshold would turn held auto-repeat into an unwanted hover. */
    if (g_jump_down && g_jump_vk &&
        !(GetAsyncKeyState((int)g_jump_vk) & 0x8000))
        g_jump_down = 0;
    if (g_nudge_wait > 0 && --g_nudge_wait == 0) nudge_fire();
    if (g_poke_wait > 0 && --g_poke_wait == 0) poke_fire();
    net_player_tick();
    if (g_hpwatch) {
        /* log every HP / state change: tells whether an instant-death trap
         * (spikes) goes through the damage path (HP drops) or sets a state */
        unsigned he = entity_addr();
        if (he && readable((void *)(size_t)(he + ENT_MAXHP), 8)) {
            static int lmx, have;
            static float lhp;
            static LONG lst;
            int mx = *(int *)(size_t)(he + ENT_MAXHP);
            float hp = *(float *)(size_t)(he + ENT_CURHP);
            LONG st = readable((void *)(size_t)(he + 0x540), 4)
                      ? *(LONG *)(size_t)(he + 0x540) : -1;
            if (!have || mx != lmx || hp != lhp || st != lst) {
                logmsg("hpwatch: hp %.1f/%d state %ld  (was %.1f/%d state %ld)",
                       (double)hp, mx, st, (double)lhp, lmx, lst);
                lmx = mx;
                lhp = hp;
                lst = st;
                have = 1;
            }
        }
    }
    if (g_vprobe) {
        /* entity x difference over one tick: tells whether the Move speed
         * multiplier reaches the physics in this mode (the hook logs the
         * argument, this logs the resulting motion) */
        static DWORD vt;
        static float vx0;
        static int have;
        unsigned ev = entity_addr();
        DWORD now = GetTickCount();
        if (ev && readable((void *)(size_t)(ev + 0x520), 4)) {
            float x = *(float *)(size_t)(ev + 0x520);
            if (have) {
                DWORD dt = now - vt;
                if (dt >= 50) {
                    float v = (x - vx0) * 1000.0f / (float)dt;
                    if (v > 8.0f || v < -8.0f)
                        logmsg("vprobe: x=%.2f dx=%.2f dt=%lums v=%.0f units/s",
                               (double)x, (double)(x - vx0), dt, (double)v);
                }
            }
            vt = now;
            vx0 = x;
            have = 1;
        } else {
            have = 0;
        }
    }
    if (g_maxxp && s) {
        int need = stats_get(s, OFF_NEED);
        int xp   = stats_get(s, OFF_XP);
        if (need > 0 && xp < need) stats_set(s, OFF_XP, need);
    }
    if (g_watch_ent_off) {
        unsigned we = entity_addr();
        if (we && (LONG)(we + g_watch_ent_off) != g_wp_addr)
            wp_arm(we + g_watch_ent_off);
    } else if (g_watch_net) {
        /* the net player object is re-created when the room changes, so the
         * address is resolved fresh every tick instead of being pinned */
        unsigned no = net_player_obj();
        if (no && (LONG)(no + 0x1C) != g_wp_addr) wp_arm(no + 0x1C);
    } else if (g_watch_addr) {
        /* [debug] watch=<hex> wins: it is what the reverse-engineering notes ask for */
        if ((LONG)g_watch_addr != g_wp_addr) wp_arm(g_watch_addr);
    } else if (DIAG_WP && !g_wp_auto_done) {
        unsigned e = entity_addr();
        if (e && (LONG)(e + ENT_CURHP) != g_wp_addr) wp_arm(e + ENT_CURHP);
    }
    if (g_watch_ent_off || g_watch_addr || g_watch_net) {
        static int wr_div;
        if (++wr_div >= 10) { wr_div = 0; wp_report(); }   /* about every 2 s */
    }
    {
        static int bd_div;
        if (++bd_div >= 10) { bd_div = 0; bag_diff(); }   /* about every 2 s */
    }
    {
        unsigned e = entity_addr();
        g_player_x = e ? (LONG)(e + 0x520) : 0;
        if (e && g_airjump) {
            float vy = *(float *)(e + 0x528);
            if (!g_jump_key_forced && vy < -80.0f && g_prev_vy > -40.0f) {
                LONG dt = (LONG)GetTickCount() - g_last_vk_tick;
                if (dt >= 0 && dt < 500 && g_last_vk && g_jump_vk != g_last_vk) {
                    g_jump_vk = g_last_vk;
                    logmsg("airjump: learned jump key vk=%ld (impulse is fixed at %.1f)",
                           g_jump_vk, (double)AIRJUMP_IMPULSE);
                }
            }
            g_prev_vy = vy;
        }
    }
    if (g_aj_probe_t0) {
        unsigned e = entity_addr();
        LONG now = (LONG)GetTickCount();
        if ((LONG)e != g_aj_probe_ent) {
            logmsg("ajprobe: entity changed %08x -> %08x", (unsigned)g_aj_probe_ent, e);
            g_aj_probe_t0 = 0;
        } else if (e && now - g_aj_probe_t0 >= 180) {
            float y0, vy0;
            memcpy(&y0, (const void *)&g_aj_probe_y0, 4);
            memcpy(&vy0, (const void *)&g_aj_probe_vy0, 4);
            logmsg("ajprobe: +%ldms y %.1f -> %.1f (dy %.1f), vy %.1f -> %.1f, state %ld",
                   now - g_aj_probe_t0, (double)y0, (double)*(float *)(e + 0x524),
                   (double)(*(float *)(e + 0x524) - y0), (double)vy0,
                   (double)*(float *)(e + 0x528), *(LONG *)(e + 0x540));
            g_aj_probe_t0 = 0;
        }
    }
    if (g_tp_check_tick && (LONG)(GetTickCount() - g_tp_check_tick) >= 400) {
        unsigned e = entity_addr();
        if ((LONG)e != g_tp_ent) e = 0;   /* level changed: never write the old spot */
        float want, ox, oy;
        *(unsigned *)&want = (unsigned)g_tp_new_x;
        *(unsigned *)&ox   = (unsigned)g_tp_old_x;
        *(unsigned *)&oy   = (unsigned)g_tp_old_y;
        g_tp_check_tick = 0;
        if (e) {
            float px = *(float *)(e + 0x520);
            float py = *(float *)(e + 0x524);
            /* no automatic restore any more: the game's own collision resolver
             * already puts the character at the nearest legal spot, and writing
             * the old position back on top of that produced a second jump that
             * looked like random teleporting.  Just report where we ended up. */
            if (px - want > 24.0f || want - px > 24.0f)
                logmsg("tp: ended at x=%.1f (asked %.1f, wall stopped it) y=%.1f",
                       (double)px, (double)want, (double)py);
            else
                logmsg("tp: landed at (%.1f,%.1f)", (double)px, (double)py);
            (void)ox; (void)oy;
        }
    }
}

/* Which container does this 0xAC record belong to?  Read-only, range compared
 * against the record area of the player's bag and of the current ground pile
 * (bag/ground +0x10 + n*0xAC).  Only used for log text. */
static const char *slot_where(unsigned slot)
{
    unsigned e = entity_addr();
    int i;
    if (!slot || slot < 0x10000) return "-";
    for (i = 0; i < 2; i++) {
        unsigned C = 0;
        if (i == 0) {
            if (!e) continue;
            if (IsBadReadPtr((void *)(size_t)(e + 0x1038), 4)) continue;
            C = *(unsigned *)(e + 0x1038);
        } else {
            unsigned sv = stats_addr(), lvl = 0;
            if (sv && !IsBadReadPtr((void *)(size_t)(sv + OFF_LEVEL_OBJ), 4))
                lvl = *(unsigned *)(sv + OFF_LEVEL_OBJ);
            C = lvl ? lvl + LEVEL_GROUND_OFF : 0;
            if (!C && !IsBadReadPtr((void *)(size_t)ADDR_GROUND_PTR, 4))
                C = *(unsigned *)ADDR_GROUND_PTR;
        }
        if (!C || IsBadReadPtr((void *)(size_t)C, 0x14)) continue;
        {
            unsigned lo = C + 0x10;
            LONG n = *(int *)(C + 8);
            if (n < 0 || n > BAG_MAX_SLOTS) continue;
            if (slot >= lo && slot < lo + (unsigned)n * 0xACu)
                return i == 0 ? "bag" : "ground";
        }
    }
    if (slot >= (unsigned)DESC_ARRAY && slot < (unsigned)DESC_ARRAY + DESC_COUNT * DESC_STRIDE)
        return "descriptor";
    return "other";
}

/* periodic diagnostic dump: only when something actually moved */
static void report(int ticks)
{
    static LONG last_calls, last_clamp, last_dec, last_dm, last_d2, last_seen, last_it;
    static LONG last_mu;
    unsigned e = entity_addr();
    float delta = 0.0f, amt = 0.0f;
    *(unsigned *)&delta = (unsigned)g_hp_lastdelta;
    *(unsigned *)&amt   = (unsigned)g_dm_lastamt;

    /* every distinct EIP that wrote the player's HP gets one log line */
    if (g_wp_seen != last_seen) {
        for (LONG i = last_seen; i < g_wp_seen && i < 64; i++)
            logmsg("WATCH: write by EIP %08x", (unsigned)g_wp_eip[i]);
        last_seen = g_wp_seen;
    }
    if (g_hp_calls == last_calls && g_clamp_hits == last_clamp
        && g_dec_total == last_dec && g_dm_calls == last_dm && g_d2_calls == last_d2
        && g_mu_calls == last_mu
        && g_it_calls == last_it) return;
    last_calls = g_hp_calls; last_clamp = g_clamp_hits;
    last_dec = g_dec_total;  last_dm = g_dm_calls; last_d2 = g_d2_calls;
    last_mu = g_mu_calls;
    last_it = g_it_calls;
    logmsg("diag t=%d | dmg_int calls=%ld player=%ld blocked=%ld lastthis=%08x ret=%08x amt=%ld",
           ticks, g_d2_calls, g_d2_player, g_d2_blocked,
           (unsigned)g_d2_lastthis, (unsigned)g_d2_lastret, g_d2_lastamt);
    logmsg("     hurt t=%d calls=%ld player=%ld blocked=%ld lastthis=%08x ret=%08x amt=%.4g",
           ticks, g_dm_calls, g_dm_player, g_dm_blocked,
           (unsigned)g_dm_lastthis, (unsigned)g_dm_lastret, (double)amt);
    logmsg("     add_hp calls=%ld player=%ld neg=%ld lastthis=%08x ret=%08x delta=%.4g",
           g_hp_calls, g_hp_player, g_hp_neg,
           (unsigned)g_hp_lastthis, (unsigned)g_hp_lastret, (double)delta);
    logmsg("     items decreases=%ld last type=%ld %ld->%ld caller=%08x slot=%08x(%s) clamped=%ld"
           " | player_ent=%08x god=%ld nocons=%ld",
           g_dec_total, g_dec_type, g_dec_old, g_dec_new, (unsigned)g_dec_ret,
           (unsigned)g_dec_slot, slot_where((unsigned)g_dec_slot),
           g_clamp_hits, e, g_god, g_nocons);
    logmsg("     consume calls=%ld blocked=%ld last type=%ld n=%ld caller=%08x",
           g_it_calls, g_it_blocked, g_it_lasttype, g_it_lastn,
           (unsigned)g_it_lastret);
    logmsg("     count-add calls=%ld (422540=%ld 422660=%ld 4225a0=%ld) dec=%ld blocked=%ld"
           " last src=%08x slot=%08x(%s) type=%ld %ld->%ld delta=%ld caller=%08x",
           g_mu_calls, g_mu_422540, g_mu_422660, g_mu_4225a0, g_mu_dec, g_mu_blocked,
           (unsigned)g_mu_src, (unsigned)g_mu_slot, slot_where((unsigned)g_mu_slot),
           g_mu_type, g_mu_old, g_mu_new, g_mu_delta, (unsigned)g_mu_ret);
}

/* low-level input hooks.  They live on their OWN thread (hook_worker below):
 * the callback runs on the thread that installed the hook and blocks the whole
 * system until it returns, so that thread must never show a dialog, write a
 * file or wait on anything.  The panel and its message boxes run on the worker
 * thread instead. */
static HHOOK g_kbd;
static HANDLE g_hook_thread;
static DWORD g_hook_tid;
static HANDLE g_hook_ready;

/* ---------------- the room's walkability grid -----------------------------
 * *(entity+0x684) is the room object (0x1A04 bytes).  At +0xABF the game keeps
 * a 63x31 raster, one byte per 16x16 world-pixel cell, COLUMN major:
 *      idx = cx*31 + cy      cx 0..62 (x = cx*16), cy 0..30 (y = cy*16)
 * 0 = open, non-zero = solid.  A standing entity has its feet on the TOP edge
 * of a solid cell and its ~60 px body in the four cells above it.
 *
 * Verified live 2026-10-23 three ways: the raster drawn against the screen
 * matched the walls; the block that stopped the player walking right
 * (cx 21..29, four cells tall) is solid in it; and the game's own
 * phase-advance spell took the player from x=25 to x=983, which is exactly
 * this raster's last standable column (cx 61) minus the body half-width.
 *
 * The room object is re-read every call: two of them alternate at runtime
 * (seen swapping between 0x00803D80 and 0x00826A24 within one second).
 */
#define GRID_W    63
#define GRID_H    31
#define GRID_CELL 16.0f
#define GRID_BODY 4                 /* cells of clear space the body needs */
#define GRID_REACH 8                /* sideways fallback radius, in cells (128 px) */
/* the world point that sits at the centre of the client area.  It is NOT the
 * room centre (504,248) - the camera puts the room 54 units right and 40 units
 * up from the window centre - but it is constant across window sizes; measured
 * 2026-10-23 at 2048x1152 (two rooms) and at maximized 2560x1529.  Together
 * with pixel-per-unit = client_height / TP_VIEW_H this makes the whole click
 * map computable at any resolution, no calibration needed. */
#define TP_VIEW_H  640.0f
#define TP_ORIGIN_X 450.0f
#define TP_ORIGIN_Y 286.0f

static unsigned char g_grid[GRID_W * GRID_H];
static unsigned g_grid_room;

static int grid_load(void)
{
    unsigned e = entity_addr(), room;
    if (!e || IsBadReadPtr((void *)(size_t)(e + 0x684), 4)) return 0;
    room = *(unsigned *)(size_t)(e + 0x684);
    if (!room || IsBadReadPtr((void *)(size_t)(room + 0xABF), GRID_W * GRID_H))
        return 0;
    memcpy(g_grid, (void *)(size_t)(room + 0xABF), sizeof g_grid);
    g_grid_room = room;
    return 1;
}

/* outside the room is solid: nothing standable beyond the walls */
static int grid_solid(int cx, int cy)
{
    if (cx < 0 || cx >= GRID_W || cy < 0 || cy >= GRID_H) return 1;
    return g_grid[cx * GRID_H + cy] != 0;
}

/* feet on the top edge of cell (cx, fy) */
static int grid_standable(int cx, int fy)
{
    int k;
    if (!grid_solid(cx, fy)) return 0;
    for (k = 1; k <= GRID_BODY; k++)
        if (grid_solid(cx, fy - k)) return 0;
    return 1;
}

/* First standable cell in one column, searched outward from the clicked row and
 * over the whole column: the nearest floor below the click wins ties against
 * the nearest one above, because a click in mid air should drop rather than
 * float.  Returns 0 when the column has nothing standable at all (a shaft, a
 * solid wall from top to bottom). */
static int grid_column_landing(int cx, int cy0, int *ofy)
{
    int d;
    for (d = 0; d < GRID_H; d++) {
        int fy = cy0 + d;
        if (fy >= GRID_BODY + 1 && fy < GRID_H && grid_standable(cx, fy)) {
            *ofy = fy;
            return 1;
        }
        if (d == 0) continue;
        fy = cy0 - d;
        if (fy >= GRID_BODY + 1 && fy < GRID_H && grid_standable(cx, fy)) {
            *ofy = fy;
            return 1;
        }
    }
    return 0;
}

/* Landing point for a requested world point.
 *
 * Stage 1 keeps the click's own column, then the two columns beside it, so the
 * character always ends up under the cursor in x whenever that is possible:
 * a click in mid air drops onto the floor under it, a click inside rock rises
 * onto the top of that rock, a click on a floor stays on that floor.
 * Stage 2 only runs when those three columns have nothing standable (a shaft,
 * the far side of a block): then the nearest cell within GRID_REACH, in any
 * direction.  *how names the stage, for the log.
 * Returns 0 when the point is outside the room or nothing nearby is standable,
 * so nothing is written and the click is refused instead of teleporting somewhere
 * unrelated. */
static int grid_find_landing(float wx, float wy, float *ox, float *oy,
                             const char **how)
{
    int cx0 = (int)(wx / GRID_CELL), cy0 = (int)(wy / GRID_CELL);
    int r, dx, dy, fy;

    if (how) *how = "none";
    if (cx0 < 0 || cx0 >= GRID_W || cy0 < 0 || cy0 >= GRID_H) return 0;
    for (r = 0; r <= 1; r++) {
        for (dx = -r; dx <= r; dx++) {
            int cx = cx0 + dx;
            if (cx < 0 || cx >= GRID_W) continue;
            if (!grid_column_landing(cx, cy0, &fy)) continue;
            *ox = (float)cx * GRID_CELL + GRID_CELL / 2.0f;
            *oy = (float)fy * GRID_CELL;
            if (how) *how = r ? "column+1" : "column";
            return abs(dx) + 1;
        }
    }
    for (r = 1; r <= GRID_REACH; r++) {
        int best_cx = -1, best_cy = -1, best_d = 0x7FFFFFFF;
        for (dx = -r; dx <= r; dx++) {
            int cx = cx0 + dx;
            if (cx < 0 || cx >= GRID_W) continue;
            for (dy = -r; dy <= r; dy++) {
                int f = cy0 + dy, d;
                if (abs(dx) != r && abs(dy) != r) continue;      /* this ring only */
                if (f <= GRID_BODY || f >= GRID_H) continue;
                if (!grid_standable(cx, f)) continue;
                d = dx * dx + dy * dy;
                if (d < best_d) { best_d = d; best_cx = cx; best_cy = f; }
            }
        }
        if (best_cx >= 0) {
            *ox = (float)best_cx * GRID_CELL + GRID_CELL / 2.0f;
            *oy = (float)best_cy * GRID_CELL;
            if (how) *how = "ring";
            return r + 1;
        }
    }
    return 0;
}

/* ---------------- right-click teleport ------------------------------------
 * The cursor maps onto the room, not onto the player: the camera does not move
 * inside a room, so the client pixel -> world conversion is a fixed affine map
 * (a 640-unit-tall view scaled to the client height, anchored at world
 * (-120,-32) in the client's top-left corner; measured, see below).  The
 * derived point is then resolved by grid_find_landing, which keeps the clicked
 * column: a click in mid air drops onto the floor under it, a click inside rock
 * rises onto the top of it, a click high above a platform lands on that
 * platform.  Nothing is written when the room has nothing standable nearby, so
 * a bad click no longer leaves the player where they were with no explanation,
 * and the engine never gets a position inside rock to roll back.
 */
static void tp_go(int sx, int sy)
{
    unsigned e = entity_addr();
    HWND gw = find_game_window();
    RECT cr;
    POINT c;
    float px, py, tx, ty, wx, wy;

    if (!g_teleport) return;
    if (!e) { logmsg("tp: no player entity"); return; }
    /* In multiplayer the entity write alone is invisible: the game draws from
     * the player state object it serialises for the peer (section 13.20).  With
     * mp_tp=1 the landing is written into both; mp_tp=0 keeps the in-room
     * teleport single-player only.  The game's room transition is NOT used -
     * that crashed the MP room loader on 2026-10-23. */
    if (game_is_mp() && !g_mp_tp) {
        logmsg("tp: in-room teleport is disabled in multiplayer (mp_tp=0)");
        return;
    }
    if (IsBadWritePtr((void *)(size_t)(e + 0x520), 8)) {
        logmsg("tp: player entity %08x is not writable", e);
        return;
    }
    if (!gw) { logmsg("tp: game window not found"); return; }
    if (!GetClientRect(gw, &cr) || cr.right <= 0 || cr.bottom <= 0) {
        logmsg("tp: GetClientRect failed (%dx%d)", (int)cr.right, (int)cr.bottom);
        return;
    }
    c.x = sx;
    c.y = sy;
    if (!ScreenToClient(gw, &c)) return;
    if (c.x < 0 || c.y < 0 || c.x >= cr.right || c.y >= cr.bottom) {
        logmsg("tp: click outside the client area (client %d,%d)", c.x, c.y);
        return;
    }
    px = *(float *)(e + 0x520);
    py = *(float *)(e + 0x524);
    {
        char ini[MAX_PATH];
        int pct, ring, bias_x, bias_y;
        const char *how = "none";
        float sc, ppu, lx = 0.0f, ly = 0.0f;
        cfg_path(ini, sizeof ini);
        pct = GetPrivateProfileIntA("cheats", "tp_scale", 100, ini);
        if (pct < 10) pct = 10;
        if (pct > 400) pct = 400;
        g_tp_scale = (float)pct / 100.0f;
        sc = g_tp_scale;
        /* Click -> world, measured 2026-10-23 from window captures with the
         * player standing still.  The game renders a 640-world-unit-tall view
         * scaled to the client height, and the world point at the centre of
         * the client area is (450,286) - the room centre (504,248) shifted 54
         * units left and 40 up by the camera.  So
         *
         *      world = (450,286) + (client - client_centre) * (640/client_h)
         *
         * Captures behind it:
         *   2048x1152 room A  feet (1322,207) for world (616,80)
         *   2048x1152 room C  feet (1875,345) for world (914,160)
         *   2560x1529 room C  feet (378,463)  for world (73.4,160)  [maximized]
         * Back-solving the centre origin gives (450.4,285.0), (441.2,288.3)
         * and (451.0,286.2) - the x spread is the sprite's drawn centre vs
         * the entity x, about 16 px.  Crucially the origin does NOT move when
         * the window is resized or maximized, so the map is computed, not
         * calibrated.  The old "base 1280x720 centred" model had the scale
         * 12% too small (1.6 vs 1.8 px/unit at 2048 wide) - that was the
         * "left accurate, drifts further right" report, ~200 px at world
         * x=900.
         * tp_scale multiplies the zoom; tp_bias_x / tp_bias_y shift the origin
         * (world units, re-read on every click) for fine tuning. */
        ppu = (float)cr.bottom / TP_VIEW_H * sc;      /* client px per world unit */
        bias_x = GetPrivateProfileIntA("cheats", "tp_bias_x", 0, ini);
        bias_y = GetPrivateProfileIntA("cheats", "tp_bias_y", 0, ini);
        if (bias_x < -400) bias_x = -400;
        if (bias_x > 400) bias_x = 400;
        if (bias_y < -400) bias_y = -400;
        if (bias_y > 400) bias_y = 400;
        wx = TP_ORIGIN_X + ((float)c.x - (float)cr.right * 0.5f) / ppu + (float)bias_x;
        wy = TP_ORIGIN_Y + ((float)c.y - (float)cr.bottom * 0.5f) / ppu + (float)bias_y;
        if (!grid_load()) {
            logmsg("tp: no room grid at *(entity+0x684) - not inside a level? move aborted"
                   " | client (%d,%d) player (%.1f,%.1f)", c.x, c.y,
                   (double)px, (double)py);
            return;
        }
        ring = grid_find_landing(wx, wy, &lx, &ly, &how);
        if (!ring) {
            logmsg("tp: refused, nothing standable in that column or within %d cells of"
                   " (%.1f,%.1f) | client (%d,%d) of %dx%d px/unit %.3f"
                   " | player (%.1f,%.1f) self (%.1f,%.1f)",
                   GRID_REACH, (double)wx, (double)wy, c.x, c.y,
                   (int)cr.right, (int)cr.bottom, (double)ppu,
                   (double)px, (double)py, (double)(wx - px), (double)(wy - py));
            return;
        }
        tx = lx; ty = ly;
        if (ring == 1) {
            /* Found in the clicked column: keep the cursor's own x instead of
             * the cell centre.  The centre rounding alone was up to 8 world
             * units = 13 screen px of sideways drift, which is what "slightly off"
             * describes once the landing rule itself is right.  Stay 2 units
             * inside the cell so the body never straddles its neighbour. */
            float cl = (float)(int)(wx / GRID_CELL) * GRID_CELL;
            tx = wx;
            if (tx < cl + 2.0f) tx = cl + 2.0f;
            if (tx > cl + GRID_CELL - 2.0f) tx = cl + GRID_CELL - 2.0f;
        }
        logmsg("tp: client (%d,%d) of %dx%d px/unit %.3f scale=%.2f bias (%d,%d)"
               " | asked (%.1f,%.1f) -> standable (%.1f,%.1f) snap (%+.1f,%+.1f) via %s"
               " | player (%.1f,%.1f) self (%+.1f,%+.1f)",
               c.x, c.y, (int)cr.right, (int)cr.bottom, (double)ppu, (double)sc,
               bias_x, bias_y,
               (double)wx, (double)wy, (double)tx, (double)ty,
               (double)(tx - wx), (double)(ty - wy), how,
               (double)px, (double)py, (double)(wx - px), (double)(wy - py));
    }
    g_tp_ent = (LONG)e;
    g_tp_old_x = (LONG)*(unsigned *)&px;
    g_tp_old_y = (LONG)*(unsigned *)&py;
    g_tp_new_x = (LONG)*(unsigned *)&tx;
    g_tp_new_y = (LONG)*(unsigned *)&ty;
    g_tp_check_tick = (LONG)GetTickCount();
    if (!g_tp_check_tick) g_tp_check_tick = 1;
    *(float *)(e + 0x520) = tx;
    *(float *)(e + 0x524) = ty;
    g_tp_guard_until = (LONG)GetTickCount() + 800;   /* the god guard must not undo this */
    if (!g_tp_guard_until) g_tp_guard_until = 1;
    if (game_is_mp()) {
        int ok = net_pos_write(tx, ty);
        logmsg("tp: mp wrote the net player position (%s)", ok ? "ok" : "object not found");
    }
}


/* ---- big map: right-click warps to the room under the cursor -------------
 * The map screen singleton is 0x0148817C: +0x0 = 1 while the map is open,
 * +0x4/+0x8 = cell under the cursor (-1 when none).  Cell records live at
 * 0x00B03008 + cell*0x2D0 (zone +0x2B8, room +0x2B0).  A warp runs the game's
 * own transition (0x00447080 + mirror the level pointer + hold the recorded
 * position) on the game thread via WM_APP+16.  Rooms without a recorded
 * spot (mod\roomspots.tsv) use the first entity of the room as the landing.
 */
#define MAP_OBJ 0x0148817Cu

static int spot_find(unsigned z, unsigned r, float *x, float *y);

static int map_is_open(void)
{
    unsigned v = *(unsigned *)(size_t)MAP_OBJ;
    return v == 1;
}

static void map_right_click(void)
{
    unsigned x = *(unsigned *)(size_t)(MAP_OBJ + 0x04);
    unsigned y = *(unsigned *)(size_t)(MAP_OBJ + 0x08);
    unsigned cell;
    float sx, sy;
    HWND gw;
    if (x > 15 || y > 15) {
        logmsg("mapwarp: right-click, no room under the cursor (%u,%u)", x, y);
        return;
    }
    cell = (x << 4) + y;
    /* the level object keeps the room's map cell at +0x22A88/+0x22A8C and the
     * loader takes exactly that pair, so the cell is the room key here.
     * Only rooms with a recorded landing spot are warped: the prop-position
     * fallback was tried on 2026-10-23 and left rooms without a usable prop
     * unrendered and frozen (cell 6,11), so the map never uses it. */
    gw = find_game_window();
    if (!gw) return;
    if (!spot_find(x, y, &sx, &sy)) {
        /* no spot yet: use the first entity of the room as the landing.
         * The 0xB03008 record is not a reliable "is there a room" test - real
         * rooms can have an all-zero progress record (cell 0,10) - so just
         * try, the user retries the same cell if the room comes up black. */
        if (g_wb_zone >= 0 || g_wb_room >= 0) return;   /* one request at a time */
        g_wb_zone = (int)x;
        g_wb_room = (int)y;
        logmsg("mapwarp: cell %u (%u,%u) no spot, trying an approximate landing",
               cell, x, y);
        PostMessageW(gw, WM_APP + 16, 0, 0);
        return;
    }
    if (g_wb_zone >= 0 || g_wb_room >= 0) return;     /* one warp at a time */
    g_wb_zone = (int)x;
    g_wb_room = (int)y;
    logmsg("mapwarp: cell %u (%u,%u) spot (%.1f,%.1f)",
           cell, x, y, (double)sx, (double)sy);
    PostMessageW(gw, WM_APP + 16, 0, 0);
}

/* ---- full map reveal -----------------------------------------------------
 * The big map's "visited" data is not the bitmap the map screen draws from
 * (map+0x9F83C).  The save object keeps 16 room records at stats+0x17F8C,
 * stride 0x28; dword 0 of record [room] has one bit per zone (the cell x).
 * Measured live on 2026-10-23: warping to (10,5) set bit 10 of record 5,
 * warping to (14,6) set bit 14 of record 6, and the map screen's bitmap then
 * mirrored it.  With all 16 records at 0xFFFF the map draws the whole zone
 * and every drawn room accepts the cursor / a right-click warp.  The game
 * copies the records into the save stream, so a save made afterwards keeps
 * the map revealed in that save.  This is a one-shot write; nothing keeps
 * forcing it afterwards, so loading an older save restores that save's bits.
 */
#define MAP_STATS_ROOMS  0x17F8Cu
#define MAP_STATS_STRIDE 0x28u

static int map_reveal_apply(void)
{
    unsigned st;
    int i;
    st = stats_addr();
    if (!st || !readable((void *)(size_t)(st + MAP_STATS_ROOMS), 16 * MAP_STATS_STRIDE)) {
        logmsg("reveal: save object not readable, nothing changed");
        return 0;
    }
    for (i = 0; i < 16; i++) {
        unsigned *p = (unsigned *)(size_t)(st + MAP_STATS_ROOMS +
                                           (unsigned)i * MAP_STATS_STRIDE);
        *p = 0xFFFFu;
    }
    logmsg("reveal: all rooms of the zone marked visited");
    return 1;
}

/* ---- light every light source of the current room -------------------------
 * Measured live on 2026-10-25 (external read-only debugger first, then the
 * same fields written in game).  A room's placeable objects exist in two
 * layers:
 *
 *   durable per-room table (survives leaving and re-entering the room):
 *     hdr   = *(void **)(level + 0xBA94)      level = *(stats + 0x17F7C)
 *     count = *(unsigned *)hdr, arr = *(void **)(hdr + 4)
 *     entry = arr + i*0x20:  +0x08 template index, +0x18 on/off
 *     template = 0x1AED85C + idx*0xDC:  +0x00 name string, +0x44 type
 *     type 2 = torch / brazier, 5 and 6 = "luz" light, 1 = item, 16 = lever
 *
 *   live records rebuilt from that table by 0x427D80 on every room load:
 *     level + 0x21CC4 + i*0x34:  +0x14 on, +0x2C state
 *     count at level + 0x21CBC
 *
 * Lighting a torch by hand writes both layers.  Writing only the live record
 * looks right until the next room load; writing only the table takes effect
 * on the next load.  So both are written here, and only for entries that are
 * 0 (off) - a stored 2 already means "lit" and is left untouched.
 * The header is taken through level+0xBA94 (the pointer the game's own room
 * loader uses) instead of recomputing the static table base from cell coords.
 */
#define LVL_OFF_LIGHT_HDR   0x0000BA94u /* *(level+0xBA94) = per-room object table */
#define LIGHT_ENT_STRIDE    0x00000020u
#define LIGHT_ENT_TMPL      0x00000008u
#define LIGHT_ENT_ON        0x00000018u
#define LIGHT_TMPL_TABLE    0x001AED85Cu
#define LIGHT_TMPL_STRIDE   0x000000DCu
#define LIGHT_TMPL_TYPE     0x00000044u
#define LVL_OFF_OBJCNT      0x0021CBCu
#define LVL_OFF_OBJS        0x0021CC4u
#define LVL_OBJ_STRIDE      0x00000034u
#define LOBJ_OFF_ON         0x00000014u
#define LOBJ_OFF_STATE      0x0000002Cu

/* The game's own "light this lamp" action is the interaction dispatcher
 * 0x48D610: ecx = object record (level + 0x21CC4 + i*0x34), stack argument =
 * the player entity.  Its only caller (0x4459D4, the "press the interact key
 * on whatever is under the player" path) reaches it through the wrapper
 * 0x48F610 with exactly those two arguments, so calling the dispatcher itself
 * gives the plain per-object branch without the wrapper's follow-up.
 *
 * For template type 2 that branch does, in this order:
 *   - table entry +0x18 = 1 and record +0x14 = 1   (the switch we used to write)
 *   - calls 0x42BDB0, "is every type-2 object in the room on?"
 *   - when that returns 1: for every type-2 entry, 0x4416B0 (effect id 0x32)
 *     at x = entry[0]*48+24, y = entry[1]*80+48, then one record into the
 *     16-slot queue 0x437FB0   (this is the ignition effect + its sound)
 * So calling it per lamp reproduces the native effect instead of faking it.
 *
 * 0x4416B0's "this" is *(void **)0x1477084, measured live as "level + 0xA788"
 * (the room's own effect list).  It is NULL while the game is not running in a
 * foreground room and the game double-buffers rooms, so the dispatcher must
 * only be called when that pointer is non-NULL and equal to the current level
 * + 0xA788 - otherwise the game's own effect block would run with a NULL this.
 * That is why the work is queued (WM_APP+20) and retried while the request is
 * pending: clicking a panel button takes the game out of the foreground.
 */
static volatile LONG g_light_req;
static int g_light_stale;

static void light_post(void)
{
    HWND gw = find_game_window();
    if (gw) PostMessageW(gw, WM_APP + 20, 0, 0);
}

/* panel / hook thread: queue the work, never touch the game here.
 * all = 0 -> this room, 1 -> every room table of the current zone */
static int light_request(int all)
{
    unsigned st, lvl;
    st = stats_addr();
    lvl = st ? *(unsigned *)(size_t)(st + OFF_LEVEL_OBJ) : 0;
    if (!lvl || !readable((void *)(size_t)(lvl + LVL_OFF_LIGHT_HDR), 4)) return 0;
    g_light_req = all ? 2 : 1;
    light_post();
    logmsg("lights: request queued (%s, level %08x)", all ? "all rooms" : "this room", lvl);
    return 1;
}

/* game thread only: light the room through the game's own interaction */
static int room_lights_apply(unsigned lvl, unsigned st)
{
    typedef void (__attribute__((thiscall)) *fx_t)(void *ctx, int x, int y);
    unsigned hdr, arr, cnt, live, ctx;
    int i, lit = 0, puffs = 0, nxy = 0;
    int xy[64][2];

    if (!lvl || !st || !readable((void *)(size_t)(lvl + LVL_OFF_LIGHT_HDR), 4)) return -1;
    hdr = *(unsigned *)(size_t)(lvl + LVL_OFF_LIGHT_HDR);
    if (!hdr || !readable((void *)(size_t)hdr, 8)) return -1;
    cnt = *(unsigned *)(size_t)hdr;
    arr = *(unsigned *)(size_t)(hdr + 4);
    if (!cnt || cnt > 64 || !arr || !readable((void *)(size_t)arr, cnt * LIGHT_ENT_STRIDE)) return -1;

    live = 0;
    if (readable((void *)(size_t)(lvl + LVL_OFF_OBJCNT), 4))
        live = *(unsigned *)(size_t)(lvl + LVL_OFF_OBJCNT);

    for (i = 0; i < (int)cnt; i++) {
        unsigned e = arr + (unsigned)i * LIGHT_ENT_STRIDE;
        unsigned tidx, tmpl, type, rec;

        if (!readable((void *)(size_t)(e + LIGHT_ENT_ON), 4)) continue;
        tidx = *(unsigned *)(size_t)(e + LIGHT_ENT_TMPL);
        if (tidx > 1024u) continue;
        tmpl = LIGHT_TMPL_TABLE + tidx * LIGHT_TMPL_STRIDE;
        if (!readable((void *)(size_t)(tmpl + LIGHT_TMPL_TYPE), 4)) continue;
        type = *(unsigned *)(size_t)(tmpl + LIGHT_TMPL_TYPE);
        if (type != 2u && type != 5u && type != 6u) continue;

        /* the ignition puff goes to every lamp of the room, like the game's own
         * all-lights-on branch, so collect the positions even for lamps that
         * were already on */
        if (type == 2u && (unsigned)i < live && (unsigned)i < 64u && nxy < 64) {
            rec = lvl + LVL_OFF_OBJS + (unsigned)i * LVL_OBJ_STRIDE;
            if (readable((void *)(size_t)(rec + LOBJ_OFF_STATE), 4)) {
                xy[nxy][0] = (int)*(unsigned *)(size_t)(rec + 0x0C);
                xy[nxy][1] = (int)*(unsigned *)(size_t)(rec + 0x10) - 0x20;
                nxy++;
            }
        }
        if (*(unsigned *)(size_t)(e + LIGHT_ENT_ON) != 0u) continue;

        /* the two flags the game itself writes when a lamp is lit */
        *(unsigned *)(size_t)(e + LIGHT_ENT_ON) = 1u;
        if ((unsigned)i < live && (unsigned)i < 64u) {
            rec = lvl + LVL_OFF_OBJS + (unsigned)i * LVL_OBJ_STRIDE;
            if (readable((void *)(size_t)(rec + LOBJ_OFF_STATE), 4)) {
                *(unsigned *)(size_t)(rec + LOBJ_OFF_ON) = 1u;
                *(unsigned *)(size_t)(rec + LOBJ_OFF_STATE) = 1u;
            }
        }
        lit++;
    }

    /* Same thing the game's scripted "light every lamp" path does
     * (0x450DE5): push one effect command per lamp with this = level+0xA788.
     * That object is part of the level struct (arrays at +0, +0x900, +0x1200,
     * count at +0x1300, capacity 0x3F); the game consumes it on its next frame,
     * so this is safe from the message loop and simply plays when it resumes. */
    ctx = lvl + 0x0000A788u;
    if (lit > 0 && nxy > 0 && readable((void *)(size_t)(ctx + 0x1300u), 4)) {
        for (i = 0; i < nxy; i++) {
            unsigned c = *(unsigned *)(size_t)(ctx + 0x1300u);
            if (c >= 0x3Eu) break;              /* leave room in the 63-slot buffer */
            ((fx_t)0x004416B0u)((void *)(size_t)ctx, xy[i][0], xy[i][1]);
            puffs++;
        }
    }
    logmsg("lights: level %08x count %u live %u -> %d lit, %d effect command(s)",
           lvl, cnt, live, lit, puffs);
    return lit;
}

/* Every room of the current zone keeps its object table in the static array at
 * 0xAFDFE0, stride 0x50 (the room loader 0x427D80 derives the per-room pointer
 * from cell coords and stores it at level+0xBA94).  Measured live: 205 of the
 * 256 slots are populated, each with count/array and a resolvable template, so
 * the whole zone can be lit in one pass.  Only the durable table is written
 * here; the current room goes through room_lights_apply() first so its live
 * records and effect commands are handled too.  Entries whose array or
 * template cannot be validated are skipped.
 */
#define ADDR_LIGHT_TABLES 0x00AFDFE0u
#define LIGHT_TABLE_STRIDE 0x00000050u

static int room_lights_all_rooms(unsigned lvl, unsigned st)
{
    unsigned i, k;
    int here, rooms = 0, lamps = 0;

    here = room_lights_apply(lvl, st);          /* current room first */

    if (!readable((void *)(size_t)ADDR_LIGHT_TABLES, 256 * LIGHT_TABLE_STRIDE)) {
        logmsg("lights: room table array not readable");
        return here;
    }
    for (i = 0; i < 256; i++) {
        unsigned hdr = ADDR_LIGHT_TABLES + i * LIGHT_TABLE_STRIDE;
        unsigned cnt = *(unsigned *)(size_t)hdr;
        unsigned arr = *(unsigned *)(size_t)(hdr + 4);
        int hit = 0;

        if (cnt == 0 || cnt > 64) continue;
        if (!arr || !readable((void *)(size_t)arr, cnt * LIGHT_ENT_STRIDE)) continue;
        for (k = 0; k < cnt; k++) {
            unsigned e = arr + k * LIGHT_ENT_STRIDE;
            unsigned tidx = *(unsigned *)(size_t)(e + LIGHT_ENT_TMPL);
            unsigned tmpl, type;

            if (tidx > 1024u) continue;
            tmpl = LIGHT_TMPL_TABLE + tidx * LIGHT_TMPL_STRIDE;
            if (!readable((void *)(size_t)(tmpl + LIGHT_TMPL_TYPE), 4)) continue;
            type = *(unsigned *)(size_t)(tmpl + LIGHT_TMPL_TYPE);
            if (type != 2u && type != 5u && type != 6u) continue;
            if (*(unsigned *)(size_t)(e + LIGHT_ENT_ON) != 0u) continue;
            *(unsigned *)(size_t)(e + LIGHT_ENT_ON) = 1u;
            lamps++;
            hit = 1;
        }
        if (hit) rooms++;
    }
    logmsg("lights: all rooms (zone tables) -> %d room(s), %d lamp(s) (current room handled separately: %d)",
           rooms, lamps, here);
    return lamps + (here > 0 ? here : 0);
}

/* ---- unlock every co-op map ------------------------------------------------
 * The multiplayer "Official maps" list is a chain of single-player dungeons,
 * each unlocked when the previous one was cleared.  The state lives in a map
 * table in the game:
 *
 *     table = *(unsigned *)0x00EE29E4;              // 0x146A980 live
 *     count = *(unsigned *)(table + 0x04);          // 51
 *     flag  = *(unsigned *)(table + 0x268 + idx*8); // != 0 -> unlocked
 *
 * Found 2026-10-24 with a read watchpoint on an entry's prerequisite id: the
 * info panel at 0x47A794 loads the entry's own id, calls the game's own id
 * lookup (0x4406F0) and tests exactly this flag at 0x47A7CB.  idx is assigned
 * by that lookup, so writing the flag half of every record unlocks the whole
 * list without needing to know the ids.  The table is rebuilt from the save on
 * load, so the button writes it live and the player has to save afterwards. */
#define ADDR_MP_MAPTABLE 0x00EE29E4u
#define ADDR_MP_SLOT     0x0017CAA90u   /* the save slot the menu has selected */
#define ADDR_ACTOR_POOL  0x0146BDF4u    /* the map table ends right before this */
#define MP_MAP_MODE_OFF  0x000u         /* +0: 0 = game_N.sav, 1 = game_mp_N.sav */
#define MP_MAP_MODE_MP   1u
#define MP_MAP_PRESENT   0x190u         /* +0x190: per-slot "save exists" flag[9] */
#define MP_MAP_FLAGS_OFF 0x268u
#define MP_MAP_SLOT_RECS 64u

/* The multiplayer lobby's menu object (static, built by 0x471ED0).  Its +0xC
 * is the screen state; measured with the user's own flow on 2026-10-24:
 *   step 1 (mode)      0x3D
 *   step 2 (character) 0x29
 *   step 3 (scene)     0x97  (0x47 once the map list/description is open)
 * The field persists across a step back in other places (the map slot does
 * not reset), so the state has to be part of the gate: without it the button
 * stayed lit after returning from the scene step. */
#define ADDR_MP_MENU     0x0017CA610u
#define MP_MENU_STATE    0x0000000Cu
#define MP_MENU_STEP3_A  0x97u
#define MP_MENU_STEP3_B  0x47u

static unsigned mp_menu_state(void)
{
    if (IsBadReadPtr((void *)ADDR_MP_MENU, 0x10)) return ~0u;
    if (*(unsigned *)ADDR_MP_MENU != 1) return ~0u;    /* menu open */
    return *(unsigned *)(size_t)(ADDR_MP_MENU + MP_MENU_STATE);
}

static int mp_scene_step(void)
{
    unsigned m = mp_menu_state();
    return m == MP_MENU_STEP3_A || m == MP_MENU_STEP3_B;
}

/* The table is the save-slot cache the multiplayer map list is drawn from.
 * Its initialiser 0x43FE00 is given a namespace (string pair at 0x52A674 /
 * 0x52A688: "save\game_%d.sav" / "save\game_mp_%d.sav") which it stores at
 * +0, and it fills one 64-record block per slot.  This is the reliable "the
 * multiplayer data is what is loaded" signal: while the game is still using
 * the single-player namespace +0 is 0, however the MP flag and 0x675174 look.
 * The first unlock flag is NOT such a signal -- a fresh MP save has all of
 * them zero (only map 0 is free), which is exactly the state the list shows
 * at the scene step.  What proves the slot itself is usable is the game's own
 * per-slot "save exists" flag at +0x190, tested at 0x4680F5 and set after a
 * save at 0x409099. */
static unsigned mp_table_mp(void)
{
    unsigned t;
    if (!game_is_mp()) return 0;
    if (IsBadReadPtr((void *)ADDR_MP_MAPTABLE, 4)) return 0;
    t = *(unsigned *)(size_t)ADDR_MP_MAPTABLE;
    if (!t || IsBadReadPtr((void *)(size_t)t, 8)) return 0;
    if (*(unsigned *)(size_t)(t + MP_MAP_MODE_OFF) != MP_MAP_MODE_MP) return 0;
    return t;
}

static int mp_slot_ready(unsigned t, unsigned *slot_out)
{
    unsigned slots, slot;
    if (IsBadReadPtr((void *)ADDR_MP_SLOT, 4)) return 0;
    slot = *(unsigned *)ADDR_MP_SLOT;
    slots = (0x1468u - MP_MAP_FLAGS_OFF) / (MP_MAP_SLOT_RECS * 8);
    if (slot >= slots) return 0;
    if (IsBadReadPtr((void *)(size_t)(t + MP_MAP_PRESENT + slot * 4), 4)) return 0;
    if (*(unsigned *)(size_t)(t + MP_MAP_PRESENT + slot * 4) == 0) return 0;
    /* The selected slot is also the slot the game will save to; if the two
     * globals disagree we do not know which one the list is showing. */
    if (IsBadReadPtr((void *)0x00675174u, 4)) return 0;
    if (*(unsigned *)0x00675174u != slot) return 0;
    if (slot_out) *slot_out = slot;
    return 1;
}

static int mp_unlock_state_ok(void)
{
    unsigned t;
    if (!mp_scene_step()) return 0;
    t = mp_table_mp();
    return t && mp_slot_ready(t, NULL);
}

static int mp_unlock_all_apply(void)
{
    unsigned t, count, slots, slot, i;
    if (!mp_scene_step()) {
        logmsg("mp unlock: refused, not on the scene step (menu state %u)",
               mp_menu_state());
        return 0;
    }
    t = mp_table_mp();
    if (!t) {
        logmsg("mp unlock: refused, no multiplayer save table is loaded");
        return 0;
    }
    count = *(unsigned *)(size_t)(t + 4);      /* map records, 51 live */
    if (count < 1 || count > MP_MAP_SLOT_RECS) {
        logmsg("mp unlock: implausible record count %u (table %08x)", count, t);
        return 0;
    }
    /* The object is 0x1468 bytes (initialiser 0x43FE00 pushes 0x1468):
     * 0x268 header + 9 slots x 64 records x 8 bytes.  Only the slot the game
     * has active is written; uninitialised slots must keep their zeros. */
    slots = (0x1468u - MP_MAP_FLAGS_OFF) / (MP_MAP_SLOT_RECS * 8);
    if (!mp_slot_ready(t, &slot)) {
        logmsg("mp unlock: refused, no multiplayer save is selected/loaded"
               " (slot %u, table %08x)", IsBadReadPtr((void *)ADDR_MP_SLOT, 4)
               ? 0xFFFFFFFFu : *(unsigned *)ADDR_MP_SLOT, t);
        return 0;
    }
    if (IsBadWritePtr((void *)(size_t)(t + MP_MAP_FLAGS_OFF + slot * MP_MAP_SLOT_RECS * 8),
                      count * 8)) {
        logmsg("mp unlock: slot %u flags at %08x not writable", slot,
               t + MP_MAP_FLAGS_OFF + slot * MP_MAP_SLOT_RECS * 8);
        return 0;
    }
    for (i = 0; i < count; i++)
        *(unsigned *)(size_t)(t + MP_MAP_FLAGS_OFF +
                              (slot * MP_MAP_SLOT_RECS + i) * 8) = 1;
    logmsg("mp unlock: slot %u of 0..%u, %u records flagged in table %08x",
           slot, slots - 1, count, t);
    return (int)count;
}

/* ---- player status effects (Buffs window) --------------------------------
 * Status effects live on the player entity in four parallel arrays (this is
 * also the save layout): count at +0x6B0, duration at +0x6B4, initial
 * duration at +0x86C, state at +0xA24, 110 slots.  The effect-name block in
 * memory is ordered by effect id starting at 1 (101 names), which is how the
 * names and ids below were classified; measured ids: 19 = total immunity,
 * 41 = slowed, 73 = frozen.
 *
 * Negative ids are the game's own room-entry clear list (0x004B4100);
 * positive ones are the rest of the name table minus three ambiguous
 * entries (62 skeletal, 64 gaining a level!, 100 Move out).  The Buffs window edits one
 * switch per id: a positive one is kept at PERMA_SECONDS, a negative one is
 * cleared every worker tick.  Selections are stored in mod\buffs.ini.
 */
typedef struct { unsigned char id; const wchar_t *name; } EffName;

static const EffName g_buff_list[] = {
    {9,   L"night vision"},
    {10,  L"\u6B66\u5668\u514D\u75AB"},
    {11,  L"\u706B\u7130\u514D\u75AB"},
    {12,  L"\u51B0\u971C\u514D\u75AB"},
    {13,  L"\u4EA1\u7075\u514D\u75AB"},
    {14,  L"\u53D8\u5F62\u514D\u75AB"},
    {15,  L"\u610F\u5FD7\u4FDD\u62A4"},
    {16,  L"\u6BD2\u7269\u514D\u75AB"},
    {17,  L"\u81EA\u7531\u884C\u52A8"},
    {18,  L"\u7535\u6D41\u514D\u75AB"},
    {19,  L"\u5B8C\u5168\u514D\u75AB"},
    {20,  L"\u6B66\u5668\u9632\u62A4"},
    {21,  L"\u706B\u7130\u9632\u62A4"},
    {22,  L"\u51B0\u971C\u9632\u62A4"},
    {23,  L"\u4EA1\u7075\u9632\u62A4"},
    {24,  L"\u7535\u6D41\u9632\u62A4"},
    {25,  L"undying magic"},
    {26,  L"enraged"},
    {27,  L"battle cry"},
    {28,  L"spirit arrows"},
    {29,  L"poisoned arrows"},
    {31,  L"split shot"},
    {33,  L"\u5315\u9996\u989D\u5916\u4F24\u5BB3"},
    {34,  L"\u5200\u5251\u989D\u5916\u4F24\u5BB3"},
    {35,  L"\u9489\u9524\u989D\u5916\u4F24\u5BB3"},
    {36,  L"\u65A7\u7C7B\u989D\u5916\u4F24\u5BB3"},
    {37,  L"\u957F\u67C4\u989D\u5916\u4F24\u5BB3"},
    {38,  L"\u5F13\u7BAD\u989D\u5916\u4F24\u5BB3"},
    {39,  L"parry wall"},
    {42,  L"general speed"},
    {45,  L"\u5FEB\u901F"},
    {46,  L"\u5F3A\u58EE"},
    {49,  L"\u591C\u89C6"},
    {50,  L"\u9677\u9631\u63A2\u6D4B"},
    {51,  L"\u9690\u8EAB"},
    {53,  L"\u51CF\u7F13\u6BD2\u6027"},
    {54,  L"\u770B\u5230\u9690\u5F62\u751F\u7269"},
    {55,  L"\u865A\u65E0\u7F25\u7F08"},
    {56,  L"\u518D\u751F"},
    {57,  L"\u6E29\u70ED\u7075\u6C14"},
    {58,  L"\u6BD2\u6027\u51CF\u5F31"},
    {59,  L"\u5438\u8840\u7075\u6C14"},
    {60,  L"\u5723\u5149\u7075\u6C14"},
    {63,  L"\u514D\u75AB"},
    {69,  L"\u6B66\u5668\u7CBE\u901A"},
    {70,  L"\u5F13\u7BAD\u7CBE\u901A"},
    {74,  L"\u63A2\u6D4B\u9677\u9631\u548C\u6697\u95E8"},
    {78,  L"\u806A\u6167"},
    {80,  L"\u9694\u7A7A\u53D6\u7269"},
    {81,  L"\u53CD\u5F39"},
    {94,  L"\u70C8\u98CE\u5E7B\u5F71"},
    {95,  L"\u7206\u88C2\u7BAD"},
    {96,  L"\u95EA\u7535\u51B2\u51FB"},
    {97,  L"\u529B\u5288\u534E\u5C71"},
    {98,  L"\u7535\u5BB9\u5668"},
    {101, L"Critical strikes"}
};
#define NBUFFS ((int)(sizeof g_buff_list / sizeof g_buff_list[0]))

static const EffName g_debuff_list[] = {
    {1,   L"\u4E2D\u6BD2 1"}, {2, L"\u4E2D\u6BD2 2"}, {3, L"\u4E2D\u6BD2 3"}, {4, L"\u4E2D\u6BD2 4"},
    {5,   L"\u8150\u8680 1"}, {6, L"\u8150\u8680 2"}, {7, L"\u8150\u8680 3"}, {8, L"\u8150\u8680 4"},
    {30,  L"\u88AB\u53D8\u5F62"},
    {32,  L"\u7761\u7720"},
    {40,  L"\u9EBB\u75F9"},
    {41,  L"\u51CF\u901F"},
    {43,  L"lycanthropy"},
    {44,  L"\u7729\u6655"},
    {47,  L"\u8FF7\u60D1"},
    {48,  L"\u6050\u60E7"},
    {52,  L"\u5931\u660E"},
    {66,  L"\u865A\u5F31"},
    {68,  L"\u75DB\u82E6\u4E2D"},
    {72,  L"\u71C3\u70E7"},
    {73,  L"\u51BB\u7ED3"},
    {83,  L"\u9EC4\u6CB9\u624B"},
    {85,  L"\u88AB\u675F\u7F1A"},
    {86,  L"\u5BFB\u6B7B"},
    {87,  L"\u5F71\u54CD\u610F\u5FD7"},
    {88,  L"\u7CBE\u795E\u5D29\u6E83"},
    {89,  L"\u7F04\u9ED8"},
    {90,  L"\u7CBE\u795E\u63A7\u5236"},
    {91,  L"(\u672A\u547D\u540D)"},
    {93,  L"\u6DF7\u4E71"}
};
#define NDEBUFFS ((int)(sizeof g_debuff_list / sizeof g_debuff_list[0]))

#define PERMA_SECONDS      99999.0f
#define OWN_EFFECT_SECONDS 3000.0f   /* anything above this was set by us */

/* Effect names come from the game's own dictio_effectName_* dictionaries,
 * which we dumped to mod\dict\effectName_<code>.tsv (index = effect id).
 * A missing or placeholder entry keeps the built-in label below. */
static int g_effect_lang;
static wchar_t g_effect_names[110][NAME_MAX];

static void effect_names_load(int li)
{
    char path[MAX_PATH], line[512];
    FILE *f;
    int i;

    for (i = 0; i < 110; i++) g_effect_names[i][0] = 0;
    if (li < 0 || li >= LANG_INTERNAL) return;
    _snprintf(path, sizeof path, "%s\\mod\\dict\\effectName_%s.tsv",
              g_dir, g_lang_codes[li]);
    f = fopen(path, "rb");
    if (!f) { logmsg("buffs: no %s - built-in names", path); return; }
    while (fgets(line, sizeof line, f)) {
        char *tab, *v, *bar;
        int idx, len;
        if ((unsigned char)line[0] == 0xEF && (unsigned char)line[1] == 0xBB &&
            (unsigned char)line[2] == 0xBF)
            memmove(line, line + 3, strlen(line + 3) + 1);
        idx = atoi(line);
        if (idx <= 0 || idx >= 110) continue;
        tab = strchr(line, '\t');
        if (!tab) continue;
        v = tab + 1;
        len = (int)strlen(v);
        while (len && (v[len - 1] == '\n' || v[len - 1] == '\r')) v[--len] = 0;
        bar = strchr(v, '|');                /* item names carry "||tag" */
        if (bar) *bar = 0;
        utf8w(v, g_effect_names[idx], NAME_MAX);
        if (!name_usable(g_effect_names[idx])) g_effect_names[idx][0] = 0;
    }
    fclose(f);
    logmsg("buffs: effect names loaded for lang=%s", g_lang_codes[li]);
}

static const wchar_t *effect_label(unsigned id, const wchar_t *builtin)
{
    return (id < 110 && g_effect_names[id][0]) ? g_effect_names[id] : builtin;
}

static int effect_lang_default(void)
{
    char p[MAX_PATH], code[16];
    int i;
    cfg_path(p, sizeof p);
    GetPrivateProfileStringA("items", "lang", "en", code, sizeof code, p);
    for (i = 0; i < LANG_INTERNAL; i++)
        if (_stricmp(code, g_lang_codes[i]) == 0) return i;
    return 0;                                /* "en", same as items_load() */
}

/* +0xA24 is not just "state": for many effects it is the magnitude the item
 * descriptor carries at +0x13C and passes to the apply function 0x00446B50.
 * The spell descriptions confirm it: spirit arrows = 2 arrows, critical
 * strike = next 3 attacks, weapon mastery 1/2/3 = 2/4/6 stacks.  The values
 * below are the strongest the game itself ships (rings 33, titan strength
 * 200, regeneration 20, ...).  Leaving the slot at 0 is why the HUD showed
 * x0 and those buffs did nothing. */
static int effect_magnitude(unsigned id)
{
    switch (id) {
    case 20: case 21: case 22: case 23: case 24: return 33;  /* protections */
    case 28: return 2;                   /* spirit arrows: two arrows */
    case 29: return 10;                  /* poisoned arrows; retail = bow level */
    case 33: case 34: case 35: case 36: case 37: case 38: return 6;
    case 45: return 33;                  /* fast */
    case 46: return 200;                 /* titan strength */
    case 51: return 3;                   /* invisibility 3 */
    case 56: return 20;                  /* regeneration */
    case 57: return 10;                  /* warm aura */
    case 59: return 4;                   /* vampiric aura */
    case 60: return 25;                  /* divine light aura */
    case 69: case 70: return 1;          /* weapon / bow mastery */
    case 78: return 33;                  /* cleverness */
    case 94: return 10;                  /* sword storm */
    case 101: return 3;                  /* critical strikes: next 3 attacks */
    }
    return 0;
}

static void set_effect(unsigned ent, unsigned id, float seconds)
{
    *(float *)(size_t)(ent + 0x6B4 + id * 4) = seconds;
    *(float *)(size_t)(ent + 0x86C + id * 4) = seconds;
    *(int *)(size_t)(ent + 0xA24 + id * 4) = 0;
}

static void clear_effect(unsigned ent, unsigned id)
{
    *(float *)(size_t)(ent + 0x6B4 + id * 4) = 0.0f;
    *(float *)(size_t)(ent + 0x86C + id * 4) = 0.0f;
    *(int *)(size_t)(ent + 0xA24 + id * 4) = 0;
}

static void buffs_cfg_path(char *out, size_t n)
{
    char p[MAX_PATH];
    GetModuleFileNameA(NULL, p, MAX_PATH);
    {
        char *s = strrchr(p, '\\');
        if (s) *s = 0;
    }
    _snprintf(out, n, "%s\\mod\\buffs.ini", p);
    out[n - 1] = 0;
}

/* Defaults (also used when mod\buffs.ini does not exist yet): every positive
 * buff on, every negative one off. */
static void buffs_cfg_load(void)
{
    char p[MAX_PATH], key[8], code[16];
    int i;
    memset(g_buff_on, 0, sizeof g_buff_on);
    memset(g_debuff_on, 0, sizeof g_debuff_on);
    buffs_cfg_path(p, sizeof p);
    for (i = 0; i < NBUFFS; i++) {
        _snprintf(key, sizeof key, "%u", g_buff_list[i].id);
        g_buff_on[g_buff_list[i].id] =
            GetPrivateProfileIntA("positive", key, 1, p) ? 1 : 0;
    }
    for (i = 0; i < NDEBUFFS; i++) {
        _snprintf(key, sizeof key, "%u", g_debuff_list[i].id);
        g_debuff_on[g_debuff_list[i].id] =
            GetPrivateProfileIntA("debuff", key, 0, p) ? 1 : 0;
    }
    /* the effect-name language starts as the item language, then takes
     * whatever was picked in the Buffs window last time */
    g_effect_lang = effect_lang_default();
    GetPrivateProfileStringA("ui", "lang", "", code, sizeof code, p);
    for (i = 0; i < LANG_INTERNAL; i++)
        if (_stricmp(code, g_lang_codes[i]) == 0) { g_effect_lang = i; break; }
    effect_names_load(g_effect_lang);
}

/* One pass, one file write.  WritePrivateProfileStringA rewrites the whole
 * file for every key: 87 keys measured 236 ms, and this runs on the thread
 * that also serves the low-level keyboard/mouse hooks, so the whole system
 * stops delivering input while it runs.  The reader still uses
 * GetPrivateProfileIntA, which parses what we write here unchanged. */
static void buffs_cfg_save(void)
{
    char p[MAX_PATH], buf[2048];
    size_t n = 0;
    int i, r;
    FILE *f;

    buffs_cfg_path(p, sizeof p);
    r = _snprintf(buf + n, sizeof buf - n, "[positive]\r\n");
    if (r > 0) n += (size_t)r;
    for (i = 0; i < NBUFFS && n < sizeof buf - 16; i++) {
        unsigned id = g_buff_list[i].id;
        r = _snprintf(buf + n, sizeof buf - n, "%u=%d\r\n", id, g_buff_on[id] ? 1 : 0);
        if (r <= 0) break;
        n += (size_t)r;
    }
    r = _snprintf(buf + n, sizeof buf - n, "[debuff]\r\n");
    if (r > 0) n += (size_t)r;
    for (i = 0; i < NDEBUFFS && n < sizeof buf - 16; i++) {
        unsigned id = g_debuff_list[i].id;
        r = _snprintf(buf + n, sizeof buf - n, "%u=%d\r\n", id, g_debuff_on[id] ? 1 : 0);
        if (r <= 0) break;
        n += (size_t)r;
    }
    r = _snprintf(buf + n, sizeof buf - n, "[ui]\r\nlang=%s\r\n",
                  g_lang_codes[g_effect_lang]);
    if (r > 0) n += (size_t)r;

    f = fopen(p, "wb");
    if (!f) { logmsg("buffs: cannot write %s", p); return; }
    if (fwrite(buf, 1, n, f) != n) logmsg("buffs: short write to %s", p);
    fclose(f);
    f = fopen(p, "rb");                       /* prove the bytes landed */
    if (!f) { logmsg("buffs: %s vanished after write", p); return; }
    fseek(f, 0, SEEK_END);
    if (ftell(f) < (long)n) logmsg("buffs: %s is %ld bytes, expected %u",
                                   p, ftell(f), (unsigned)n);
    fclose(f);
}

static void status_effects_tick(void)
{
    unsigned st, ent;
    float *dur, *snap;
    int i, applied = 0, cleared = 0;

    st = stats_addr();
    ent = st ? *(unsigned *)(size_t)(st + OFF_ENTITY) : 0;
    if (!ent || !readable((void *)(size_t)(ent + 0x6B4), 0x200)) return;
    dur = (float *)(size_t)(ent + 0x6B4);
    snap = (float *)(size_t)(ent + 0x86C);

    for (i = 0; i < NBUFFS; i++) {
        unsigned id = g_buff_list[i].id;
        int mag = effect_magnitude(id);
        if (g_buff_on[id]) {
            if (dur[id] < OWN_EFFECT_SECONDS) {
                set_effect(ent, id, PERMA_SECONDS);
                applied++;
            }
            /* keep the magnitude/charge count topped up: the game consumes
             * critical strikes and mastery stacks while fighting.  Only raise
             * it, so a stronger value the game set itself (e.g. poisoned
             * arrows = a high bow level) is not lowered. */
            if (mag && *(int *)(size_t)(ent + 0xA24 + id * 4) < mag) {
                *(int *)(size_t)(ent + 0xA24 + id * 4) = mag;
                applied++;
            }
        } else if (dur[id] > OWN_EFFECT_SECONDS) {
            /* god mode holds total immunity (19) every ~200 ms to keep the
             * instant-kill traps harmless; clearing it here made the icon blink
             * on and off.  Skip it while god is on - the checkbox cannot win
             * against a held effect anyway. */
            if (id == 19 && g_god) continue;
            clear_effect(ent, id);
            cleared++;
        }
    }
    for (i = 0; i < NDEBUFFS; i++) {
        unsigned id = g_debuff_list[i].id;
        if (g_debuff_on[id] && (dur[id] != 0.0f || snap[id] != 0.0f)) {
            clear_effect(ent, id);
            cleared++;
        }
    }
    if (applied || cleared) {
        int cnt = 0;
        for (i = 0; i < 110; i++)
            if (dur[i] != 0.0f) cnt++;
        *(int *)(size_t)(ent + 0x6B0) = cnt;
    }
}

/* ---- Buffs window --------------------------------------------------------
 * One checkbox per effect id: upper half keeps a positive effect at max
 * duration, lower half clears a negative one every worker tick.  The two
 * "All"/"None" buttons set a whole half.  A language combo switches the
 * labels to the game's own effectName dictionary.  Selections are stored in
 * mod\buffs.ini (section positive / debuff, key = effect id; [ui] lang).
 */
#define BF_W 990
#define BF_H 940
#define BF_ID_BUFF    1200
#define BF_ID_DEBUFF  2200
#define BF_ID_ALL_P   1300
#define BF_ID_NONE_P  1301
#define BF_ID_ALL_D   2300
#define BF_ID_NONE_D  2301
#define BF_ID_CLOSE   2400
#define BF_ID_LANG    2500

static HWND g_buffs;
static wchar_t g_buffs_cls[64];
/* The panel, both sub-windows and the low-level keyboard/mouse hooks all live
 * on this one thread, and the system waits for a low-level hook to return
 * before it delivers ANY input.  So nothing slow may happen inside a control
 * notification: mod\buffs.ini is written at most once a second from the timer
 * instead of once per click (All/None used to flush 87 profile keys inline). */
static int g_bcfg_dirty;
static DWORD g_bflush_tick;

static void buffs_close(void)
{
    if (g_bcfg_dirty) { buffs_cfg_save(); g_bcfg_dirty = 0; }
    if (g_buffs && IsWindow(g_buffs)) DestroyWindow(g_buffs);
}

static void buffs_apply_lang(HWND h)
{
    wchar_t txt[NAME_MAX + 24];
    int i;

    SendMessageW(h, WM_SETREDRAW, FALSE, 0);
    for (i = 0; i < NBUFFS; i++) {
        HWND cb = GetDlgItem(h, BF_ID_BUFF + i);
        if (!cb) continue;
        wsprintfW(txt, L"%u  %s", g_buff_list[i].id,
                  effect_label(g_buff_list[i].id, g_buff_list[i].name));
        SetWindowTextW(cb, txt);
    }
    for (i = 0; i < NDEBUFFS; i++) {
        HWND cb = GetDlgItem(h, BF_ID_DEBUFF + i);
        if (!cb) continue;
        wsprintfW(txt, L"%u  %s", g_debuff_list[i].id,
                  effect_label(g_debuff_list[i].id, g_debuff_list[i].name));
        SetWindowTextW(cb, txt);
    }
    SendMessageW(h, WM_SETREDRAW, TRUE, 0);
    RedrawWindow(h, NULL, NULL, RDW_ERASE | RDW_FRAME | RDW_INVALIDATE | RDW_ALLCHILDREN);
}

static LRESULT CALLBACK buffs_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_COMMAND: {
        int id = LOWORD(w);
        if (id == BF_ID_LANG && HIWORD(w) == CBN_SELCHANGE) {
            int sel = (int)SendMessageW((HWND)l, CB_GETCURSEL, 0, 0);
            if (sel >= 0 && sel < LANG_INTERNAL) {
                g_effect_lang = sel;
                effect_names_load(g_effect_lang);   /* one small file read */
                buffs_apply_lang(h);
                g_bcfg_dirty = 1;                   /* [ui] lang, flushed later */
                logmsg("buffs: language -> %s", g_lang_codes[g_effect_lang]);
            }
            return 0;
        }
        if (id >= BF_ID_BUFF && id < BF_ID_BUFF + NBUFFS) {
            int i = id - BF_ID_BUFF;
            unsigned eid = g_buff_list[i].id;
            g_buff_on[eid] = SendMessageW((HWND)l, BM_GETCHECK, 0, 0) == BST_CHECKED;
            g_bcfg_dirty = 1;
            status_effects_tick();
            return 0;
        }
        if (id >= BF_ID_DEBUFF && id < BF_ID_DEBUFF + NDEBUFFS) {
            int i = id - BF_ID_DEBUFF;
            unsigned eid = g_debuff_list[i].id;
            g_debuff_on[eid] = SendMessageW((HWND)l, BM_GETCHECK, 0, 0) == BST_CHECKED;
            g_bcfg_dirty = 1;
            status_effects_tick();
            return 0;
        }
        if (id == BF_ID_ALL_P || id == BF_ID_NONE_P ||
            id == BF_ID_ALL_D || id == BF_ID_NONE_D) {
            int on, i, pos = (id == BF_ID_ALL_P || id == BF_ID_NONE_P);
            on = (id == BF_ID_ALL_P || id == BF_ID_ALL_D);
            SendMessageW(h, WM_SETREDRAW, FALSE, 0);
            if (pos) {
                for (i = 0; i < NBUFFS; i++) {
                    g_buff_on[g_buff_list[i].id] = (unsigned char)on;
                    SendMessageW(GetDlgItem(h, BF_ID_BUFF + i), BM_SETCHECK,
                                 on ? BST_CHECKED : BST_UNCHECKED, 0);
                }
            } else {
                for (i = 0; i < NDEBUFFS; i++) {
                    g_debuff_on[g_debuff_list[i].id] = (unsigned char)on;
                    SendMessageW(GetDlgItem(h, BF_ID_DEBUFF + i), BM_SETCHECK,
                                 on ? BST_CHECKED : BST_UNCHECKED, 0);
                }
            }
            SendMessageW(h, WM_SETREDRAW, TRUE, 0);
            RedrawWindow(h, NULL, NULL,
                         RDW_ERASE | RDW_FRAME | RDW_INVALIDATE | RDW_ALLCHILDREN);
            g_bcfg_dirty = 1;
            status_effects_tick();
            return 0;
        }
        if (id == BF_ID_CLOSE) { buffs_close(); return 0; }
        return 0;
    }
    case WM_KEYDOWN:
        if (w == VK_ESCAPE) { buffs_close(); return 0; }
        break;
    case WM_CLOSE:
        buffs_close();
        return 0;
    case WM_DESTROY:
        g_buffs = NULL;
        if (g_hwnd) {
            EnableWindow(g_hwnd, TRUE);
            SetActiveWindow(g_hwnd);
        }
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static void buffs_show(void)
{
    WNDCLASSEXW wc;
    RECT r = { 0, 0, BF_W, BF_H }, pr;
    HINSTANCE hi = GetModuleHandleA(NULL);
    int i, col, row;

    if (g_buffs && IsWindow(g_buffs)) {
        ShowWindow(g_buffs, SW_SHOW);
        SetForegroundWindow(g_buffs);
        return;
    }
    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = buffs_proc;
    wc.hInstance = hi;
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wsprintfW(g_buffs_cls, L"UnepicBuffs%08X", (unsigned)(size_t)&buffs_proc);
    wc.lpszClassName = g_buffs_cls;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        logmsg("buffs: RegisterClassExW failed: %lu", GetLastError());
        return;
    }
    AdjustWindowRect(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE);
    g_buffs = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, g_buffs_cls,
                              L"Unepic Better Mod - buffs",
                              WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                              CW_USEDEFAULT, CW_USEDEFAULT,
                              r.right - r.left, r.bottom - r.top,
                              g_hwnd, NULL, hi, NULL);
    if (!g_buffs) { logmsg("buffs: CreateWindowExW failed: %lu", GetLastError()); return; }

    CreateWindowExW(0, L"BUTTON", L"Positive buffs (keep at max duration)",
                    WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 12, 4, BF_W - 24, 496,
                    g_buffs, NULL, hi, NULL);
    CreateWindowExW(0, L"BUTTON", L"All", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                    BF_W - 176, 10, 76, 24, g_buffs, (HMENU)BF_ID_ALL_P, hi, NULL);
    CreateWindowExW(0, L"BUTTON", L"None", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                    BF_W - 92, 10, 76, 24, g_buffs, (HMENU)BF_ID_NONE_P, hi, NULL);
    for (i = 0; i < NBUFFS; i++) {
        wchar_t txt[NAME_MAX + 24];
        col = i / 19;
        row = i % 19;
        wsprintfW(txt, L"%u  %s", g_buff_list[i].id,
                  effect_label(g_buff_list[i].id, g_buff_list[i].name));
        CreateWindowExW(0, L"BUTTON", txt, WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                        24 + col * 316, 40 + row * 23, 306, 22,
                        g_buffs, (HMENU)(BF_ID_BUFF + i), hi, NULL);
        SendMessageW(GetDlgItem(g_buffs, BF_ID_BUFF + i), BM_SETCHECK,
                     g_buff_on[g_buff_list[i].id] ? BST_CHECKED : BST_UNCHECKED, 0);
    }

    CreateWindowExW(0, L"BUTTON", L"No debuffs (clear these every 200 ms)",
                    WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 12, 504, BF_W - 24, 388,
                    g_buffs, NULL, hi, NULL);
    CreateWindowExW(0, L"BUTTON", L"All", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                    BF_W - 176, 510, 76, 24, g_buffs, (HMENU)BF_ID_ALL_D, hi, NULL);
    CreateWindowExW(0, L"BUTTON", L"None", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                    BF_W - 92, 510, 76, 24, g_buffs, (HMENU)BF_ID_NONE_D, hi, NULL);
    for (i = 0; i < NDEBUFFS; i++) {
        wchar_t txt[NAME_MAX + 24];
        col = i / 15;
        row = i % 15;
        wsprintfW(txt, L"%u  %s", g_debuff_list[i].id,
                  effect_label(g_debuff_list[i].id, g_debuff_list[i].name));
        CreateWindowExW(0, L"BUTTON", txt, WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                        24 + col * 460, 540 + row * 23, 450, 22,
                        g_buffs, (HMENU)(BF_ID_DEBUFF + i), hi, NULL);
        SendMessageW(GetDlgItem(g_buffs, BF_ID_DEBUFF + i), BM_SETCHECK,
                     g_debuff_on[g_debuff_list[i].id] ? BST_CHECKED : BST_UNCHECKED, 0);
    }
    CreateWindowExW(0, L"STATIC", L"Language", WS_CHILD | WS_VISIBLE,
                    20, 907, 70, 22, g_buffs, NULL, hi, NULL);
    {
        HWND cmb = CreateWindowExW(0, L"COMBOBOX", NULL,
                        WS_CHILD | WS_VISIBLE | WS_VSCROLL | CBS_DROPDOWNLIST,
                        96, 903, 260, 220, g_buffs,
                        (HMENU)BF_ID_LANG, hi, NULL);
        for (i = 0; i < LANG_INTERNAL; i++)
            SendMessageW(cmb, CB_ADDSTRING, 0, (LPARAM)g_lang_labels[i]);
        SendMessageW(cmb, CB_SETCURSEL, (WPARAM)g_effect_lang, 0);
    }
    CreateWindowExW(0, L"STATIC", L"names from the game's own dictionaries",
                    WS_CHILD | WS_VISIBLE, 372, 907, 340, 22,
                    g_buffs, NULL, hi, NULL);
    CreateWindowExW(0, L"BUTTON", L"Close", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                    BF_W - 124, 900, 100, 28, g_buffs, (HMENU)BF_ID_CLOSE, hi, NULL);
    EnumChildWindows(g_buffs, saves_font_cb, (LPARAM)g_ui_font);

    if (g_hwnd) {
        GetWindowRect(g_hwnd, &pr);
        GetWindowRect(g_buffs, &r);
        SetWindowPos(g_buffs, HWND_TOP,
                     pr.left + ((pr.right - pr.left) - (r.right - r.left)) / 2,
                     pr.top + ((pr.bottom - pr.top) - (r.bottom - r.top)) / 2,
                     0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
        EnableWindow(g_hwnd, FALSE);
    }
    ShowWindow(g_buffs, SW_SHOW);
    SetForegroundWindow(g_buffs);
}

static LRESULT CALLBACK mouse_proc(int code, WPARAM wp, LPARAM lp)
{
    if (code == HC_ACTION && wp == WM_RBUTTONDOWN) {
        int fg = is_game_foreground();
        MSLLHOOKSTRUCT *m = (MSLLHOOKSTRUCT *)lp;
        if (fg && map_is_open()) {
            /* the big map is up: right-click belongs to the map, never to the
             * in-game teleport - swallow it either way */
            (void)m;
            map_right_click();
            return 1;
        }
        if (g_teleport) {
            logmsg("mouse: rdown at %ld,%ld fg=%d", (LONG)m->pt.x, (LONG)m->pt.y, fg);
            if (fg) tp_go((int)m->pt.x, (int)m->pt.y);
        }
    }
    return CallNextHookEx(g_mouse, code, wp, lp);
}

/* Multiplayer position catch-up (docs/DEVELOPMENT.md section 13.20).
 *
 * The picture in MP is drawn from the net player object.  That object is
 * written by the game from a player-state snapshot only about 1.5 times per
 * second (measured 2026-10-24 with a write watchpoint: the only writer was a
 * block memcpy from 0x1248D920, i.e. a snapshot), so a teleport, an air jump
 * or the Move speed multiplier shows up in the game state but not on screen.
 * Copying x in at ~60 Hz keeps horizontal speed visible; y is copied only in
 * airborne states (101/102/103) so the air jump still shows, never on a ladder
 * (state 104 / +-100 units per second) where it fought the game's own climb
 * and bounced the player to the top (2026-10-24).
 * Runs multiplayer only and exits with g_stopping. */
static DWORD WINAPI net_sync_thread(LPVOID p)
{
    int writes = 0;
    LONG ladder_until = 0;
    (void)p;
    while (!g_stopping) {
        if (!g_netsync_on) { Sleep(150); continue; }
        if (game_is_mp()) {
            unsigned e = entity_addr();
            unsigned o = net_player_obj();
            /* x is synced for horizontal Move speed (the game itself rewrites
             * the net object from a snapshot only ~1.5 times per second); y is
             * synced only while airborne (101/102/103), which is what makes the
             * air jump visible.  Nothing at all is written in ladder state 104
             * (and for 600 ms afterwards), and the +-100 units/s entry frame
             * itself is skipped: any write there fought the game's own
             * climb/entry and bounced the player back to the top, which made
             * descending need several presses. */
            if (e && o && readable((void *)(size_t)(e + 0x520), 8) &&
                !IsBadWritePtr((void *)(size_t)(o + 0x1C), 8)) {
                float ex = *(float *)(size_t)(e + 0x520);
                float ey = *(float *)(size_t)(e + 0x524);
                float nx = *(float *)(size_t)(o + 0x1C);
                float ny = *(float *)(size_t)(o + 0x20);
                LONG now = (LONG)GetTickCount();
                LONG st = readable((void *)(size_t)(e + 0x540), 4)
                          ? *(LONG *)(size_t)(e + 0x540) : 0;
                float vy = readable((void *)(size_t)(e + 0x528), 4)
                           ? *(float *)(size_t)(e + 0x528) : 0.0f;
                int ladder_state = st == 104;
                int ladder_speed = (vy > 99.5f && vy < 100.5f) ||
                                   (vy < -99.5f && vy > -100.5f);
                int airborne = st == 101 || st == 102 || st == 103;
                /* Only the real ladder state arms the 600 ms quiet period.
                 * A normal fall passes through |vy| = 100 for a frame; using
                 * that to arm 600 ms would freeze the sync for most of the
                 * fall.  The frame itself is still skipped. */
                if (ladder_state) ladder_until = now + 600;
                if (!ladder_speed && (LONG)(now - ladder_until) >= 0) {
                    if (ex != nx) {
                        *(float *)(size_t)(o + 0x1C) = ex;
                        writes++;
                    }
                    if (airborne && ey != ny) {
                        *(float *)(size_t)(o + 0x20) = ey;
                        writes++;
                    }
                }
            }
        }
        Sleep(16);
    }
    if (writes) logmsg("netsync: stopped after %d catch-up writes", writes);
    return 0;
}

/* God mode has to cover the instant-kill traps as well (spikes and the like).
 * They do not go through entity::damage_int - measured 2026-10-24: HP 330 -> 0
 * and entity state -> 118 in the same tick, followed by a respawn - so there is
 * no damage call to block.  Instead this keeps the last normal position and,
 * only while god is on, undoes the death the moment it appears: HP back to
 * max, state back to 0 and the player back where they stood.  It follows the
 * god checkbox live and costs nothing when the checkbox is off. */
/* Multiplayer sometimes throws the airjump impulse away: 200-300 ms after the
 * press the entity is still in a falling state and vy is back at +400.  For a
 * short window after each press, if the entity is airborne but no longer
 * rising, put the impulse back (at most three times per press). */
static void airjump_retry_tick(void)
{
    LONG now = (LONG)GetTickCount();
    unsigned e;
    LONG st;
    float vy;
    if (g_aj_retry_left <= 0) return;
    if ((LONG)(now - g_aj_retry_until) >= 0) { g_aj_retry_left = 0; return; }
    e = entity_addr();
    if (!e) return;
    st = readable((void *)(size_t)(e + 0x540), 4)
         ? *(LONG *)(size_t)(e + 0x540) : 0;
    vy = readable((void *)(size_t)(e + 0x528), 4)
         ? *(float *)(size_t)(e + 0x528) : 0.0f;
    if ((st == 101 || st == 102 || st == 103) && vy > 0.0f) {
        *(float *)(size_t)(e + 0x528) = AIRJUMP_IMPULSE;
        *(LONG *)(size_t)(e + 0x540) = 103;
        g_aj_retry_left--;
        logmsg("airjump: impulse restored (vy %.1f state %ld, %ld left)",
               (double)vy, st, (long)g_aj_retry_left);
    }
}

static DWORD WINAPI god_guard_thread(LPVOID p)
{
    int fixes = 0, eff_div = 0, eff_set = 0;
    (void)p;
    while (!g_stopping) {
        unsigned e;
        int retrying;
        airjump_retry_tick();
        retrying = g_aj_retry_left > 0 &&
                   (LONG)(GetTickCount() - g_aj_retry_until) < 0;
        if (!g_god) {
            if (eff_set) {
                unsigned ce = entity_addr();
                if (ce && readable((void *)(size_t)(ce + 0x6B4), 4)) {
                    *(float *)(size_t)(ce + 0x6B4 + 19 * 4) = 0.0f;
                    *(float *)(size_t)(ce + 0x86C + 19 * 4) = 0.0f;
                }
                eff_set = 0;
                logmsg("godguard: total-immunity effect released");
            }
            Sleep(retrying ? 8 : 120);
            continue;
        }
        e = entity_addr();
        if (e && readable((void *)(size_t)(e + 0x6B4), 4)) {
            /* effect 19 = "total immunity" (data/dict/effectName_en.tsv).  The
             * game itself decides what that covers, so if it includes the
             * instant-kill traps nothing has to be patched.  Refreshed every
             * ~200 ms; the slots are duration +0x6B4, initial +0x86C, state
             * +0xA24, stride 4, 110 slots (see section 5.12). */
            if (++eff_div >= 50) {
                eff_div = 0;
                *(float *)(size_t)(e + 0x6B4 + 19 * 4) = 99999.0f;
                *(float *)(size_t)(e + 0x86C + 19 * 4) = 99999.0f;
                *(int *)(size_t)(e + 0xA24 + 19 * 4) = 0;
                if (!eff_set) {
                    logmsg("godguard: total-immunity effect (19) held while god is on");
                    eff_set = 1;
                }
            }
        }
        if (e && readable((void *)(size_t)(e + ENT_MAXHP), 8) &&
            readable((void *)(size_t)(e + 0x520), 8)) {
            int mx = *(int *)(size_t)(e + ENT_MAXHP);
            float hp = *(float *)(size_t)(e + ENT_CURHP);
            LONG st = readable((void *)(size_t)(e + 0x540), 4)
                      ? *(LONG *)(size_t)(e + 0x540) : 0;
            if (st == 118 || hp <= 0.0f) {
                if (mx > 0) *(float *)(size_t)(e + ENT_CURHP) = (float)mx;
                *(LONG *)(size_t)(e + 0x540) = 0;
                fixes++;
                if (fixes == 1 || (fixes % 50) == 0)
                    logmsg("godguard: instant-death trap undone (#%d, state %ld, hp %.1f)",
                           fixes, st, (double)hp);
            }
        }
        Sleep(4);
    }
    if (fixes) logmsg("godguard: %d trap(s) undone in total", fixes);
    return 0;
}

/* Multiplayer: the picture is drawn from the net player object (section 13.20), so
 * the jump writes the game's jump state 103 and god_guard_thread keeps a short
 * retry window for impulses the snapshot throws away (see airjump_retry_tick).
 * The net_sync_thread above mirrors the y only in airborne states. */
static LRESULT CALLBACK kbd_proc(int code, WPARAM wp, LPARAM lp)
{
    if (code == HC_ACTION && (wp == WM_KEYUP || wp == WM_SYSKEYUP)) {
        KBDLLHOOKSTRUCT *k = (KBDLLHOOKSTRUCT *)lp;
        if (g_jump_vk && (LONG)k->vkCode == g_jump_vk) g_jump_down = 0;
        return CallNextHookEx(g_kbd, code, wp, lp);
    }
    if (code == HC_ACTION && (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN)) {
        KBDLLHOOKSTRUCT *k = (KBDLLHOOKSTRUCT *)lp;
        if (is_game_foreground()) {
            g_last_vk = (LONG)k->vkCode;
            g_last_vk_tick = (LONG)GetTickCount();
            if (g_airjump && g_jump_vk && (LONG)k->vkCode == g_jump_vk) {
                if (!g_jump_down) {                 /* one press = one jump */
                    unsigned e = entity_addr();
                    g_jump_down = 1;
                    g_aj_retry_until = (LONG)GetTickCount() + 300;
                    g_aj_retry_left = 3;
                    if (e) {
                        float vy = *(float *)(e + 0x528);
                        LONG st = *(LONG *)(e + 0x540);
                        int on_floor = 1;
                        /* +0x540 == 0 does NOT mean "on the ground": the live log
                         * has state 0 presses while the player was falling, and
                         * those were the taps that did nothing.  Ask the room
                         * grid instead - a cell is the floor when it is solid and
                         * the body fits above it (one cell of tolerance for the
                         * y rounding at the boundary). */
                        if (grid_load()) {
                            float gx = *(float *)(e + 0x520);
                            float gy = *(float *)(e + 0x524);
                            int gcx = (int)(gx / GRID_CELL);
                            int gfy = (int)(gy / GRID_CELL);
                            on_floor = grid_standable(gcx, gfy) ||
                                       grid_standable(gcx, gfy + 1);
                        }
                        if (st == 0 && on_floor) {
                            /* standing on the ground: the game's own jump runs from
                             * this same keypress (vy 400 -> -339..-368, state 0 -> 103).
                             * Do not fight it - forcing the impulse here would also
                             * run while it is still in the pre-jump frame. */
                            logmsg("airjump: on the ground (state 0, floor below),"
                                   " left to the game");
                        } else {
                            /* airborne OR falling with state 0: refresh the whole
                             * impulse on every press, no matter whether we are still
                             * rising.  The old gate (only when vy > -60) swallowed
                             * presses during the ascent, which is why only frantic
                             * mashing worked. */
                            g_aj_probe_ent = (LONG)e;
                            g_aj_probe_y0 = *(LONG *)(e + 0x524);
                            g_aj_probe_vy0 = *(LONG *)(e + 0x528);
                            *(float *)(e + 0x528) = AIRJUMP_IMPULSE;
                            /* The impulse alone is not enough in multiplayer:
                             * the game kept the falling state 102 and restored
                             * vy 400 within 200-300 ms (probe: dy +90 while
                             * the airjump log had just written -368).  The
                             * game's own jump ends in state 103; set the same
                             * state so the physics treats this as a jump. */
                            if (readable((void *)(size_t)(e + 0x540), 4))
                                *(LONG *)(size_t)(e + 0x540) = 103;
                            g_aj_probe_t0 = (LONG)GetTickCount();
                            logmsg("airjump: vy %.1f -> %.1f (state %ld -> 103, floor %d)",
                                   (double)vy, (double)AIRJUMP_IMPULSE, st, on_floor);
                        }
                    }
                }
            }
        }
        if (k->vkCode == VK_OEM_3 && is_game_foreground()) {
            logmsg("hotkey tap");
            /* child windows behave like the saves window: the first press
             * closes the child, only the next one touches the panel - the
             * panel must never be hidden while a child is still open. */
            if (g_saves && IsWindow(g_saves)) {
                saves_close();
                return CallNextHookEx(g_kbd, code, wp, lp);
            }
            if (g_buffs && IsWindow(g_buffs)) {
                buffs_close();
                return CallNextHookEx(g_kbd, code, wp, lp);
            }
            if (g_panel_visible) panel_hide(); else panel_show();
            logmsg("panel %s", g_panel_visible ? "shown" : "hidden");
        }
        /* Light all from inside the game.  This is the path that can also play
         * the native ignition puff: the room's effect list (level+0xA788) only
         * exists while the game is actually drawing the room, and clicking a
         * panel button takes that away.  F9 is free in the game (its quick bar
         * uses F1..F8).  The check is against the real game window, not just
         * "a window of this process" - the panel lives in the same process. */
        if (k->vkCode == VK_F9 && GetForegroundWindow() == find_game_window()) {
            if (light_request(0))
                logmsg("hotkey: light all requested");
        }
    }
    return CallNextHookEx(g_kbd, code, wp, lp);
}

/* ---- map/room watch (change-only diagnostic) -----------------------------
 * NOTE: [ent+0x1038] is the player's CONTAINER (76 x 172-byte item records),
 * not the room - the earlier comment here called it "the loaded room" and that
 * was wrong (see section 13.16).  The room object is [ent+0x684].  This watcher only
 * reports changes, so the fields below are read as-is for the log line.
 */
static unsigned g_last_room, g_last_room_a, g_last_room_b;

static void map_watch_tick(void)
{
    unsigned st = stats_addr(), ent, room, a, b;
    if (!st || !readable((void *)(size_t)(st + OFF_ENTITY), 4)) return;
    ent = *(unsigned *)(size_t)(st + OFF_ENTITY);
    if (!ent || !readable((void *)(size_t)(ent + 0x1038), 4)) return;
    room = *(unsigned *)(size_t)(ent + 0x1038);
    if (!room || !readable((void *)(size_t)(room + 0x180), 4)) return;
    a = *(unsigned *)(size_t)(room + 0x178);
    b = *(unsigned *)(size_t)(room + 0x17C);
    if (room == g_last_room && a == g_last_room_a && b == g_last_room_b) return;
    logmsg("mapwatch: room=%08x w=%u h=%u a178=%u a17c=%u a1e0=%u  (was %08x %u %u)",
           room,
           *(unsigned *)(size_t)(room + 0x08),
           *(unsigned *)(size_t)(room + 0x0C),
           a, b, *(unsigned *)(size_t)(room + 0x1E0),
           g_last_room, g_last_room_a, g_last_room_b);
    g_last_room = room;
    g_last_room_a = a;
    g_last_room_b = b;
}

/* ---- game mode watch (single-player vs multiplayer) ----------------------
 * 0x00672334 is the flag the game itself reads to choose save\game_mp_N.sav.
 * Log every transition so a money/feature branch can be traced in the log. */
static int g_last_mp = -1;
static void mode_watch_tick(void)
{
    int mp = game_is_mp();
    if (mp == g_last_mp) return;
    logmsg("mode: %s (0x%08x=%u), money item %d, UN item %d",
           mp ? "multiplayer" : "single-player", ADDR_MP_MODE,
           *(unsigned *)ADDR_MP_MODE, ITEM_COIN, ITEM_UN);
    g_last_mp = mp;
}

/* ---- room bookmarks (zone/room -> last known player position) -----------
 * The map cell records name a room (zone at +0x2B8, room at +0x2B0) but not
 * a position, and 0x0042E030 does not place the player.  So we remember where
 * the player stood in every room: one second after the room changes, append
 * "zone<TAB>room<TAB>x<TAB>y" to mod\roomspots.tsv.  A warp looks the room up
 * there and writes the coordinates after loading; unknown rooms are refused.
 */
typedef struct { unsigned zone, room; float x, y; } RoomSpot;
static RoomSpot g_spot[512];
static int g_nspot;
static unsigned g_track_zone, g_track_room;
/* a warp is applied over ~1 s: the engine keeps writing the level pointer and
 * the player position for a few frames after the room load, so we hold ours */
static int g_pend_n;
static float g_pend_x, g_pend_y;
static unsigned g_pend_lvl;
static int g_track_wait = -1;

static void spots_path(char *out, size_t n)
{
    char ini[MAX_PATH];
    char *s;
    cfg_path(ini, sizeof ini);
    s = strrchr(ini, '\\');
    if (s) *s = 0;
    _snprintf(out, n, "%s\\mod\\roomspots.tsv", ini);
}

static void spots_load(void)
{
    char path[MAX_PATH], line[256];
    FILE *f;
    spots_path(path, sizeof path);
    f = fopen(path, "r");
    if (!f) { logmsg("roomspots: none yet (%s)", path); return; }
    while (fgets(line, sizeof line, f)) {
        unsigned z, r;
        float x, y;
        if (sscanf(line, "%u\t%u\t%f\t%f", &z, &r, &x, &y) == 4) {
            int i, dup = -1;
            for (i = 0; i < g_nspot; i++)
                if (g_spot[i].zone == z && g_spot[i].room == r) dup = i;
            if (dup < 0 && g_nspot < (int)(sizeof g_spot / sizeof g_spot[0])) dup = g_nspot++;
            if (dup >= 0) {
                g_spot[dup].zone = z;
                g_spot[dup].room = r;
                g_spot[dup].x = x;
                g_spot[dup].y = y;
            }
        }
    }
    fclose(f);
    logmsg("roomspots: %d room(s) known", g_nspot);
}

static void spot_append(unsigned z, unsigned r, float x, float y)
{
    char path[MAX_PATH];
    FILE *f;
    spots_path(path, sizeof path);
    f = fopen(path, "a");
    if (!f) return;
    fprintf(f, "%u\t%u\t%.1f\t%.1f\n", z, r, (double)x, (double)y);
    fclose(f);
}

static int spot_find(unsigned z, unsigned r, float *x, float *y)
{
    int i;
    for (i = g_nspot - 1; i >= 0; i--) {
        if (g_spot[i].zone == z && g_spot[i].room == r) {
            *x = g_spot[i].x;
            *y = g_spot[i].y;
            return 1;
        }
    }
    return 0;
}

static void room_track_tick(void)
{
    unsigned st = stats_addr(), lvl, ent, z, r;
    if (!st) return;
    if (g_pend_n > 0) {
        if (g_pend_lvl) {
            if (*(unsigned *)(size_t)(st + OFF_LEVEL_OBJ) != g_pend_lvl)
                *(unsigned *)(size_t)(st + OFF_LEVEL_OBJ) = g_pend_lvl;
        }
        if (g_pend_x != 0.0f || g_pend_y != 0.0f) {
            ent = *(unsigned *)(size_t)(st + OFF_ENTITY);
            if (ent && readable((void *)(size_t)(ent + 0x524), 4)) {
                *(float *)(size_t)(ent + 0x520) = g_pend_x;
                *(float *)(size_t)(ent + 0x524) = g_pend_y;
            }
        }
        if (--g_pend_n == 0)
            logmsg("warp: level/position held for 1 s, releasing");
    }
    lvl = *(unsigned *)(size_t)(st + OFF_LEVEL_OBJ);
    if (!lvl || !readable((void *)(size_t)(lvl + 0x22A90), 4)) return;
    z = *(unsigned *)(size_t)(lvl + 0x22A88);
    r = *(unsigned *)(size_t)(lvl + 0x22A8C);
    if (z == g_track_zone && r == g_track_room) {
        if (g_track_wait > 0 && --g_track_wait == 0) {
            ent = *(unsigned *)(size_t)(st + OFF_ENTITY);
            if (ent && readable((void *)(size_t)(ent + 0x524), 4)) {
                float x = *(float *)(size_t)(ent + 0x520);
                float y = *(float *)(size_t)(ent + 0x524);
                int i, dup = -1;
                for (i = 0; i < g_nspot; i++)
                    if (g_spot[i].zone == z && g_spot[i].room == r) dup = i;
                if (dup < 0 && g_nspot < (int)(sizeof g_spot / sizeof g_spot[0]))
                    dup = g_nspot++;
                if (dup >= 0) {
                    g_spot[dup].zone = z;
                    g_spot[dup].room = r;
                    g_spot[dup].x = x;
                    g_spot[dup].y = y;
                }
                logmsg("roomspot: zone=%u room=%u pos=(%.1f,%.1f)", z, r,
                       (double)x, (double)y);
                spot_append(z, r, x, y);
            }
        }
        return;
    }
    logmsg("roomspot: entered zone=%u room=%u", z, r);
    g_track_zone = z;
    g_track_room = r;
    g_track_wait = 5;            /* 5 x 200 ms: let the engine place the player */
}

/* Install the two low-level hooks and pump this thread's queue.  The callbacks
 * fire on this thread and the OS waits for them, so nothing slow may ever run
 * here - that is exactly why the panel and its MessageBoxes moved off it. */
static DWORD WINAPI hook_worker(LPVOID p)
{
    MSG msg;
    (void)p;
    g_hook_tid = GetCurrentThreadId();
    g_kbd = SetWindowsHookExW(WH_KEYBOARD_LL, kbd_proc, GetModuleHandleA(NULL), 0);
    logmsg("kbd hook %s (thread %lu)", g_kbd ? "installed" : "FAILED", g_hook_tid);
    g_mouse = SetWindowsHookExW(WH_MOUSE_LL, mouse_proc, GetModuleHandleA(NULL), 0);
    logmsg("mouse hook %s", g_mouse ? "installed" : "FAILED");
    /* force the message queue into existence before announcing readiness:
     * PostThreadMessage(WM_QUIT) from CoreStop fails on a thread that has not
     * called a message function yet, and the hook thread would never exit */
    PeekMessageW(&msg, NULL, WM_USER, WM_USER, PM_NOREMOVE);
    if (g_hook_ready) SetEvent(g_hook_ready);
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (g_kbd) { UnhookWindowsHookEx(g_kbd); g_kbd = NULL; }
    if (g_mouse) { UnhookWindowsHookEx(g_mouse); g_mouse = NULL; }
    logmsg("hooks removed");
    return 0;
}

/* Stop the hook thread and remove both hooks.  Called from CoreStop BEFORE the
 * worker is asked to quit and before any code is unpatched: the callbacks must
 * not be able to run core code while the patches are being torn down. */
static void hooks_stop(void)
{
    if (!g_hook_thread) return;
    if (g_hook_tid) PostThreadMessageW(g_hook_tid, WM_QUIT, 0, 0);
    WaitForSingleObject(g_hook_thread, 2000);
    CloseHandle(g_hook_thread);
    g_hook_thread = NULL;
    g_hook_tid = 0;
}

static DWORD WINAPI worker(LPVOID p)
{    (void)p;
    if (!ENABLE_PATCH) { logmsg("patch disabled"); return 0; }
    g_wtid = GetCurrentThreadId();
    logmsg("core worker up (stats ptr -> %08x)", *(unsigned *)ADDR_STATS_PTR);
    if (DIAG_WP) g_veh = AddVectoredExceptionHandler(1, veh_handler);
    apply_patch();

    /* hooks first: they are the cheats that need key/mouse input, so they must
     * work even when the panel cannot be created */
    g_hook_ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_hook_thread = CreateThread(NULL, 0, hook_worker, NULL, 0, NULL);
    if (g_hook_thread && g_hook_ready) WaitForSingleObject(g_hook_ready, 2000);
    if (g_hook_ready) { CloseHandle(g_hook_ready); g_hook_ready = NULL; }
    logmsg("hook thread %s", g_hook_thread ? "up" : "FAILED");

    if (!panel_create()) {             /* fallback: no UI, cheats still work */
        int t = 0;
        for (;;) {
            if (g_stopping) break;
            tick();
            map_watch_tick();
            room_track_tick();
            mode_watch_tick();
            status_effects_tick();
            if (++t % 15 == 0) report(t);
            Sleep(200);
        }
        return 0;
    }

    SetTimer(g_hwnd, 1, 200, NULL);
    {
        MSG msg;
        int ticks = 0, tick_noime = 0;
        DWORD last_status = 0;
        while (GetMessageW(&msg, NULL, 0, 0) > 0) {
            if (g_stopping) break;
            if (msg.message == WM_TIMER) {
                tick();
                map_watch_tick();
                room_track_tick();
                mode_watch_tick();
                status_effects_tick();
                /* coalesced mod\buffs.ini write: never inside a click handler,
                 * so a burst of checkbox changes costs at most one flush a second
                 * (the input hooks now live on their own thread, see hook_worker) */
                if (g_bcfg_dirty && (LONG)GetTickCount() - g_bflush_tick > 1000) {
                    g_bflush_tick = GetTickCount();
                    g_bcfg_dirty = 0;
                    buffs_cfg_save();
                }
                if (g_give_done) { g_give_done = 0; give_report((int)g_give_result); }
                else if (g_give_req
                         && (LONG)GetTickCount() - g_give_post_tick > 4000) {
                    g_give_req = 0;
                    give_report(-99);
                }
                if (g_reto_done) {
                    g_reto_done = 0;
                    retos_report((int)g_reto_result, (int)g_reto_unlocked);
                } else if (g_reto_req
                           && (LONG)GetTickCount() - g_reto_post_tick > 4000) {
                    g_reto_req = 0;
                    hint_flash(L"the game thread did not answer");
                }
                if (g_ach_done) {
                    g_ach_done = 0;
                    ach_report((int)g_ach_result, (int)g_ach_ok);
                } else if (g_ach_req
                           && (LONG)GetTickCount() - g_ach_post_tick > 8000) {
                    g_ach_req = 0;
                    hint_flash(L"the game thread did not answer");
                }
                if (g_light_req) {
                    /* one retry per tick until the game window answers; the
                     * handler clears it, a lost message would keep it forever,
                     * so give up after a few seconds of a stale request */
                    if (++g_light_stale > 25) {
                        g_light_stale = 0;
                        g_light_req = 0;
                        logmsg("lights: request dropped (no answer from the game window)");
                    } else {
                        light_post();
                    }
                } else {
                    g_light_stale = 0;
                }
                ++ticks;
                if (ticks % 15 == 0) report(ticks);
                if (tick_noime++ % 10 == 0) {
                    ime_guard();
                    hook_steam_write();
                    report_steam_write();
                    if (g_speed_cnt != g_speed_last_cnt) {
                        g_speed_last_cnt = g_speed_cnt;
                        logmsg("speed hook: value=%.3f pct=%ld",
                               (double)*(float *)&g_last_speed, g_speed_pct);
                    }
                }
                if (g_panel_visible) {
                    DWORD now = GetTickCount();
                    if (now - last_status >= 250) {
                        last_status = now;
                        panel_refresh();
                        /* the map table is only valid while the multiplayer
                         * save table is the loaded one, so the button follows
                         * the live state; log each flip so a misfire in the
                         * menu can be traced from unepic_mod.log */
                        if (g_bt_mpunlock) {
                            static int was = -1;
                            int ok = mp_unlock_state_ok();
                            EnableWindow(g_bt_mpunlock, ok);
                            if (ok != was) {
                                unsigned t = mp_table_mp();
                                unsigned sl = 0xFFFFFFFFu, pr = 0;
                                if (t && !IsBadReadPtr((void *)ADDR_MP_SLOT, 4)) {
                                    sl = *(unsigned *)ADDR_MP_SLOT;
                                    if (sl < 9)
                                        pr = *(unsigned *)(size_t)(t + MP_MAP_PRESENT + sl * 4);
                                }
                                logmsg("mp unlock: button %s (mp=%d table=%08x"
                                       " mode=%u slot=%u present=%u menu=%08x)",
                                       ok ? "enabled" : "disabled",
                                       game_is_mp() ? 1 : 0, t,
                                       t ? *(unsigned *)(size_t)t : 0, sl, pr,
                                       mp_menu_state());
                                was = ok;
                            }
                        }
                        /* Force save is single-player only; follow the mode live
                         * so entering/leaving multiplayer cannot leave it on. */
                        if (g_bt_force) EnableWindow(g_bt_force, !game_is_mp());
                    }
                }
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    buffs_close();          /* its class lives in this DLL, do not orphan it */
    if (g_hwnd) {
        KillTimer(g_hwnd, 1);
        DestroyWindow(g_hwnd);
        g_hwnd = NULL;
    }
    UnregisterClassW(L"UnepicPanel", GetModuleHandleA(NULL));
    if (g_ui_font) { DeleteObject(g_ui_font); g_ui_font = NULL; }
    if (g_ui_bold) { DeleteObject(g_ui_bold); g_ui_bold = NULL; }
    if (g_saves_cls[0]) {
        UnregisterClassW(g_saves_cls, GetModuleHandleA(NULL));
        g_saves_cls[0] = 0;
    }
    g_wtid = 0;
    return 0;
}

/* drop core_*.dll copies left over by earlier sessions (locked ones stay) */
static void cleanup_old_cores(void)
{
    char dir[MAX_PATH], pat[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    GetModuleFileNameA(NULL, dir, MAX_PATH);
    char *s = strrchr(dir, '\\');
    if (s) *s = 0;
    _snprintf(pat, sizeof pat, "%s\\mod\\core_*.dll", dir);
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        char f[MAX_PATH];
        _snprintf(f, sizeof f, "%s\\mod\\%s", dir, fd.cFileName);
        DeleteFileA(f);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

/* optional: arm DR0 on an arbitrary address for write tracing
 * [debug] watch=DA8730   (hex, no 0x) - declared above, next to bag_diff */
static int g_dumpdict;
static int g_dumpmap;                  /* [debug] dumpmap: map/room recon, one-shot */
static unsigned g_gtv[3];              /* [debug] givetest = type,count,ground */

/* one-shot item test: [debug] givetest=6005,1,0 */
static void parse_givetest(const char *s)
{
    char *end;
    int i = 0;
    while (i < 3 && *s) {
        unsigned long v = strtoul(s, &end, 10);
        if (end == s) break;
        g_gtv[i++] = (unsigned)v;
        s = end;
        while (*s == ',' || *s == ' ') s++;
    }
}

/* ---- log-only INT 3 breakpoints ------------------------------------------
 * [debug] bp=437fb0,423480     (hex, comma separated, max 4)
 * Each hit logs ecx (this), the stack args and the caller, then single-steps
 * the original instruction and re-arms.  Nothing is changed for the game.
 */
#define MAXBP 4
#define BP_HIT_CAP 20000
#define BP_SEEN 96
#define BP_PEND 8
static struct { unsigned addr; unsigned char orig; int armed; int hits;
                DWORD pend[BP_PEND]; int npend;
                unsigned long long seen[BP_SEEN]; int nseen; } g_bp[MAXBP];
static int g_nb;

static void bp_parse(const char *s)
{
    g_nb = 0;
    while (*s && g_nb < MAXBP) {
        char *end;
        unsigned long v = strtoul(s, &end, 16);
        if (end == s) { s++; continue; }
        if (v > 0x10000 && v < 0x7FF00000) {
            g_bp[g_nb].addr = (unsigned)v;
            g_bp[g_nb].hits = 0;
            g_bp[g_nb].armed = 0;
            g_bp[g_nb].nseen = 0;
            g_bp[g_nb].npend = 0;
            g_nb++;
        }
        s = end;
        while (*s == ',' || *s == ' ' || *s == ';' || *s == '\t') s++;
    }
}

static void bp_install(void)
{
    int i;
    DWORD old;
    for (i = 0; i < g_nb; i++) {
        unsigned char *p = (unsigned char *)(size_t)g_bp[i].addr;
        if (IsBadReadPtr(p, 1)) { logmsg("bp: %08x unreadable", g_bp[i].addr); continue; }
        if (!VirtualProtect(p, 1, PAGE_EXECUTE_READWRITE, &old)) {
            logmsg("bp: VirtualProtect %08x failed %lu", g_bp[i].addr, GetLastError());
            continue;
        }
        g_bp[i].orig = p[0];
        p[0] = 0xCC;
        VirtualProtect(p, 1, old, &old);
        FlushInstructionCache(GetCurrentProcess(), p, 1);
        g_bp[i].armed = 1;
        g_bp[i].hits = 0;
        g_bp[i].nseen = 0;
        g_bp[i].npend = 0;
        logmsg("bp: armed at %08x (orig byte %02x)", g_bp[i].addr, g_bp[i].orig);
    }
}

static void bp_remove(void)
{
    int i;
    DWORD old;
    for (i = 0; i < g_nb; i++) {
        unsigned char *p = (unsigned char *)(size_t)g_bp[i].addr;
        if (!g_bp[i].armed) continue;
        if (VirtualProtect(p, 1, PAGE_EXECUTE_READWRITE, &old)) {
            p[0] = g_bp[i].orig;
            VirtualProtect(p, 1, old, &old);
            FlushInstructionCache(GetCurrentProcess(), p, 1);
        }
        g_bp[i].armed = 0;
    }
}

/* returns 1 when the exception was one of our breakpoints and was logged */
static int bp_hit_breakpoint(EXCEPTION_POINTERS *ep)
{
    CONTEXT *c = ep->ContextRecord;
    int i;
    for (i = 0; i < g_nb; i++) {
        if (!g_bp[i].armed) continue;
        if (c->Eip != g_bp[i].addr) continue;
        {
            unsigned char *p = (unsigned char *)(size_t)g_bp[i].addr;
            unsigned *sp = (unsigned *)(size_t)c->Esp;
            DWORD old;
            g_bp[i].hits++;
            if (g_bp[i].hits > BP_HIT_CAP) {
                if (VirtualProtect(p, 1, PAGE_EXECUTE_READWRITE, &old)) {
                    p[0] = g_bp[i].orig;
                    VirtualProtect(p, 1, old, &old);
                    FlushInstructionCache(GetCurrentProcess(), p, 1);
                }
                logmsg("BP %08x: %d hits, disarmed", g_bp[i].addr, g_bp[i].hits);
                g_bp[i].armed = 0;
                return 1;
            }
            {
                unsigned long long key = ((unsigned long long)sp[0] << 32) ^
                                         ((unsigned long long)sp[1] << 16) ^ sp[2] ^
                                         ((unsigned long long)c->Eax << 8) ^
                                         ((unsigned long long)(sp[3] & 0xffff) << 48);
                int s, dup = 0;
                for (s = 0; s < g_bp[i].nseen; s++)
                    if (g_bp[i].seen[s] == key) { dup = 1; break; }
                if (!dup && g_bp[i].nseen < BP_SEEN) {
                    g_bp[i].seen[g_bp[i].nseen++] = key;
                    logmsg("BP %08x #%d from=%08x this=%08x eax=%08x | a0=%08x a1=%08x a2=%08x a3=%08x a4=%08x a5=%08x | edx=%08x ebx=%08x esi=%08x edi=%08x",
                           g_bp[i].addr, g_bp[i].hits, sp[0], c->Ecx, c->Eax,
                           sp[1], sp[2], sp[3], sp[4], sp[5], sp[6],
                           c->Edx, c->Ebx, c->Esi, c->Edi);
                }
            }
            if (!VirtualProtect(p, 1, PAGE_EXECUTE_READWRITE, &old)) return 1;
            p[0] = g_bp[i].orig;
            VirtualProtect(p, 1, old, &old);
            FlushInstructionCache(GetCurrentProcess(), p, 1);
            {
                DWORD me = GetCurrentThreadId();
                int s;
                for (s = 0; s < g_bp[i].npend; s++)
                    if (g_bp[i].pend[s] == me) break;
                if (s == g_bp[i].npend && g_bp[i].npend < BP_PEND)
                    g_bp[i].pend[g_bp[i].npend++] = me;
                else if (g_bp[i].npend >= BP_PEND) {
                    /* too many threads in flight: put the trap back, do not step */
                    DWORD o2;
                    if (VirtualProtect(p, 1, PAGE_EXECUTE_READWRITE, &o2)) {
                        p[0] = 0xCC;
                        VirtualProtect(p, 1, o2, &o2);
                        FlushInstructionCache(GetCurrentProcess(), p, 1);
                    }
                    return 1;
                }
                c->EFlags |= 0x100u;      /* trap flag: one single step */
            }
        }
        return 1;
    }
    return 0;
}

static int bp_hit_singlestep(EXCEPTION_POINTERS *ep)
{
    CONTEXT *c = ep->ContextRecord;
    int i, s;
    DWORD me = GetCurrentThreadId();
    for (i = 0; i < g_nb; i++) {
        for (s = 0; s < g_bp[i].npend; s++)
            if (g_bp[i].pend[s] == me) break;
        if (s == g_bp[i].npend) continue;
        {
            unsigned char *p = (unsigned char *)(size_t)g_bp[i].addr;
            DWORD old;
            g_bp[i].pend[s] = g_bp[i].pend[--g_bp[i].npend];
            c->EFlags &= ~0x100u;
            if (g_bp[i].npend == 0 && g_bp[i].armed &&
                VirtualProtect(p, 1, PAGE_EXECUTE_READWRITE, &old)) {
                p[0] = 0xCC;
                VirtualProtect(p, 1, old, &old);
                FlushInstructionCache(GetCurrentProcess(), p, 1);
            }
        }
        return 1;
    }
    return 0;
}

/* ---- [debug] dumpmap=1: map / room reconnaissance (one-shot) -------------
 * Everything here is read-only; the ini key is reset after it runs.  It logs
 *
 *   1. stats+0x17F00..0x17F9F.  The save loader fills +0x17F40..+0x17F50 with
 *      six dwords read from its "WARP" chunk (0x0043D9E1..0x0043DA44), so the
 *      map/room/position the game resumed from can be read straight off it.
 *   2. The loaded level object (stats+0x17F7C): first 0x200 bytes as dwords,
 *      then every printable run >= 4 chars in its first 0x4000 bytes.
 *   3. Up to 12 "map_" strings in the whole process with 40 chars of context -
 *      the loaded map file name, if the game keeps it around.
 */
static int dump_ascii_run(const unsigned char *p, int n, char *out, int cap)
{
    int i = 0, o = 0;
    for (i = 0; i < n && o < cap - 1; i++) {
        unsigned char c = p[i];
        out[o++] = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
    }
    out[o] = 0;
    return o;
}

static void dump_file_raw(const char *name, const void *p, unsigned n)
{
    char ini[MAX_PATH], path[MAX_PATH], *s;
    HANDLE h;
    DWORD w = 0;
    cfg_path(ini, sizeof ini);
    s = strrchr(ini, '\\');
    if (s) *s = 0;
    _snprintf(path, sizeof path, "%s\\mod\\%s", ini, name);
    h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        logmsg("dumpmap: cannot write %s (err %lu)", path, GetLastError());
        return;
    }
    WriteFile(h, p, n, &w, NULL);
    CloseHandle(h);
    logmsg("dumpmap: wrote %s (%u bytes)", path, n);
}

static void dump_map_info(void)
{
    static unsigned char buf[0x10000];
    unsigned st = stats_addr(), lvl, a, i;
    MEMORY_BASIC_INFORMATION mbi;
    char line[128];
    int hits = 0, runs = 0;

    logmsg("dumpmap: ---- map/room recon ----");
    if (!st || !readable((void *)(size_t)st, 0x18000)) {
        logmsg("dumpmap: no stats object (not in game yet?)");
        return;
    }
    for (i = 0x17F00; i <= 0x17F90; i += 0x10) {
        unsigned *q = (unsigned *)(size_t)(st + i);
        logmsg("dumpmap: stats+%05x: %08x %08x %08x %08x", i, q[0], q[1], q[2], q[3]);
    }
    lvl = *(unsigned *)(size_t)(st + OFF_LEVEL_OBJ);
    logmsg("dumpmap: stats=%08x level=%08x entity=%08x",
           st, lvl, *(unsigned *)(size_t)(st + OFF_ENTITY));

    /* current map index: 0x00675174 holds index+1 (set at 0x00471CB0 by the
     * map loader); the per-map table is *(0x00EE29E4), 32 bytes per entry */
    {
        unsigned idx = *(unsigned *)(size_t)0x675174u;
        unsigned tab = *(unsigned *)(size_t)0xee29e4u;
        logmsg("dumpmap: global[675174]=%u table[ee29e4]=%08x", idx, tab);
        if (tab && readable((void *)(size_t)tab, 8 * 0x20)) {
            for (i = 0; i < 8; i++) {
                unsigned *q = (unsigned *)(size_t)(tab + i * 0x20);
                logmsg("dumpmap: map[%u]: %08x %08x %08x %08x | %08x %08x %08x %08x",
                       i, q[0], q[1], q[2], q[3], q[4], q[5], q[6], q[7]);
            }
        }
    }
    if (readable((void *)(size_t)st, 0x20000))
        dump_file_raw("statsdump.bin", (void *)(size_t)st, 0x20000);
    if (lvl && readable((void *)(size_t)lvl, 0x8000))
        dump_file_raw("lvldump.bin", (void *)(size_t)lvl, 0x8000);

    /* the player entity points at the loaded room: [ent+0x1038] is what the
     * map loader / tile code works on (0x00471D5B, 0x00471E3A) */
    {
        unsigned ent = *(unsigned *)(size_t)(st + OFF_ENTITY);
        if (ent && readable((void *)(size_t)(ent + 0x1030), 0x10)) {
            unsigned room = *(unsigned *)(size_t)(ent + 0x1038);
            unsigned other = *(unsigned *)(size_t)(ent + 0x1034);
            logmsg("dumpmap: entity=%08x [ent+1034]=%08x [ent+1038]=%08x",
                   ent, other, room);
            if (room && readable((void *)(size_t)room, 0x200)) {
                for (i = 0; i < 0x200; i += 0x10) {
                    unsigned *q = (unsigned *)(size_t)(room + i);
                    logmsg("dumpmap: room+%04x: %08x %08x %08x %08x",
                           i, q[0], q[1], q[2], q[3]);
                }
                if (readable((void *)(size_t)room, 0x8000))
                    dump_file_raw("roomdump.bin", (void *)(size_t)room, 0x8000);            }
        }
    }

    if (lvl && readable((void *)(size_t)lvl, 0x200)) {
        for (i = 0; i < 0x200; i += 0x10) {
            unsigned *q = (unsigned *)(size_t)(lvl + i);
            logmsg("dumpmap: lvl+%04x: %08x %08x %08x %08x", i, q[0], q[1], q[2], q[3]);
        }
        /* strings in the level object */
        if (readable((void *)(size_t)lvl, 0x4000)) {
            const unsigned char *p = (const unsigned char *)(size_t)lvl;
            int n = 0;
            for (i = 0; i < 0x4000 && runs < 16; ) {
                if (p[i] >= 0x20 && p[i] < 0x7F) {
                    for (n = 0; i + n < 0x4000 && p[i + n] >= 0x20 && p[i + n] < 0x7F; n++) ;
                    if (n >= 4) {
                        dump_ascii_run(p + i, n > 60 ? 60 : n, line, sizeof line);
                        logmsg("dumpmap: lvl str +%04x: \"%s\"%s", i, line, n > 60 ? "..." : "");
                        runs++;
                    }
                    i += n;
                } else {
                    i++;
                }
            }
        }
    } else {
        logmsg("dumpmap: no level object (menu / loading screen)");
    }

    /* whole-process scan for "map_" - bounded, read-only */
    a = 0x10000;
    while (a < 0x7FFF0000 && hits < 12) {
        unsigned base, size, off;
        DWORD pr;
        if (!VirtualQuery((void *)(size_t)a, &mbi, sizeof mbi)) break;
        base = (unsigned)(size_t)mbi.BaseAddress;
        size = (unsigned)mbi.RegionSize;
        pr = mbi.Protect;
        if (mbi.State == MEM_COMMIT && size && !(pr & PAGE_GUARD) &&
            (pr & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                   PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) {
            for (off = 0; off < size && hits < 12; ) {
                unsigned chunk = size - off;
                if (chunk > sizeof buf) chunk = sizeof buf;
                if (!IsBadReadPtr((void *)(size_t)(base + off), chunk)) {
                    unsigned j;
                    memcpy(buf, (void *)(size_t)(base + off), chunk);
                    for (j = 0; j + 4 < chunk && hits < 12; j++) {
                        if (buf[j] == 'm' && buf[j + 1] == 'a' &&
                            buf[j + 2] == 'p' && buf[j + 3] == '_') {
                            unsigned left = j > 16 ? j - 16 : 0;
                            dump_ascii_run(buf + left, 56, line, sizeof line);
                            logmsg("dumpmap: %08x: %s", base + off + left, line);
                            hits++;
                            j += 40;
                        }
                    }
                }
                off += chunk;
            }
        }
        a = base + size;
        if (a <= base) break;
    }
    logmsg("dumpmap: map_ scan: %d hit(s)", hits);
    logmsg("dumpmap: ---- end ----");
}

static void read_debug_cfg(void)
{
    char p[MAX_PATH], b[96];
    cfg_path(p, sizeof p);
    GetPrivateProfileStringA("debug", "watch", "", b, sizeof b, p);
    g_watch_addr = (unsigned)strtoul(b, NULL, 16);
    GetPrivateProfileStringA("debug", "watchent", "", b, sizeof b, p);
    g_watch_ent_off = (unsigned)strtoul(b, NULL, 16);
    g_watch_rw = GetPrivateProfileIntA("debug", "watchrw", 0, p);
    g_watch_net = GetPrivateProfileIntA("debug", "watchnet", 0, p);
    g_netsync_on = GetPrivateProfileIntA("debug", "netsync", 0, p);
    g_dumpdict = GetPrivateProfileIntA("debug", "dumpdict", 0, p);
    g_dumpmap  = GetPrivateProfileIntA("debug", "dumpmap", 0, p);
    g_warp_id  = GetPrivateProfileIntA("debug", "warptest", -1, p);
    g_roomwarp_cell = GetPrivateProfileIntA("debug", "roomwarp", -1, p);
    GetPrivateProfileStringA("debug", "warpbyid", "", b, sizeof b, p);
    if (sscanf(b, "%d,%d", &g_wb_zone, &g_wb_room) != 2) g_wb_zone = g_wb_room = -1;
    g_nudge_x = g_nudge_y = 0.0f;
    g_nudge_wait = 0;
    g_nudge_ramp = GetPrivateProfileIntA("debug", "nudge_ramp", 0, p);
    GetPrivateProfileStringA("debug", "nudge", "", b, sizeof b, p);
    if (b[0] && sscanf(b, "%f,%f", &g_nudge_x, &g_nudge_y) >= 1 &&
        (g_nudge_x || g_nudge_y)) {
        g_nudge_wait = 12;
        logmsg("cfg: nudge (%.1f,%.1f) pending, fires in about 2 s%s",
               (double)g_nudge_x, (double)g_nudge_y, g_nudge_ramp ? " (ramp)" : "");
    }
    g_poke_addr = 0; g_poke_val = 0.0f; g_poke_wait = 0;
    GetPrivateProfileStringA("debug", "poke", "", b, sizeof b, p);
    if (b[0]) {
        char *end = NULL;
        unsigned long pa = strtoul(b, &end, 16);
        float pv = 0.0f;
        if (pa && end && sscanf(end, ",%f", &pv) == 1) {
            g_poke_addr = (unsigned)pa;
            g_poke_val  = pv;
            g_poke_wait = 12;
            logmsg("cfg: poke %08x = %.4f pending", (unsigned)pa, (double)pv);
        } else {
            logmsg("cfg: poke wants ADDR,FLOAT with a hex ADDR - got \"%s\"", b);
        }
    }
    g_netwatch = GetPrivateProfileIntA("debug", "netwatch", 0, p);
    g_vprobe = GetPrivateProfileIntA("debug", "vprobe", 0, p);
    g_hpwatch = GetPrivateProfileIntA("debug", "hpwatch", 0, p);
    g_pokenet_wait = 0;
    g_pokenet_x = g_pokenet_y = 0.0f;
    GetPrivateProfileStringA("debug", "pokenet", "", b, sizeof b, p);
    if (b[0] && sscanf(b, "%f,%f", &g_pokenet_x, &g_pokenet_y) == 2) {
        g_pokenet_wait = 12;
        logmsg("cfg: pokenet (%.2f,%.2f) pending", (double)g_pokenet_x, (double)g_pokenet_y);
    }
    if (g_netwatch) logmsg("cfg: netwatch=1 (logs the net player position on change)");
    g_wpauto   = GetPrivateProfileIntA("debug", "wpauto", 0, p);
    GetPrivateProfileStringA("debug", "bp", "", b, sizeof b, p);
    bp_parse(b);
    GetPrivateProfileStringA("debug", "givetest", "", b, sizeof b, p);
    parse_givetest(b);
    if (g_watch_addr) logmsg("cfg: watch=%08x", g_watch_addr);
    if (g_watch_ent_off) logmsg("cfg: watchent=+%x (game thread only)", g_watch_ent_off);
    if (g_dumpdict) logmsg("cfg: dumpdict=%d", g_dumpdict);
    if (g_dumpmap)  logmsg("cfg: dumpmap=1 (one-shot)");
    if (g_warp_id >= 0) logmsg("cfg: warptest=%d (one-shot)", g_warp_id);
    if (g_roomwarp_cell >= 0) logmsg("cfg: roomwarp=%d (one-shot)", g_roomwarp_cell);
    if (g_nb) logmsg("cfg: %d breakpoint(s)", g_nb);
    if (g_gtv[1]) logmsg("cfg: givetest=%u,%u,%u", g_gtv[0], g_gtv[1], g_gtv[2]);
}

/* ---- offline dictionary dump (one-shot helper) ---------------------------
 * [debug] dumpdict=1 makes the core call the game's OWN dictionary loader
 * (0x004E8240, thiscall(self, ansi path, flag)) once per language / category,
 * into a private 0x8008-byte scratch object, and writes the result out as
 * UTF-8 TSV:  <exe dir>\\mod\\dict\<category>_<lang>.tsv   ("index \t text")
 *
 * Object layout, verified against the live one at 0x005B0948:
 *   +0x0000  DWORD        entry count (1604 for itemNames_en)
 *   +0x0004  DWORD[count] -> UTF-16 "display||tag" strings
 * The game ships 19 languages; the packed flag makes the loader swap the
 * ".txt" extension for ".bin" and decrypt, which is why we do not have to
 * implement the container format ourselves.
 */
static const char *g_dict_langs[] = {
    "br", "ca", "ch", "cz", "en", "eu", "fr", "ge", "gr", "hu",
    "it", "jp", "kr", "nl", "po", "pt", "ru", "sp", "tw" };
static const char *g_dict_cats[] = { "itemNames", "itemDesc",
                                     "effectName", "bonus", "bonusDesc",
                                     "challenges", "logros" };

static void dump_dictionaries(void)
{
    typedef int (__attribute__((thiscall)) *dict_load_t)(void *self, const char *path, int flag);
    dict_load_t load = (dict_load_t)0x004E8240u;
    char dir[MAX_PATH], file[MAX_PATH], name[128], ini[MAX_PATH];
    static WCHAR wbuf[512];
    static char ubuf[4096];
    int ci, li, files = 0;

    _snprintf(dir, sizeof dir, "%s\\mod\\dict", g_dir);
    CreateDirectoryA(dir, NULL);
    for (ci = 0; ci < (int)(sizeof g_dict_cats / sizeof g_dict_cats[0]); ci++) {
        for (li = 0; li < (int)(sizeof g_dict_langs / sizeof g_dict_langs[0]); li++) {
            void *obj = VirtualAlloc(NULL, 0x8008, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            unsigned count, i, written = 0;
            int ret;
            FILE *f;
            if (!obj) { logmsg("dict: VirtualAlloc failed, giving up"); goto finish; }
            _snprintf(name, sizeof name, "dictios_pc/dictio_%s_%s.txt",
                      g_dict_cats[ci], g_dict_langs[li]);
            ret = load(obj, name, 1);      /* 1 = packed: loader swaps .txt -> .bin */
            count = *(unsigned *)obj;
            logmsg("dict: %s ret=%d count=%u p0=%08x p1=%08x", name, ret, count,
                   count ? ((unsigned *)obj)[1] : 0u,
                   count > 1 ? ((unsigned *)obj)[2] : 0u);
            if (count == 0 || count > 4000 ||
                IsBadReadPtr((void *)(size_t)((unsigned *)obj)[1], 2)) {
                logmsg("dict: %s not populated", name);
                VirtualFree(obj, 0, MEM_RELEASE);
                continue;
            }
            _snprintf(file, sizeof file, "%s\\%s_%s.tsv", dir, g_dict_cats[ci], g_dict_langs[li]);
            f = fopen(file, "wb");
            if (!f) {
                logmsg("dict: cannot open %s", file);
                VirtualFree(obj, 0, MEM_RELEASE);
                continue;
            }
            fwrite("\xEF\xBB\xBF", 1, 3, f);
            for (i = 0; i < count; i++) {
                unsigned p = ((unsigned *)obj)[1 + i];
                int nw = 0, j;
                fprintf(f, "%u\t", i);
                if (p > 0x10000 && !IsBadReadPtr((void *)(size_t)p, 2)) {
                    while (nw < 500) {
                        WCHAR *pw = (WCHAR *)(size_t)(p + (unsigned)nw * 2u);
                        if (IsBadReadPtr(pw, 2) || *pw == 0) break;
                        wbuf[nw++] = *pw;
                    }
                    wbuf[nw] = 0;
                    ubuf[0] = 0;
                    WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, ubuf, sizeof ubuf - 1, NULL, NULL);
                    for (j = 0; ubuf[j]; j++)
                        if (ubuf[j] == '\t' || ubuf[j] == '\n' || ubuf[j] == '\r') ubuf[j] = ' ';
                    fprintf(f, "%s", ubuf);
                    written++;
                }
                fprintf(f, "\n");
            }
            fclose(f);
            VirtualFree(obj, 0, MEM_RELEASE);
            files++;
            logmsg("dict: %s count=%u written=%u", name, count, written);
        }
    }
finish:
    cfg_path(ini, sizeof ini);
    WritePrivateProfileStringA("debug", "dumpdict", "0", ini);
    logmsg("dict: done, %d file(s) in mod\\dict, dumpdict reset to 0", files);
}

/* ---------------- first-chance exception logger ---------------- */
static PVOID g_veh_crash;

static int bp_hit_breakpoint(EXCEPTION_POINTERS *ep);
static int bp_hit_singlestep(EXCEPTION_POINTERS *ep);

static LONG CALLBACK crash_logger(EXCEPTION_POINTERS *ep)
{
    static int n;
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_SINGLE_STEP) {
        if (bp_hit_singlestep(ep)) return EXCEPTION_CONTINUE_EXECUTION;
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (code == EXCEPTION_BREAKPOINT) {
        if (bp_hit_breakpoint(ep)) return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (n < 10) {
        unsigned *sp = (unsigned *)ep->ContextRecord->Esp;
        n++;
        logmsg("EXC code=%08lx addr=%08x eip=%08x esp=%08x",
               code, (unsigned)(size_t)ep->ExceptionRecord->ExceptionAddress,
               (unsigned)ep->ContextRecord->Eip, (unsigned)ep->ContextRecord->Esp);
        if (readable(sp, 32))
            logmsg("  stack: %08x %08x %08x %08x %08x %08x %08x %08x",
                   sp[0], sp[1], sp[2], sp[3], sp[4], sp[5], sp[6], sp[7]);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

/* ---------------- IME guard: the game only wants raw keys ---------------- */
static HWND    g_game_hwnd;
static WNDPROC g_game_oldproc;
static DWORD   g_enum_pid;
static HWND    g_enum_best;
static LONG    g_enum_area;

static BOOL CALLBACK enum_proc(HWND h, LPARAM lp)
{
    DWORD p;
    RECT r;
    (void)lp;
    GetWindowThreadProcessId(h, &p);
    if (p != g_enum_pid || !IsWindowVisible(h)) return TRUE;
    if (!GetWindowRect(h, &r)) return TRUE;
    LONG a = (r.right - r.left) * (r.bottom - r.top);
    if (a > g_enum_area) { g_enum_area = a; g_enum_best = h; }
    return TRUE;
}

static HWND find_game_window(void)
{
    g_enum_pid = GetCurrentProcessId();
    g_enum_best = NULL;
    g_enum_area = 0;
    EnumWindows(enum_proc, 0);
    return g_enum_best;
}

/* windowed = the game window does not cover its whole monitor */
static int game_windowed(void)
{
    HWND w = find_game_window();
    MONITORINFO mi;
    RECT r;
    if (!w || !GetWindowRect(w, &r)) return 1;
    mi.cbSize = sizeof mi;
    if (!GetMonitorInfoW(MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST), &mi)) return 1;
    return !(r.left <= mi.rcMonitor.left && r.top <= mi.rcMonitor.top &&
             r.right >= mi.rcMonitor.right && r.bottom >= mi.rcMonitor.bottom);
}

/* Background audio is NOT implemented - three routes were tried on 2026-10-22
 * and each one is a dead end, so the switch was taken out again:
 *   1. Hiding WM_ACTIVATEAPP / WM_ACTIVATE(WA_INACTIVE) / WM_KILLFOCUS from the
 *      game window proc: the messages really were blocked (logged) and the
 *      background stayed silent.
 *   2. Rewriting the DSSCL_NORMAL push at 0x4E2A50 (ARCAudio::Init asks for that
 *      level: push 1 / push hwnd / push this / mov eax,[ecx+0x18] / call eax,
 *      failure string "Error setting cooperative level.") *before* the init
 *      runs: the game then dies at startup with "el archivo de sonido
 *      bottle_open no es valido".  That .wav is intact on disk (159,992 bytes,
 *      valid RIFF/WAVE, 44100/stereo/16), so what fails is CreateSoundBuffer at
 *      the new level, not the file.
 *   3. Calling that same IDirectSound method ourselves at runtime (ARCAudio
 *      singleton 0x01B142C0+0x68, vtable+0x18, shape fn(this, hwnd, level),
 *      posted to the game thread): it returned DS_OK and then *everything* went
 *      silent, foreground included - changing the cooperative level after the
 *      buffers exist is what kills it.  There is no safe moment for it here.
 * The level lives in the process only, so none of this survives a game restart.
 * What is left, if it ever matters: hook DirectSound's own focus handling. */

static LRESULT CALLBACK game_wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_IME_STARTCOMPOSITION:
    case WM_IME_COMPOSITION:
    case WM_IME_COMPOSITIONFULL:
    case WM_IME_CHAR:
    case WM_IME_SELECT:
    case WM_IME_CONTROL:
    case WM_INPUTLANGCHANGEREQUEST:
        return 0;
    case WM_APP + 8: {   /* force save: full serializer on the game thread */
        typedef int (__attribute__((thiscall)) *ser_t)(void *self, int a, int b, int c);
        int r;
        if (game_is_mp()) {
            logmsg("force save: refused on the game thread (multiplayer)");
            return 0;
        }
        r = ((ser_t)0x0043E4C0u)((void *)0x00674EF8u,
                                 *(int *)0x00675174u, 0, 0);
        logmsg("force save: serializer ret=%d", r);
        if (g_hwnd) PostMessageW(g_hwnd, WM_APP + 9, (WPARAM)(r ? 1 : 0), 0);
        return 0;
    }
    case WM_APP + 10:       /* give / drop one item, on the game thread */
        if (g_give_req) give_do_pending();
        return 0;
    case WM_APP + 12:       /* close challenges + unlock their achievements */
        if (g_reto_req) {
            g_reto_result = retos_complete_all();
            g_reto_done = 1;
            g_reto_req = 0;
        }
        return 0;
    case WM_APP + 13:       /* the 52 plain achievements */
        if (g_ach_req) {
            g_ach_result = ach_unlock_all();
            g_ach_done = 1;
            g_ach_req = 0;
        }
        return 0;
    case WM_APP + 14: {     /* [debug] warptest: load another map */
        typedef int (__attribute__((thiscall)) *warpt_t)(void *self, int id, int arg2);
        int r = ((warpt_t)0x00471CA0u)((void *)0x17CA610u, g_warp_id, 0);
        logmsg("warptest: loader(%d) ret=%d", g_warp_id, r);
        g_warp_id = -1;
        return 0;
    }
    case WM_APP + 15: {     /* [debug] roomwarp: jump to the room of a map cell */
        typedef int (__attribute__((thiscall)) *goto_t)(void *self, int zone, int room, int arg3);
        unsigned rec = 0x00B03008u + (unsigned)g_roomwarp_cell * 0x2D0u;
        int room = *(int *)(size_t)(rec + 0x2B0);
        int zone = *(int *)(size_t)(rec + 0x2B8);
        unsigned st = stats_addr();
        unsigned ent = st ? *(unsigned *)(size_t)(st + OFF_ENTITY) : 0;
        int r = 0;
        if (ent)
            r = ((goto_t)0x00447080u)((void *)(size_t)ent, zone, room, 0);
        logmsg("roomwarp: cell=%d zone=%d room=%d ret=%d ent=%08x",
               g_roomwarp_cell, zone, room, r, ent);
        g_roomwarp_cell = -1;
        return 0;
    }
    case WM_APP + 16: {     /* warp request: [debug] warpbyid or map right-click */
        typedef int (__attribute__((thiscall)) *goto_t)(void *self, int zone, int room, int arg3);
        unsigned st;
        unsigned ent;
        float sx = 0, sy = 0;
        int r = -1, have = 0;
        if (g_wb_zone < 0 || g_wb_room < 0) return 0;
        st = stats_addr();
        ent = st ? *(unsigned *)(size_t)(st + OFF_ENTITY) : 0;
        have = spot_find((unsigned)g_wb_zone, (unsigned)g_wb_room, &sx, &sy);
        if (have)
            logmsg("warpbyid: recorded spot for (%d,%d) is (%.1f,%.1f)",
                   g_wb_zone, g_wb_room, (double)sx, (double)sy);
        if (ent)
            r = ((goto_t)0x00447080u)((void *)(size_t)ent, g_wb_zone, g_wb_room, 0);
        if (st) {
            /* the wrapper prepares the new room but does not re-point
             * stats+0x17F7C; the caller does.  The player entity keeps the
             * active level at +0x684 (level + 4), so mirror that. */
            unsigned ent2 = *(unsigned *)(size_t)(st + OFF_ENTITY);
            unsigned lvl2 = ent2 ? *(unsigned *)(size_t)(ent2 + 0x684) : 0;
            unsigned lvlp = (lvl2 > 0x10000) ? lvl2 - 4 : 0;
            unsigned cur = *(unsigned *)(size_t)(st + OFF_LEVEL_OBJ);
            logmsg("warpbyid: player+684=%08x -> level %08x (was %08x)",
                   lvl2, lvlp, cur);
            if (lvlp) {
                if (lvlp != cur)
                    *(unsigned *)(size_t)(st + OFF_LEVEL_OBJ) = lvlp;
                g_pend_lvl = lvlp;
            }
            if (!have && lvlp) {
                /* fallback: land on a prop of the new room (type != 3 = player) */
                int i, n = *(int *)(size_t)(lvlp + 0xBA98);
                unsigned arr = *(unsigned *)(size_t)(lvlp + 0xBAA0);
                for (i = 0; i < n && i < 64 && arr; i++) {
                    unsigned e2 = *(unsigned *)(size_t)(arr + i * 4);
                    float x2, y2;
                    if (!e2 || !readable((void *)(size_t)(e2 + 0x524), 4)) continue;
                    if (*(unsigned *)(size_t)(e2 + 0x50C) == 3) continue;
                    x2 = *(float *)(size_t)(e2 + 0x520);
                    y2 = *(float *)(size_t)(e2 + 0x524);
                    if (x2 > 0 && x2 < 8192 && y2 > 0 && y2 < 8192) {
                        sx = x2; sy = y2; have = 1;
                        logmsg("warpbyid: fallback landing at entity[%d] (%.1f,%.1f)",
                               i, (double)x2, (double)y2);
                        break;
                    }
                }
            }
            if (have) {
                g_pend_x = sx;
                g_pend_y = sy;
                g_pend_n = 5;
                logmsg("warpbyid: holding level %08x pos (%.1f,%.1f) for 1 s",
                       lvlp, (double)sx, (double)sy);
            }
        }
        logmsg("warpbyid: zone=%d room=%d ret=%d ent=%08x",
               g_wb_zone, g_wb_room, r, ent);
        g_wb_zone = g_wb_room = -1;
        return 0;
    }
    case WM_APP + 18: {   /* load a save slot: fill in the game's queued-load state */
        int slot = (int)g_load_slot;
        g_load_slot = -1;
        if (slot < 0) return 0;
        logmsg("load: queue slot %d (current=%d)", slot, *(int *)0x00675174u);
        *(int *)0x00675174u = slot;   /* slot the loader will read */
        *(int *)0x00639894u = 1;      /* pending load, consumed by the main loop */
        return 0;
    }
    case WM_APP + 20: {   /* run the queued light request, game thread */
        unsigned st2, lvl2;
        LONG mode;
        if (!g_light_req) return 0;
        mode = g_light_req;
        g_light_req = 0;
        st2 = stats_addr();
        lvl2 = st2 ? *(unsigned *)(size_t)(st2 + OFF_LEVEL_OBJ) : 0;
        if (mode == 2 && !readable((void *)(size_t)0x00AFDFE0u, 8)) {
            logmsg("lights: all-rooms request dropped (no zone tables)");
            return 0;
        }
        if (mode == 2) room_lights_all_rooms(lvl2, st2);
        else room_lights_apply(lvl2, st2);
        return 0;
    }
    }
    return CallWindowProcW(g_game_oldproc, h, m, w, l);
}

static void ime_guard(void)
{
    HWND w = find_game_window();
    if (!w || (g_hwnd && w == g_hwnd)) return;
    if (w != g_game_hwnd) {
        if (g_game_hwnd && g_game_oldproc)
            SetWindowLongPtrW(g_game_hwnd, GWLP_WNDPROC, (LONG_PTR)g_game_oldproc);
        ImmDisableIME(0);
        ImmAssociateContext(w, NULL);
        g_game_oldproc = (WNDPROC)SetWindowLongPtrW(w, GWLP_WNDPROC, (LONG_PTR)game_wndproc);
        g_game_hwnd = w;
        logmsg("ime: disabled on hwnd=%p", (void *)w);
    }
}

static void ime_release(void)
{
    if (g_game_hwnd && g_game_oldproc)
        SetWindowLongPtrW(g_game_hwnd, GWLP_WNDPROC, (LONG_PTR)g_game_oldproc);
    g_game_hwnd = NULL;
    g_game_oldproc = NULL;
}

/* wrapper 0x405450(name, data, size) -> Steam FileWrite; record its caller */
#define FN_STEAM_WRITE 0x00405450u
static volatile LONG g_fw_cnt;
static volatile LONG g_fw_name, g_fw_data, g_fw_size, g_fw_ret;
static LONG g_fw_last;

static void patch_steam_wrapper(void)
{
    unsigned char *tgt = (unsigned char *)FN_STEAM_WRITE;
    static const unsigned char PRO[] = { 0xFF,0x15,0x30,0x33,0x52,0x00 };
    unsigned char *h;
    if (memcmp(tgt, PRO, sizeof PRO) != 0) {
        logmsg("steam wrapper prologue differs: %02x %02x %02x %02x", tgt[0],tgt[1],tgt[2],tgt[3]);
        return;
    }
    h = alloc_exec(96);
    if (!h) return;
    memcpy(h + 0x00, "\xFF\x05", 2); *(DWORD *)(h + 0x02) = (DWORD)&g_fw_cnt;
    memcpy(h + 0x06, "\x8B\x44\x24\x04", 4);
    h[0x0A] = 0xA3; *(DWORD *)(h + 0x0B) = (DWORD)&g_fw_name;
    memcpy(h + 0x0F, "\x8B\x44\x24\x08", 4);
    h[0x13] = 0xA3; *(DWORD *)(h + 0x14) = (DWORD)&g_fw_data;
    memcpy(h + 0x18, "\x8B\x44\x24\x0C", 4);
    h[0x1C] = 0xA3; *(DWORD *)(h + 0x1D) = (DWORD)&g_fw_size;
    memcpy(h + 0x21, "\x8B\x04\x24", 3);
    h[0x24] = 0xA3; *(DWORD *)(h + 0x25) = (DWORD)&g_fw_ret;
    memcpy(h + 0x29, tgt, 6);
    h[0x2F] = 0xE9; *(DWORD *)(h + 0x30) = (DWORD)(tgt + 6 - (h + 0x34));
    dump_hex("steam wrapper handler", h, 0x34);
    if (install_jmp(tgt, 6, h))
        logmsg("steam wrapper hook ok");
    else
        logmsg("steam wrapper hook FAILED");
}

static void report_steam_write(void)
{
    LONG n = g_fw_cnt;
    if (n == g_fw_last) return;
    g_fw_last = n;
    {
        const char *nm = (const char *)(size_t)g_fw_name;
        char buf[96];
        if (nm && !IsBadReadPtr(nm, 1)) {
            _snprintf(buf, sizeof buf, "%s", nm);
        } else buf[0] = 0;
        logmsg("steam write: name='%s' size=%ld caller=%08x", buf,
               (long)g_fw_size, (unsigned)g_fw_ret);
        (void)0;
    }
}

/* ---- movement helper 0x4B4240(..., x_ptr, ..., speed) --------------------
 * x ptr is at [esp+8], the speed float at [esp+0x14]; scale it only when the
 * destination is the player's own x coordinate.
 *  0x00 8B 44 24 08           mov eax,[esp+8]
 *  0x04 3B 05 <&g_player_x>   cmp eax,[player_x]
 *  0x0A 75 1D                jne KEEP
 *  0x0C D9 44 24 14           fld  [esp+0x14]
 *  0x10 D8 0D <&g_speed_mul>  fmul [speed_mul]
 *  0x16 D9 5C 24 14           fstp [esp+0x14]
 *  0x1A 8B 44 24 14           mov  eax,[esp+0x14]
 *  0x1E A3 <&g_last_speed>    mov  [last],eax
 *  0x23 FF 05 <&g_speed_cnt>  inc  [cnt]
 *  0x29 51 56 8B 74 24 10     KEEP: original 6 bytes
 *  0x2F E9 rel32              jmp -> 0x4B4246
 */
static void patch_speed(void)
{
    unsigned char *tgt = (unsigned char *)0x004B4240u;
    static const unsigned char PRO[] = { 0x51, 0x56, 0x8B, 0x74, 0x24, 0x10 };
    const int KEEP = 0x29;
    unsigned char *h;
    if (memcmp(tgt, PRO, sizeof PRO) != 0) {
        logmsg("speed prologue differs: %02x %02x %02x %02x %02x %02x",
               tgt[0],tgt[1],tgt[2],tgt[3],tgt[4],tgt[5]);
        return;
    }
    h = alloc_exec(96);
    if (!h) return;
    memcpy(h + 0x00, "\x8B\x44\x24\x08", 4);
    memcpy(h + 0x04, "\x3B\x05", 2);  *(DWORD *)(h + 0x06) = (DWORD)&g_player_x;
    h[0x0A] = 0x75; h[0x0B] = (unsigned char)(KEEP - 0x0C);
    memcpy(h + 0x0C, "\xD9\x44\x24\x14", 4);
    memcpy(h + 0x10, "\xD8\x0D", 2);  *(DWORD *)(h + 0x12) = (DWORD)&g_speed_mul;
    memcpy(h + 0x16, "\xD9\x5C\x24\x14", 4);
    memcpy(h + 0x1A, "\x8B\x44\x24\x14", 4);
    h[0x1E] = 0xA3; *(DWORD *)(h + 0x1F) = (DWORD)&g_last_speed;
    memcpy(h + 0x23, "\xFF\x05", 2);  *(DWORD *)(h + 0x25) = (DWORD)&g_speed_cnt;
    memcpy(h + KEEP, tgt, 6);
    h[0x2F] = 0xE9; *(DWORD *)(h + 0x30) = (DWORD)(tgt + 6 - (h + 0x34));
    dump_hex("speed handler", h, 0x34);
    if (install_jmp(tgt, 6, h)) logmsg("speed hook ok");
    else logmsg("speed hook FAILED");
}

/* ---- attack animation durations (player update 0x4A019E) -----------------
 * Every frame the game copies two attack-phase durations (entity+0x178,
 * entity+0x17C) into entity+0x550 / entity+0x554, which gate the 1 -> 2 -> 0
 * attack state at 0x4A0470 and 0x4A063D.  We divide them on the way in, so
 * the game's own weapon/stat computation stays untouched.
 *
 *  0x00 D9 85 78 01 00 00   fld  [ebp+0x178]
 *  0x06 D8 35 <&g_atk_div>  fdiv [g_atk_div]
 *  0x0C D9 5C 24 14         fstp [esp+0x14]
 *  0x10 D9 44 24 14         fld  [esp+0x14]
 *  0x14 D9 95 50 05 00 00   fst  [ebp+0x550]     ; leaves scaled copy
 *  0x1A D9 85 7C 01 00 00   fld  [ebp+0x17c]
 *  0x20 D8 35 <&g_atk_div>  fdiv [g_atk_div]
 *  0x26 D9 5C 24 14         fstp [esp+0x14]
 *  0x2A D9 44 24 14         fld  [esp+0x14]
 *  0x2E D9 95 54 05 00 00   fst  [ebp+0x554]     ; leaves scaled copy
 *  0x34 E9 rel32            jmp  0x4A01C6
 */
static void patch_attack_speed(void)
{
    unsigned char *tgt = (unsigned char *)0x004A019Eu;
    static const unsigned char PRO[] = {
        0xD9,0x85,0x78,0x01,0x00,0x00, 0xD9,0x5C,0x24,0x14,
        0xD9,0x44,0x24,0x14, 0xD9,0x95,0x50,0x05,0x00,0x00 };
    unsigned char *h, *p;
    if (memcmp(tgt, PRO, sizeof PRO) != 0) {
        logmsg("atkspd prologue differs: %02x %02x %02x %02x %02x %02x",
               tgt[0],tgt[1],tgt[2],tgt[3],tgt[4],tgt[5]);
        return;
    }
    h = alloc_exec(128);
    if (!h) return;
    p = h;
    *p++ = 0xD9; *p++ = 0x85; *(DWORD *)p = 0x178; p += 4;
    *p++ = 0xD8; *p++ = 0x35; *(DWORD *)p = (DWORD)&g_atk_div; p += 4;
    *p++ = 0xD9; *p++ = 0x5C; *p++ = 0x24; *p++ = 0x14;
    *p++ = 0xD9; *p++ = 0x44; *p++ = 0x24; *p++ = 0x14;
    *p++ = 0xD9; *p++ = 0x95; *(DWORD *)p = 0x550; p += 4;
    *p++ = 0xD9; *p++ = 0x85; *(DWORD *)p = 0x17C; p += 4;
    *p++ = 0xD8; *p++ = 0x35; *(DWORD *)p = (DWORD)&g_atk_div; p += 4;
    *p++ = 0xD9; *p++ = 0x5C; *p++ = 0x24; *p++ = 0x14;
    *p++ = 0xD9; *p++ = 0x44; *p++ = 0x24; *p++ = 0x14;
    *p++ = 0xD9; *p++ = 0x95; *(DWORD *)p = 0x554; p += 4;
    *p++ = 0xE9; *(DWORD *)p = (DWORD)(0x004A01C6u - (DWORD)(p + 4)); p += 4;
    dump_hex("atkspd handler", h, (unsigned)(p - h));
    if (install_jmp(tgt, 6, h)) logmsg("attack-speed hook ok");
    else logmsg("attack-speed hook FAILED");
}

/* ---------------- Steam RemoteStorage FileWrite tracer -------------------
 * Saves are written through ISteamRemoteStorage, not CreateFileA.  The
 * interface's vtable entry 0 is FileWrite(file, data, size); patch it in place
 * (the game caches the interface pointer, so vtable patching is the only way).
 */
typedef void *(*get_iface_t)(void);
typedef int (__attribute__((thiscall)) *fw_t)(void *self, const char *name,
                                              const void *data, int size);

static fw_t g_fw;
static void **g_fw_slot;
static int g_steam_hooked;

static int __attribute__((thiscall)) my_fw(void *self, const char *name,
                                          const void *data, int size)
{
    if (name)
        logmsg("SteamFileWrite(%s, %d) caller=%08x", name, size,
               (unsigned)(size_t)__builtin_return_address(0));
    return g_fw(self, name, data, size);
}

static void hook_steam_write(void)
{
    unsigned char *base = (unsigned char *)GetModuleHandleA(NULL);
    IMAGE_DOS_HEADER *dh = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + dh->e_lfanew);
    IMAGE_IMPORT_DESCRIPTOR *id = (IMAGE_IMPORT_DESCRIPTOR *)
        (base + nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);

    if (g_steam_hooked) return;
    for (; id->Name; id++) {
        const char *mod = (const char *)(base + id->Name);
        IMAGE_THUNK_DATA *ilt, *iat;
        if (_stricmp(mod, "steam_api.dll") != 0) continue;
        ilt = (IMAGE_THUNK_DATA *)(base + id->OriginalFirstThunk);
        iat = (IMAGE_THUNK_DATA *)(base + id->FirstThunk);
        for (int i = 0; ilt[i].u1.AddressOfData; i++) {
            IMAGE_IMPORT_BY_NAME *ib;
            get_iface_t getter;
            void *iface;
            void **vt;
            DWORD old;
            if (ilt[i].u1.Ordinal & IMAGE_ORDINAL_FLAG) continue;
            ib = (IMAGE_IMPORT_BY_NAME *)(base + ilt[i].u1.AddressOfData);
            if (strcmp((const char *)ib->Name, "SteamRemoteStorage") != 0) continue;
            getter = (get_iface_t)iat[i].u1.Function;
            if (!getter) return;
            iface = getter();
            if (!iface) { logmsg("steam: interface not ready yet"); return; }
            vt = *(void ***)iface;
            if (!vt) return;
            if (VirtualProtect(&vt[0], 4, PAGE_READWRITE, &old)) {
                g_fw = (fw_t)vt[0];
                g_fw_slot = &vt[0];
                vt[0] = (void *)my_fw;
                VirtualProtect(&vt[0], 4, old, &old);
                g_steam_hooked = 1;
                logmsg("steam FileWrite hook: iface=%p vt=%p orig=%p",
                       iface, (void *)vt, (void *)g_fw);
            } else {
                logmsg("steam vtable VirtualProtect failed");
            }
            return;
        }
    }
}

static void unhook_steam_write(void)
{
    if (g_fw_slot && g_fw) {
        DWORD old;
        if (VirtualProtect(g_fw_slot, 4, PAGE_READWRITE, &old)) {
            *g_fw_slot = (void *)g_fw;
            VirtualProtect(g_fw_slot, 4, old, &old);
        }
        g_fw_slot = NULL;
        g_steam_hooked = 0;
    }
}

/* ---------------- exported interface (loader <-> core) ---------------- */
EXP int CoreStart(void)
{
    static int started;
    if (started) return 1;
    started = 1;
    g_stopping = 0;
    cfg_load_cheats();
    cleanup_old_cores();
    g_veh_crash = AddVectoredExceptionHandler(1, crash_logger);
    read_debug_cfg();
    spots_load();
    buffs_cfg_load();
    buffs_cfg_save();       /* create mod\buffs.ini with the defaults */
    if (g_nb) bp_install();
    if (g_dumpdict) dump_dictionaries();
    if (g_dumpmap) {
        char mp[MAX_PATH];
        dump_map_info();
        cfg_path(mp, sizeof mp);
        WritePrivateProfileStringA("debug", "dumpmap", "0", mp);
    }
    if (g_watch_addr || g_watch_ent_off || g_watch_net) {
        g_veh = AddVectoredExceptionHandler(1, veh_handler);
        if (g_watch_addr) set_dr_all(g_watch_addr, g_watch_rw ? 0x000F0001u : 0x000D0001u);
    }
    items_load();
    desc_load();
    ime_guard();
    if (g_warp_id >= 0) {
        /* after ime_guard(): the WM_APP handler only exists once our window
         * proc is installed */
        char wp[MAX_PATH];
        cfg_path(wp, sizeof wp);
        WritePrivateProfileStringA("debug", "warptest", "-1", wp);
        warp_test_fire();
    }
    if (g_roomwarp_cell >= 0) {
        char wp[MAX_PATH];
        cfg_path(wp, sizeof wp);
        WritePrivateProfileStringA("debug", "roomwarp", "-1", wp);
        roomwarp_fire();
    }
    if (g_wb_zone >= 0 && g_wb_room >= 0) {
        char wp[MAX_PATH];
        cfg_path(wp, sizeof wp);
        WritePrivateProfileStringA("debug", "warpbyid", "", wp);
        warpbyid_fire();
    }
    if (g_gtv[1]) {
        char p[MAX_PATH];
        give_request(g_gtv[0], (int)g_gtv[1], (int)g_gtv[2]);
        cfg_path(p, sizeof p);
        WritePrivateProfileStringA("debug", "givetest", "0", p);
        logmsg("givetest: request posted for type=%u count=%u ground=%u",
               g_gtv[0], g_gtv[1], g_gtv[2]);
    }
    hook_steam_write();
    patch_steam_wrapper();
    patch_speed();
    patch_attack_speed();
    logmsg("CoreStart: creating worker");
    g_worker = CreateThread(NULL, 0, worker, NULL, 0, NULL);
    g_netsync = CreateThread(NULL, 0, net_sync_thread, NULL, 0, NULL);
    g_godguard = CreateThread(NULL, 0, god_guard_thread, NULL, 0, NULL);
    logmsg("CoreStart pid=%lu worker=%p netsync=%p godguard=%p", GetCurrentProcessId(),
           (void *)g_worker, (void *)g_netsync, (void *)g_godguard);
    return g_worker != NULL;
}

EXP void CoreStop(void)
{
    logmsg("CoreStop requested");
    g_stopping = 1;
    if (g_netsync) {
        WaitForSingleObject(g_netsync, 1000);
        CloseHandle(g_netsync);
        g_netsync = NULL;
    }
    if (g_godguard) {
        WaitForSingleObject(g_godguard, 1000);
        CloseHandle(g_godguard);
        g_godguard = NULL;
    }
    hooks_stop();               /* no callback may run while we unpatch below */
    if (g_wtid) PostThreadMessageW(g_wtid, WM_QUIT, 0, 0);
    if (g_worker) {
        WaitForSingleObject(g_worker, 3000);
        CloseHandle(g_worker);
        g_worker = NULL;
    }
    ime_release();
    bp_remove();
    unhook_steam_write();
    if (g_veh_crash) { RemoveVectoredExceptionHandler(g_veh_crash); g_veh_crash = NULL; }
    if (g_watch_addr || g_watch_ent_off || g_watch_net) {
        set_dr_all(0, 0);
        g_watch_addr = 0;
        g_watch_ent_off = 0;
    }
    if (g_veh) { RemoveVectoredExceptionHandler(g_veh); g_veh = NULL; }
    logmsg("stop: unpatch %d sites", g_npatch);
    unpatch_all();
    logmsg("stop: tamper");
    if (NEUTRALISE_FLAG) restore_tamper();
    logmsg("stop: dr/veh");
    if (DIAG_WP) {
        set_dr_all(0, 0);
        if (g_veh) { RemoveVectoredExceptionHandler(g_veh); g_veh = NULL; }
    }
    logmsg("CoreStop done");
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(h);
    {
        char exe[MAX_PATH], *s;
        GetModuleFileNameA(NULL, exe, MAX_PATH);
        s = strrchr(exe, '\\');
        if (s) *s = 0;
        _snprintf(g_dir, sizeof g_dir, "%s", exe);
        _snprintf(g_logpath, sizeof g_logpath, "%s\\unepic_mod.log", exe);
        _snprintf(g_ini, sizeof g_ini, "%s\\unepic_mod.ini", exe);
    }
    return TRUE;
}
