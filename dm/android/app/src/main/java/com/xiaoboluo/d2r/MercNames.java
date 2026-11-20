package com.xiaoboluo.d2r;

// 佣兵名字（data/local/lng/strings/mercenaries.json 的 merc01..merc41，就是 hireling.txt 的 NameFirst）。
// 单位 +0xb0 那个小整数是名字表里的序号（实测 271 罗格读出来是 5 -> 艾莉）。
// Mercenary names (merc01..merc41 in mercenaries.json, which hireling.txt names as NameFirst). The small int at
// unit +0xb0 is the ordinal into that table (measured: the Rogue read 5 -> 艾莉).
final class MercNames {
    private MercNames() { }
    // 索引 1 = merc01（数组下标 0 也是 merc01，顺手对齐）
    static final String[] NAMES = { "",
        "艾莉薩",
        "安普利莎",
        "安諾",
        "艾伯海雅",
        "艾莉",
        "佩吉",
        "巴森媞",
        "布萊絲",
        "京子",
        "克勞蒂亞",
        "昆德莉",
        "凱爾",
        "維莎莉亞",
        "依麗莎",
        "弗蘿利亞",
        "費歐娜",
        "吉溫妮",
        "蓋兒",
        "哈娜",
        "海瑟",
        "艾蓮娜",
        "黛安",
        "依索德",
        "迪佛",
        "依索拉",
        "伊東亞",
        "雷伊妮",
        "梅可",
        "瑪哈拉",
        "莉莎",
        "梅葛哈",
        "奧蓮娜",
        "奧麗亞娜",
        "萊恩",
        "羅珊妮",
        "萊莎",
        "夏琳",
        "席克哈",
        "戴比",
        "泰蓮娜",
        "溫蒂",
    };
    static String get(int idx) { return idx >= 1 && idx < NAMES.length ? NAMES[idx] : null; }
}
