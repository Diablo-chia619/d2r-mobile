package com.xiaoboluo.d2r;

// 佣兵名字（mercenaries.json 的 merc01..merc41，取 zhCN 简体名；zhTW 是繁体，逐字转换会对不上，
// 例如 merc03 简体「安诺儿」繁体「安諾」）。单位 +0xb0 那个小整数是否等于名表序号仍待验证。
// Mercenary names (merc01..merc41 in mercenaries.json, zhCN simplified; zhTW differs per-word, e.g. merc03 is
// 安诺儿 vs 安諾). Whether the small int at unit +0xb0 is the ordinal is still unverified.
final class MercNames {
    private MercNames() { }
    static final String[] NAMES = { "",
        "阿丽萨",
        "安普丽萨",
        "安诺儿",
        "阿巴雅",
        "艾丽",
        "派吉",
        "巴森提",
        "布蕾西",
        "京子",
        "克劳蒂亚",
        "昆德蕊",
        "凯尔",
        "维萨拉",
        "奥雷克萨",
        "弗萝利亚",
        "菲欧娜",
        "基温尼",
        "盖尔",
        "哈娜",
        "希瑟",
        "伊安萨",
        "戴安妮",
        "伊索德",
        "迪沃",
        "伊瑟拉",
        "伊童雅",
        "黎恩",
        "米克",
        "马哈拉",
        "利亚萨",
        "梅甘",
        "奥蕾娜",
        "奥利安娜",
        "瑞安",
        "罗泽妮",
        "雷萨",
        "夏琳",
        "席卡",
        "黛比",
        "泰蕾娜",
        "温迪",
    };
    static String get(int idx) { return idx >= 1 && idx < NAMES.length ? NAMES[idx] : null; }
}
