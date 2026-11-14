// 符文升级（自动合成）—— 实现。地址均为 3.3.94012 原映像偏移。
//
// 原版事实（读码 + 真机 dump）：
//   符文与宝石在同一张表（gems）：符文码 r01..r33 = 1..33 号；宝石码 g{品质}{种类}，品质 c = 碎裂 / f = 裂开的 /
//     s = 普通 / z = 无瑕 / p = 完美，种类 v 紫 b 蓝 g 绿 r 红 w 白(钻石) y 黄(黄玉)；骷髅 sk{c,f,u,l,z}。
//   物品表 = 数据表 +0x15c0（行数 +0x15c8，每行 0x1c0），+0x80 物品码（3 字节）。物品类号 = 行号。
//   物品单位：+4 类号、+8 id、+0xc 模式（0 存放 / 4 光标）、+0x10 数据（+0x55 页 0 背包 / 3 魔盒 / 4 储藏箱、
//     +0x9c 高级仓库叠数、+0xb0 下一件）、+0x38 位置（+0x10 x、+0x14 y）、+0x90 自己的容器。
//   拿一件到光标 = 0x19 [物品 id][u32 x][u32 y][u32 页]（17 字节）。
//   放进容器 = 原版 main+0x3bdb10(物品, 目标单位, 页 3 = 魔盒, 0xff, 0, {0})，自己找空位并发出 0x18。
//   合成 = 0x20 [1..4 魔盒 id][5 页][6 格][7 原料数 − 1] + 每 5 字节 {物品 id u32, 格字节（低 4 位 x、高 4 位 y）}。
//     真机实测 3 颗 r07 的包长 23 = 8 + 3×5，服务端 0x6e8938–0x6e8b0c 逐项核对、对不上静默丢。
//     [5] / [6] 的含义还没定（页 / 格），先按页 3、格 0 发；diag.c 的「合成读数」行会打出真实值供对照。
//   每轮只补差额：魔盒里已有的本轮原料（含上一轮的产物）不重复搬，凑够就发 0x20。
//
// Rune upgrading (automatic crafting) - implementation. Addresses are offsets in the 3.3.94012 original image.
//
// Original-game facts (code reading + real-device dumps):
//   Runes and gems share one table (gems): rune code r01..r33 = numbers 1..33; gem code g{quality}{kind}, quality
//     c = chipped / f = flawed / s = standard / z = flawless / p = perfect, kind v amethyst b sapphire g emerald
//     r ruby w diamond y topaz; skulls sk{c,f,u,l,z}.
//   Item table = data table +0x15c0 (row count +0x15c8, 0x1c0 per row), +0x80 item code (3 bytes). Class id = row number.
//   Item unit: +4 class id, +8 id, +0xc mode (0 stored / 4 cursor), +0x10 data (+0x55 page 0 inventory / 3 cube /
//     4 stash, +0x9c advanced-stash stack count, +0xb0 next), +0x38 position (+0x10 x, +0x14 y), +0x90 own container.
//   Pick one to the cursor = 0x19 [item id][u32 x][u32 y][u32 page] (17 bytes).
//   Put into a container = original main+0x3bdb10(item, target unit, page 3 = cube, 0xff, 0, {0}); it finds a free cell and sends 0x18 itself.
//   Transmute = 0x20 [1..4 cube id][5 page][6 cell][7 ingredient count - 1] + every 5 bytes {item id u32, cell byte (low 4 bits x, high 4 bits y)}.
//     Measured on device: three r07 runes give a 23-byte packet = 8 + 3x5; the server checks 0x6e8938-0x6e8b0c entry by entry and drops silently on mismatch.
//     The meaning of [5] / [6] is not settled (page / cell); they go out as page 3 / cell 0 for now, and the 「合成读数」(transmute readout) line in diag.c prints the real values.
//   Each round only tops up the shortfall: materials already in the cube (including the previous round's product) are not moved again; once complete, 0x20 goes out.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dm.h"
#include "dm_cursor.h"
#include "dm_i18n.h"
#include "dm_runeup.h"

#define PAGE(d) (*(uint8_t *)((d) + 0x55))
enum { PG_INV = 0, PG_CUBE = 3, PG_STASH = 4 };

static uint8_t *rb(void) { static uint8_t *b; if (!b) for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) b = (uint8_t *)g_mod[i].base; return b; }
static uint64_t player(uint8_t *b) { return ((uint64_t (*)(uint32_t))(b + 0x45f9d0))(*(uint32_t *)(b + 0x2d20e4c)); }
static uint64_t unit_any(uint8_t *b, uint32_t id, int type) { return ((uint64_t (*)(uint64_t, uint32_t, uint32_t, uint32_t))(b + 0x45f5f0))((uint64_t)(uintptr_t)(b + 0x2d603a0 + type * 0x400), id & 0x7f, id, 4); }
static uint32_t shared_unit_id(uint8_t *b) {   // 共享仓库当前页的单位 id（同 dm_runeword.c） | unit id of the shared stash's current page (same as dm_runeword.c)
    uint64_t m = *(uint64_t *)(b + 0x2f05450), bk = m ? *(uint64_t *)(m + 8) : 0, n = m ? *(uint64_t *)m : 0; if (!bk || !n) return 0;
    const uint64_t key = 0x8d4d18a675312bd4ull; uint64_t node = *(uint64_t *)(bk + 8 * ((n - 1) & 0x1f864eb9001295cdull));
    for (int g = 0; node && g < 64; g++, node = *(uint64_t *)node) if (*(uint64_t *)(node + 8) == key) { uint64_t a1 = *(uint64_t *)(node + 0x10), a2 = a1 ? *(uint64_t *)(a1 + 0x10) : 0; return a2 ? *(uint32_t *)a2 : 0; }
    return 0;
}
static void owners(uint8_t *b, uint64_t pl, uint64_t us[3]) { us[0] = pl; us[1] = ((uint64_t (*)(uint64_t))(b + 0x652650))(pl); uint32_t sid = shared_unit_id(b); us[2] = sid ? unit_any(b, sid, 0) : 0;
    if (!us[2]) us[2] = ((uint64_t (*)(uint64_t))(b + 0x652780))(pl); if (us[2] == us[1] || us[2] == pl) us[2] = 0; }
static uint64_t first_item(uint64_t u) { uint64_t c = u ? *(uint64_t *)(u + 0x90) : 0; return c && *(uint32_t *)c == 0x01020304 ? *(uint64_t *)(c + 0x10) : 0; }
static uint64_t tables(uint8_t *b, uint64_t it) { uint8_t v = *(uint8_t *)(it + 0x1bd); return v >= 1 && v <= 3 ? *(uint64_t *)(b + 0x2d64a18 + v * 16) : 0; }
static void send_pkt(uint8_t *b, const uint8_t *p, int n) { ((void (*)(const uint8_t *, int))(b + 0x2ceab0))(p, n); }
static int grid_xy(uint64_t it, int *x, int *y) { uint64_t pp = *(uint64_t *)(it + 0x38); if (!pp) return 0; *x = *(int32_t *)(pp + 0x10); *y = *(int32_t *)(pp + 0x14); return 1; }
static uint32_t item_code(uint8_t *b, uint64_t it) { return ((uint32_t (*)(uint64_t))(b + 0x570260))(it); }
static uint64_t held_item(uint64_t pl) {   // 光标上那件（物品栏 +0x40，模式 4） | the item on the cursor (inventory +0x40, mode 4)
    uint64_t inv = pl ? *(uint64_t *)(pl + 0x90) : 0, h = inv && *(uint32_t *)inv == 0x01020304 ? *(uint64_t *)(inv + 0x40) : 0;
    return h && *(uint32_t *)h == 4 ? h : 0;
}
void dm_pouch_notice(const char *text);   // 宿主提示通道（dm_pouch.c） | host notice channel (dm_pouch.c)
static void rune_name(uint8_t *b, uint64_t t, int no, char *out, size_t n);   // 定义在文件末尾，这里先声明 | defined at the end of the file
static const char *loc_key(uint8_t *b, const char *code) { struct { const char *p; uint64_t k; } q = { code, 3 }; const char *s = ((const char *(*)(void *))(b + 0x7f5630))(&q); return s && *s ? s : code; }

// ---------- 配方表（原版 gems 表的符文升级段） ----------
// 目标号 → {原料号, 原料数, 宝石码}；1 号没有配方（El 只能掉）
// target -> {source number, source count, gem code}; number 1 has no recipe (El only drops)
typedef struct { uint8_t from, n; char gem[4]; } recipe_t;
static const recipe_t RCP[34] = {
    [2] = { 1, 3, "" }, [3] = { 2, 3, "" }, [4] = { 3, 3, "" }, [5] = { 4, 3, "" }, [6] = { 5, 3, "" },
    [7] = { 6, 3, "" }, [8] = { 7, 3, "" }, [9] = { 8, 3, "" }, [10] = { 9, 3, "" },            // El→Thul：3 个同号 | 3 of a kind
    [11] = { 10, 3, "gcy" }, [12] = { 11, 3, "gcv" }, [13] = { 12, 3, "gcb" }, [14] = { 13, 3, "gcr" },
    [15] = { 14, 3, "gcg" }, [16] = { 15, 3, "gcw" },                                          // Thul→Pul：3 个 + 碎裂宝石 | 3 plus a chipped gem
    [17] = { 16, 3, "gfy" }, [18] = { 17, 3, "gfv" }, [19] = { 18, 3, "gfb" }, [20] = { 19, 3, "gfr" },
    [21] = { 20, 3, "gfg" },                                                                   // Lum→Um：3 个 + 裂开的宝石 | 3 plus a flawed gem
    [22] = { 21, 2, "gfw" }, [23] = { 22, 2, "gsy" }, [24] = { 23, 2, "gsv" }, [25] = { 24, 2, "gsb" },
    [26] = { 25, 2, "gsr" }, [27] = { 26, 2, "gsg" }, [28] = { 27, 2, "gsw" },                  // Pul→Lo：2 个 + 普通宝石 | 2 plus a standard gem
    [29] = { 28, 2, "gly" }, [30] = { 29, 2, "gzv" }, [31] = { 30, 2, "glb" }, [32] = { 31, 2, "glr" },
    [33] = { 32, 2, "glg" },                                                                    // Sur→Zod：2 个 + 无瑕宝石 | 2 plus a flawless gem
};

// ---------- 码 → 类号（首次用到时扫一遍物品表） ----------
static int g_cls_rune[34];
static int g_cls_gem[40]; static char g_gem_code[40][4]; static int g_cls_gem_n;
static uint64_t g_tbl_seen;

static int cls_of_code(uint64_t t, const char *code) {
    uint64_t ir = t ? *(uint64_t *)(t + 0x15c0) : 0; uint32_t in = t ? *(uint32_t *)(t + 0x15c8) : 0; if (!ir) return 0;
    for (uint32_t i = 0; i < in; i++) if (!memcmp((const void *)(ir + (uint64_t)i * 0x1c0 + 0x80), code, 3)) return (int)i;
    return 0;
}
static void build_cls_cache(uint8_t *b, uint64_t t) {
    (void)b; if (!t || g_tbl_seen == t) return;
    memset(g_cls_rune, 0, sizeof g_cls_rune); memset(g_cls_gem, 0, sizeof g_cls_gem); g_cls_gem_n = 0;
    for (int i = 1; i <= 33; i++) { char c[4] = { 'r', (char)('0' + i / 10), (char)('0' + i % 10), 0 }; g_cls_rune[i] = cls_of_code(t, c); }
    for (int i = 2; i <= 33; i++) { const char *g = RCP[i].gem; if (!g[0]) continue; int have = 0;
        for (int k = 0; k < g_cls_gem_n; k++) if (!memcmp(g_gem_code[k], g, 3)) { have = 1; break; }
        if (!have && g_cls_gem_n < 40) { memcpy(g_gem_code[g_cls_gem_n], g, 4); g_cls_gem[g_cls_gem_n] = cls_of_code(t, g); g_cls_gem_n++; } }
    g_tbl_seen = t;
    LOG("符文升级：类号表建好（1 号=%d、2 号=%d、33 号=%d；%d 种宝石）", g_cls_rune[1], g_cls_rune[2], g_cls_rune[33], g_cls_gem_n);
}
static int gem_idx(const char *code) { for (int k = 0; k < g_cls_gem_n; k++) if (!memcmp(g_gem_code[k], code, 3)) return k; return -1; }
int dm_runeup_rune_no(int cls) {
    uint8_t *b = rb(); if (!b) return 0; uint64_t pl = player(b); uint64_t it = pl ? first_item(pl) : 0; if (!it) return 0;
    build_cls_cache(b, tables(b, it));
    for (int i = 1; i <= 33; i++) if (g_cls_rune[i] == cls) return i;
    return 0;
}

// ---------- 库存 ----------
typedef struct { int rune[34]; int gem[40]; } inv_t;
static void scan_inv(uint8_t *b, uint64_t pl, inv_t *v) {   // 只算现在拿得到的：背包 / 魔盒 / 个人储藏箱（箱开着）/ 高级仓库成叠 / 共享仓库 | only what is reachable now
    memset(v, 0, sizeof *v); if (!pl) return;
    int stash_open = *(uint8_t *)(b + 0x2d5ff60) != 0; uint64_t us[3]; owners(b, pl, us);
    for (int o = 0; o < 3; o++) for (uint64_t it = first_item(us[o]); it; ) { uint64_t d = *(uint64_t *)(it + 0x10); if (!d) break;
        if (*(uint32_t *)(it + 0xc) == 0) { int pg = PAGE(d), st = *(uint8_t *)(d + 0x9c);
            if (o || pg == PG_INV || pg == PG_CUBE || (pg == PG_STASH && stash_open)) {
                int cls = (int)*(uint32_t *)(it + 4), cnt = st ? st : 1;
                for (int i = 1; i <= 33; i++) if (g_cls_rune[i] == cls) { v->rune[i] += cnt; break; }
                for (int k = 0; k < g_cls_gem_n; k++) if (g_cls_gem[k] == cls) { v->gem[k] += cnt; break; } } }
        it = *(uint64_t *)(d + 0xb0); }
}

// ---------- 计划 ----------
typedef struct { int craft[34]; int need_gem[40]; int short_rune, short_cnt, short_gem; } plan_t;
static int make_plan(uint8_t *b, uint64_t t, int to, plan_t *p) {
    (void)t; memset(p, 0, sizeof *p); p->short_rune = -1; p->short_gem = -1;
    if (to < 1 || to > 33) return 1;
    inv_t have; scan_inv(b, player(b), &have);
    int need[34]; memset(need, 0, sizeof need); need[to] = 1;
    for (int i = to; i >= 2; i--) {
        int d = need[i] - have.rune[i]; if (d < 0) d = 0;
        p->craft[i] = d;
        if (!d) continue;
        if (!RCP[i].from) { p->short_rune = i; p->short_cnt = d; return 1; }
        need[i - 1] += d * RCP[i].n;
        if (RCP[i].gem[0]) { int gi = gem_idx(RCP[i].gem); if (gi >= 0) p->need_gem[gi] += d; }
    }
    if (need[1] > have.rune[1]) { p->short_rune = 1; p->short_cnt = need[1] - have.rune[1]; }
    for (int k = 0; k < g_cls_gem_n; k++) if (p->need_gem[k] > have.gem[k]) { p->short_gem = k; p->short_cnt = p->need_gem[k] - have.gem[k]; }
    return (p->short_rune >= 0 || p->short_gem >= 0) ? 1 : 0;
}

// ---------- 魔盒 ----------
static uint64_t find_cube(uint8_t *b, uint64_t pl, uint32_t *id) {
    *id = 0; uint64_t it = first_item(pl);
    for (int c = 0; it && c < 400; c++) { uint64_t d = *(uint64_t *)(it + 0x10); if (!d) break;
        uint32_t cd = item_code(b, it); if (!memcmp(&cd, "box", 3)) { *id = *(uint32_t *)(it + 8); return it; }
        it = *(uint64_t *)(d + 0xb0); }
    return 0;
}
static int cube_others(uint8_t *b, uint64_t pl) {   // 魔盒里非符文 / 非宝石的件数（别的配方材料会把这次合成抢走） | count of non-rune/non-gem items in the cube
    int n = 0; uint64_t it = first_item(pl);
    for (int c = 0; it && c < 400; c++) { uint64_t d = *(uint64_t *)(it + 0x10); if (!d) break;
        if (PAGE(d) == PG_CUBE) { uint32_t cd = item_code(b, it); const char *s = (const char *)&cd;
            int is_rune = s[0] == 'r' && s[1] >= '0' && s[1] <= '9';
            int is_gem = (s[0] == 'g' && strchr("cfszp", s[1])) || (s[0] == 's' && s[1] == 'k');
            if (!is_rune && !is_gem) n++; }
        it = *(uint64_t *)(d + 0xb0); }
    return n;
}
static int cube_of_cls(uint8_t *b, uint64_t pl, int cls) {   // 魔盒里这类号有几件 | how many of this class the cube holds
    int n = 0; uint64_t it = first_item(pl);
    for (int c = 0; it && c < 400; c++) { uint64_t d = *(uint64_t *)(it + 0x10); if (!d) break;
        if (PAGE(d) == PG_CUBE && (int)*(uint32_t *)(it + 4) == cls) n++;
        it = *(uint64_t *)(d + 0xb0); }
    return n;
}

// ---------- 挑一颗料（不在魔盒里的） ----------
// kind: 0 格子（背包 / 个人储藏箱）/ 1 共享仓库 / 2 高级仓库成叠
static int pick_material(uint8_t *b, uint64_t pl, int cls, int *kind, int *page, int *x, int *y, uint32_t *id) {
    int stash_open = *(uint8_t *)(b + 0x2d5ff60) != 0; uint64_t us[3]; owners(b, pl, us);
    for (int rank = 0; rank < 4; rank++) for (int o = 0; o < 3; o++) for (uint64_t it = first_item(us[o]); it; ) { uint64_t d = *(uint64_t *)(it + 0x10); if (!d) break;
        if ((int)*(uint32_t *)(it + 4) == cls && *(uint32_t *)(it + 0xc) == 0) {
            int pg = PAGE(d), st = *(uint8_t *)(d + 0x9c), r = -1;
            if (o == 0 && pg == PG_INV) r = 0;
            else if (o == 0 && pg == PG_STASH && stash_open) r = 1;
            else if (st && o == 0) r = 2;
            else if (o) r = 3;
            if (r == rank) { *kind = st ? 2 : o ? 1 : 0; *page = pg; *id = *(uint32_t *)(it + 8); grid_xy(it, x, y); return 1; } }
        it = *(uint64_t *)(d + 0xb0); }
    return 0;
}

// ---------- 状态机 ----------
// st: 1 = 备料（凑够就发 0x20）/ 2 = 等光标 / 5 = 等结果
typedef struct {
    int st, to, m, want_rune, want_gem, wait;
    uint32_t cube;
    uint32_t pid;   // 正在搬的这一件 | the piece being moved
} job_t;
static job_t g_up;
int dm_runeup_busy(void) { return g_up.st != 0; }

static uint64_t up_tables(uint8_t *b, uint64_t pl) { uint64_t it = pl ? first_item(pl) : 0; return it ? tables(b, it) : 0; }

static void up_fail(const char *why) {
    if (g_up.st) { LOG("【红】符文升级：停（%s；造到 %d 号，目标 %d 号）", why, g_up.m, g_up.to);
        char line[200]; snprintf(line, sizeof line, "符文合成停了：%s", why); dm_pouch_notice(line); }
    g_up.st = 0;
}

void dm_runeup_pump(void) {
    if (!g_up.st) return;
    uint8_t *b = rb(); if (!b) { g_up.st = 0; return; }
    uint64_t pl = player(b); if (!pl) { up_fail("没有玩家"); return; }
    uint64_t cube_it = find_cube(b, pl, &g_up.cube); if (!cube_it) { up_fail("魔盒不在身上"); return; }
    build_cls_cache(b, tables(b, cube_it));
    int cls_from = g_cls_rune[RCP[g_up.m].from];
    int gi = RCP[g_up.m].gem[0] ? gem_idx(RCP[g_up.m].gem) : -1;
    int cls_gem = gi >= 0 ? g_cls_gem[gi] : 0;

    if (g_up.st == 2) {   // 等光标拿到 → 放进魔盒 | wait for the cursor, then put it into the cube
        uint64_t h = held_item(pl);
        if (!h) { if (++g_up.wait > 60) up_fail("拿料超时"); return; }
        if (*(uint32_t *)(h + 8) != g_up.pid) { g_up.st = 1; return; }
        typedef int (*putfn)(uint64_t, uint64_t, int, int, int, void *);
        int out = 0; int r = ((putfn)(b + 0x3bdb10))(h, pl, PG_CUBE, 0xff, 0, &out) & 1;
        LOG("符文升级：%u 放进魔盒 → %s", g_up.pid, r ? "已放" : "放不下");
        if (!r) { up_fail("魔盒放不下"); return; }
        g_up.st = 1; g_up.wait = 0; return;
    }

    if (g_up.st == 1) {
        int have_r = cube_of_cls(b, pl, cls_from), have_g = gi >= 0 ? cube_of_cls(b, pl, cls_gem) : 0;
        if (have_r < g_up.want_rune || have_g < g_up.want_gem) {   // 还缺 → 再搬一件 | still short -> move one more piece
            int cls = have_r < g_up.want_rune ? cls_from : cls_gem;
            int kind = 0, page = 0, x = 0, y = 0; uint32_t id = 0;
            if (!pick_material(b, pl, cls, &kind, &page, &x, &y, &id)) { up_fail("库存里挑不到料了"); return; }
            if (kind == 2) { LOG("【红】符文升级：料在高级仓库成叠（造 %d 号要 %d 号），这一版还没接成叠取件", g_up.m, RCP[g_up.m].from); up_fail("成叠来源未支持"); return; }
            uint8_t p[17]; memset(p, 0, sizeof p); p[0] = 0x19;
            uint32_t v[4] = { id, (uint32_t)x, (uint32_t)y, (uint32_t)page }; memcpy(p + 1, v, 16); send_pkt(b, p, 17);
            g_up.pid = id;
            LOG("符文升级：搬料 id %u（%d,%d 页 %d）→ 魔盒里符文 %d/%d、宝石 %d/%d", id, x, y, page, have_r, g_up.want_rune, have_g, g_up.want_gem);
            g_up.st = 2; g_up.wait = 0; return;
        }
        // 料齐 → 发 0x20 | materials complete -> send 0x20
        int total = g_up.want_rune + (gi >= 0 ? 1 : 0);
        uint8_t p[8 + 5 * 4]; memset(p, 0, sizeof p); uint32_t cid = g_up.cube; memcpy(p + 1, &cid, 4);
        p[5] = PG_CUBE; p[6] = 0; p[7] = (uint8_t)(total - 1);
        int n = 0; uint64_t q = first_item(pl);
        for (int c = 0; q && c < 400 && n < total; c++) { uint64_t d = *(uint64_t *)(q + 0x10); if (!d) break;
            if (PAGE(d) == PG_CUBE) { int cl = (int)*(uint32_t *)(q + 4);
                if (cl == cls_from || (gi >= 0 && cl == cls_gem)) { int x = 0, y = 0; grid_xy(q, &x, &y);
                    uint32_t iid = *(uint32_t *)(q + 8); memcpy(p + 8 + 5 * n, &iid, 4); p[8 + 5 * n + 4] = (uint8_t)((x & 15) | ((y & 15) << 4)); n++; } }
            q = *(uint64_t *)(d + 0xb0); }
        if (n < total) { up_fail("魔盒里的料数不对"); return; }
        send_pkt(b, p, 8 + 5 * n);
        LOG("符文升级：发合成 0x20（魔盒 id%u，%d 项，包长 %d，页 %u 格 %u）→ 等结果", g_up.cube, n, 8 + 5 * n, p[5], p[6]);
        g_up.st = 5; g_up.wait = 0; return;
    }

    if (g_up.st == 5) {   // 等结果：产物出现 = 成了 | wait: the product showing up means it worked
        if (++g_up.wait > 200) { up_fail("发合成后 3 秒魔盒没变"); return; }
        if (g_up.wait < 5) return;
        if (cube_of_cls(b, pl, g_cls_rune[g_up.m]) < 1) return;
        LOG("符文升级：造出 1 颗 %d 号（魔盒里 %d 颗）", g_up.m, cube_of_cls(b, pl, g_cls_rune[g_up.m]));
        inv_t v; scan_inv(b, pl, &v);
        if (v.rune[g_up.to] >= 1) { LOG("符文升级：%d 号到手，收工", g_up.to); g_up.st = 0;
            { char rn[64] = ""; rune_name(b, tables(b, cube_it), g_up.to, rn, sizeof rn); char line[200]; snprintf(line, sizeof line, "符文合成完成：%s", rn); dm_pouch_notice(line); } return; }
        plan_t p; if (make_plan(b, tables(b, cube_it), g_up.to, &p)) { up_fail("材料不够了"); return; }
        int next = 0; for (int i = 2; i <= g_up.to; i++) if (p.craft[i] > 0) { next = i; break; }
        if (!next) { up_fail("计划里没有可造的号"); return; }
        g_up.m = next; g_up.want_rune = RCP[next].n; g_up.want_gem = RCP[next].gem[0] ? 1 : 0;
        g_up.st = 1; g_up.wait = 0;
        LOG("符文升级：下一轮造 %d 号（%d 颗 %d 号%s）", next, g_up.want_rune, RCP[next].from, g_up.want_gem ? " + 宝石" : "");
        return;
    }

    g_up.st = 0;
}

int dm_runeup_check(int rune_no, char *why, int cap) {
    uint8_t *b = rb(); if (!b) return 1; uint64_t pl = player(b); if (!pl) return 1;
    uint64_t t = up_tables(b, pl); build_cls_cache(b, t);
    plan_t p; if (!make_plan(b, t, rune_no, &p)) { if (why && cap > 0) why[0] = 0; return 0; }
    if (why && cap > 0) {
        char a[128] = "", c[128] = "";
        if (p.short_rune >= 0) { char c2[4] = { 'r', (char)('0' + p.short_rune / 10), (char)('0' + p.short_rune % 10), 0 };
            snprintf(a, sizeof a, dm_ui("dmRwNeedRune"), loc_key(b, c2), p.short_cnt); }
        if (p.short_gem >= 0) snprintf(c, sizeof c, dm_ui("dmRwNeedGem"), loc_key(b, g_gem_code[p.short_gem]), p.short_cnt);
        snprintf(why, cap, "%s%s%s", a, a[0] && c[0] ? " / " : "", c);
    }
    return 1;
}

int dm_runeup_start(int rune_no, char *why, int cap) {
    uint8_t *b = rb(); if (!b) return 1; uint64_t pl = player(b); if (!pl) return 1;
    uint32_t cid = 0; uint64_t cube = find_cube(b, pl, &cid); if (!cube) { if (why) snprintf(why, cap, "%s", dm_ui("dmRwNoCube")); return 1; }
    build_cls_cache(b, tables(b, cube));
    if (cube_others(b, pl)) { if (why) snprintf(why, cap, "%s", dm_ui("dmRwCubeBusy")); return 1; }
    plan_t p; if (make_plan(b, tables(b, cube), rune_no, &p)) { if (why) snprintf(why, cap, "%s", dm_ui("dmRwNoMat")); return 1; }
    memset(&g_up, 0, sizeof g_up); g_up.to = rune_no; g_up.cube = cid;
    int first = 0; for (int i = 2; i <= rune_no; i++) if (p.craft[i] > 0) { first = i; break; }
    if (!first) { if (why) snprintf(why, cap, "%s", dm_ui("dmRwNoMat")); return 1; }
    g_up.m = first; g_up.want_rune = RCP[first].n; g_up.want_gem = RCP[first].gem[0] ? 1 : 0; g_up.st = 1; g_up.wait = 0;
    LOG("符文升级：开始补 %d 号（第一轮造 %d 号，要 %d 颗 %d 号%s，魔盒 id%u）", rune_no, first, g_up.want_rune, RCP[first].from, g_up.want_gem ? " + 宝石" : "", cid);
    return 0;
}


static void rune_name(uint8_t *b, uint64_t t, int no, char *out, size_t n) {   // 符文名（原版串） | rune name (original game string)
    out[0] = 0; if (no < 1 || no > 33 || !t) return;
    uint64_t ir = *(uint64_t *)(t + 0x15c0); uint32_t in = *(uint32_t *)(t + 0x15c8); int cls = g_cls_rune[no];
    if (!ir || cls < 0 || (uint32_t)cls >= in) return;
    const char *c = (const char *)(ir + (uint64_t)cls * 0x1c0 + 0x80);
    struct { const char *p; uint64_t k; } q = { c, 3 }; const char *s = ((const char *(*)(void *))(b + 0x7f5630))(&q);
    snprintf(out, n, "%s", s && *s ? s : c);
}

// ---------- 符文页空格：点一下记下、点第二下开始合成 ----------
// Rune page empty cell: first tap arms it, the second tap starts crafting
static int g_tap_no;
int dm_runeup_empty(int rune_no) {   // 这一号在手上/囊里/仓库一颗都没有（=符文页那格是空的） | no rune of this number anywhere reachable (= that page cell is empty)
    uint8_t *b = rb(); if (!b || rune_no < 1 || rune_no > 33) return 0;
    uint64_t pl = player(b); if (!pl) return 0;
    build_cls_cache(b, up_tables(b, pl));
    inv_t v; scan_inv(b, pl, &v);
    return v.rune[rune_no] == 0;
}
int dm_runeup_tap_empty(int rune_no) {   // 返回 1 = 这一下吃掉（第一次提示 / 开始合成 / 正在跑） | 1 = swallow this tap
    uint8_t *b = rb(); if (!b) return 0;
    if (g_up.st) { LOG("符文升级：合成还在跑，这次点击不动"); return 1; }
    char rn[64] = ""; uint64_t pl = player(b), t = pl ? up_tables(b, pl) : 0;
    rune_name(b, t, rune_no, rn, sizeof rn);
    if (g_tap_no != rune_no) {
        g_tap_no = rune_no;
        LOG("符文升级：点了空格 %d 号（%s）→ 记下，再点一次就开始合成", rune_no, rn);
        { char line[200]; snprintf(line, sizeof line, "再点一次合成 %s", rn); dm_pouch_notice(line); }
        return 1; }
    g_tap_no = 0;
    char why[200] = "";
    int r = dm_runeup_start(rune_no, why, sizeof why);
    LOG("%s符文升级：点空格 %d 号（%s）→ %s", r ? "【红】" : "", rune_no, rn, r ? why : "开始合成");
    { char line[240]; snprintf(line, sizeof line, r ? "符文合成没开始：%s" : "开始合成 %s", r ? why : rn); dm_pouch_notice(line); }
    return 1;
}

// 符文合成按钮用：从 2 号往上逐号算「还能合几个」，只写 >0 的号（缺的号不列，列表短）
// For the rune-craft button: from No.2 upward, how many of each can still be made; only numbers with >0 are written (missing ones are left out, keeping the list short)
void dm_runeup_list_text(char *out, int cap) {
    out[0] = 0; uint8_t *b = rb(); if (!b || cap < 8) return;
    uint64_t pl = player(b); if (!pl) return;
    build_cls_cache(b, up_tables(b, pl));
    inv_t v; scan_inv(b, pl, &v);
    int gem[40]; for (int k = 0; k < g_cls_gem_n && k < 40; k++) gem[k] = v.gem[k];
    int n = 0; char rn[64];
    for (int no = 2; no <= 33; no++) {
        int need = RCP[no].n, from = RCP[no].from, cnt = v.rune[from] / need;
        if (RCP[no].gem[0]) { int gi = gem_idx(RCP[no].gem); if (gi >= 0 && gem[gi] < cnt) cnt = gem[gi]; }
        if (cnt <= 0) continue;
        v.rune[from] -= cnt * need; v.rune[no] += cnt;
        if (RCP[no].gem[0]) { int gi = gem_idx(RCP[no].gem); if (gi >= 0) gem[gi] -= cnt; }
        rune_name(b, up_tables(b, pl), no, rn, sizeof rn);
        n += snprintf(out + n, cap - n, "%s%s ×%d", n ? "  " : "", rn, cnt);
        if (n >= cap - 16) break;
    }
    if (!n) snprintf(out, cap, "%s", dm_ui("dmRwNoMat"));
}

// 符文合成弹框用：每号「还能合几个」，只回 >0 的；返回条数（最多 max 条）
// For the rune-craft popup: how many of each rune number can still be made; only >0 entries; returns the count (at most max)
int dm_runeup_list(int *no_out, int *cnt_out, int max) {
    uint8_t *b = rb(); if (!b || !no_out || !cnt_out || max < 1) return 0;
    uint64_t pl = player(b); if (!pl) return 0;
    build_cls_cache(b, up_tables(b, pl));
    inv_t v; scan_inv(b, pl, &v);
    int n = 0;
    for (int no = 2; no <= 33 && n < max; no++) {
        if (!RCP[no].from) continue;
        // 逐层算「缺口」：某一层现货够就不往下要（和 make_plan 一模一样），所以手上已有的 19# 会抵掉 18# 的需求。
        // Layer-by-layer deficit: if a layer's stock covers it, nothing is demanded from below (exactly what make_plan does),
        // so runes already in hand at the top of the chain offset the demand for the lower ones.
        int need[34]; memset(need, 0, sizeof need); need[no] = 1;
        for (int i = no; i >= 2; i--) {
            if (!need[i] || !RCP[i].from) continue;
            int def = need[i] - v.rune[i]; if (def < 0) def = 0;
            if (def) need[i - 1] += def * RCP[i].n;
        }
        int cnt = 9999, ok = 0;
        for (int i = 1; i <= no; i++) {
            if (!need[i]) continue; ok = 1; int q = v.rune[i] / need[i]; if (q < cnt) cnt = q;
            if (RCP[i].gem[0]) { int gi = gem_idx(RCP[i].gem); if (gi < 0 || v.gem[gi] < 1) { cnt = 0; break; } }   // 这一层要的宝石一颗都没有 → 合不了 | no gem at all for this layer -> cannot craft
        }
        if (!ok || cnt < 1) continue;
        no_out[n] = no; cnt_out[n] = cnt; n++;
    }
    return n;
}
