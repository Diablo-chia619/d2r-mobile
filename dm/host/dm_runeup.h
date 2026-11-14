// 符文升级（自动合成）：符文明细不够时，点清单里缺的那颗符文，自动从库存凑料、放进魔盒、发原版合成包，
// 一路从低号合上来，直到补出这一颗。配方表按原版 gems 表（符文与宝石同表：r01..r33、gcv..skz），
// 匹配仍由服务端自己判（0x702f60），这里只负责照表放料。只在游戏主线程跑。
//
// Rune upgrading (automatic crafting): when a rune is missing, tapping that rune in the list gathers the
// materials from the player's stock, drops them into the Horadric Cube and sends the original transmute
// packet, working its way up from the lowest rune until the missing one exists. The recipe table follows the
// original game's gems table (runes and gems share it: r01..r33, gcv..skz); matching is still decided by the
// server (0x702f60) - this module only puts the right things in the cube. Game main thread only.
#ifndef DM_RUNEUP_H
#define DM_RUNEUP_H

#include <stdint.h>

// 这一号符文现在够不够（含"库存里已经有了"）：0 = 够，非 0 = 缺，why 里写缺什么（可直接上屏，UTF-8）
// 每次都重扫库存，不缓存 | Whether this rune number is affordable right now (including "already in stock"):
// 0 = enough, nonzero = short, with why holding what is missing (ready to display, UTF-8). Rescans every call.
int dm_runeup_check(int rune_no, char *why, int cap);

// 开始补这一号（材料够时才会真的动手）。0 = 已开始 / 1 = 不能开始，why 写原因
// Start filling this rune number (only acts when affordable). 0 = started / 1 = cannot start, why says why
int dm_runeup_start(int rune_no, char *why, int cap);

void dm_runeup_pump(void);   // 游戏主线程每帧 | game main thread, every frame
int dm_runeup_busy(void);    // 还在跑吗 | still running?
int dm_runeup_rune_no(int cls);   // 物品类号 → 符文号（1..33，不是符文返回 0） | item class id -> rune number (1..33, 0 when not a rune)
int dm_runeup_empty(int rune_no);   // 这一号一颗都没有（符文页那格是空的） | no rune of this number anywhere
int dm_runeup_tap_empty(int rune_no);   // 符文页空格被点了：第一下记下、第二下开始合成；返回 1 = 吃掉这次点击 | empty rune-page cell tapped

#endif
