package com.xiaoboluo.d2r;

import android.content.Context;
import android.util.Log;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.util.ArrayList;

// 佣兵说话：读 files/dm/merc_talk.txt，按事件（喝药 / 血低 / 换场景 / 怪多 / 自己快死）调 DeepSeek 生成一句台词，
// 交给 CamThreats 画在佣兵头顶。环境数字由宿主（dm_world.c g_ctx / diag.c hud_tick）提供；最近说过的五句一起塞进提示词，
// 明确要求不得重复。发版时配置里 key 留空 → ready() 为假，整块功能不联网、不说话。
// Mercenary chatter: reads files/dm/merc_talk.txt, asks DeepSeek for one line on an event (potion / low life / new
// area / many monsters / nearly dead), and hands it to CamThreats to draw above the mercenary. The numbers come from
// the host (dm_world.c g_ctx / diag.c hud_tick); the last five lines go into the prompt with an explicit instruction
// not to repeat them. In the shipped build the config's key is empty, so ready() is false and nothing goes online.
final class MercTalk {
    private static final String TAG = "mercTalk";

    private String key = "", model = "deepseek-chat", url = "https://api.deepseek.com/chat/completions";
    private int interval = 12, hold = 4, debug = 0;
    // 气泡相对头顶的上下偏移（屏幕高度比例；正数往下、负数往上），用户自己微调
    // Vertical offset of the bubble from his head as a fraction of the screen height (positive = lower)
    private float bubbleDy = 0f;
    private String tone = "", style = "", self = "佣兵", you = "主人", extra = "";
    private String line;
    private volatile long lastLineAt;
    private long lastEventAt;
    private volatile boolean busy;
    private final File cfg;

    // 环境（宿主填）：0 玩家血量‰ 1 玩家上限 2 佣兵血量‰ 3 佣兵上限 4 附近怪数 5 附近精英数 6 玩家当前血量 7 场景 id
    // Environment (filled by the host): 0 player hp per mille, 1 player max, 2 merc hp per mille, 3 merc max,
    // 4 nearby monsters, 5 nearby elites, 6 player current hp, 7 level id
    private final int[] ctx = new int[20];
    private int lastPm = -1, lastLv = -1, lastMon = -1, eliteDrawn = -1, lastMp = -1;
    private String pendingWhy;

    // 最近说过的几句（拼进提示词要求不重复） | the last few lines, put into the prompt so it does not repeat
    private final ArrayList<String> recent = new ArrayList<String>();

    MercTalk(Context c) {
        // 先用 .txt（手机文件管理器能直接编辑），没有再找 .ini（老名字），最后退回应用私有目录
        // Prefer .txt (a phone file manager can edit it), then the old .ini name, then the private folder
        File ext = c.getExternalFilesDir(null), in2 = new File(c.getFilesDir(), "dm");
        File f = null;
        for (File dir : new File[] { ext != null ? new File(ext, "dm") : null, in2 }) {
            if (dir == null) continue;
            for (String n : new String[] { "merc_talk.txt", "merc_talk.ini" }) { File t = new File(dir, n); if (t.isFile()) { f = t; break; } }
            if (f != null) break;
        }
        cfg = f != null ? f : new File(in2, "merc_talk.txt");
        load();
        Log.e(TAG, "配置载入：" + (ready() ? "已启用" : "未填 key，不启用") + " 文件=" + cfg);
    }

    private void load() {
        if (!cfg.exists()) return;
        BufferedReader r = null;
        try {
            r = new BufferedReader(new InputStreamReader(new FileInputStream(cfg), "UTF-8"));
            String ln;
            while ((ln = r.readLine()) != null) {
                ln = ln.trim();
                if (ln.isEmpty() || ln.startsWith("#") || ln.startsWith(";")) continue;
                int i = ln.indexOf('=');
                if (i <= 0) continue;
                String k = ln.substring(0, i).trim(), v = ln.substring(i + 1).trim();
                if (k.equals("key")) key = v;
                else if (k.equals("model")) model = v;
                else if (k.equals("url")) url = v;
                else if (k.equals("interval")) interval = parseInt(v, interval);
                else if (k.equals("hold")) hold = parseInt(v, hold);
                else if (k.equals("debug")) debug = parseInt(v, 0);
                else if (k.equals("bubble_dy")) bubbleDy = parseFloat(v, 0f);
                else if (k.equals("tone")) tone = v;
                else if (k.equals("style")) style = v;
                else if (k.equals("self")) self = v;
                else if (k.equals("you")) you = v;
                else if (k.equals("extra")) extra = v;
            }
        } catch (Exception e) {
            if (debug > 0) Log.w(TAG, "读配置失败：" + e);
        } finally {
            try { if (r != null) r.close(); } catch (Exception ignored) { }
        }
    }

    boolean ready() { return key != null && key.length() > 8; }

    // 每帧调一次：刷新环境、挑事件、必要时去要一句。返回当前该画的台词（没有就 null）
    // Called once per frame: refreshes the environment, picks an event, asks for a line when due. Returns the line
    // to draw (null when there is none)
    String line(int[] c) {
        if (c != null) System.arraycopy(c, 0, ctx, 0, Math.min(c.length, ctx.length));
        if (!ready()) return null;
        long now = System.currentTimeMillis();
        if (line != null && now - lastLineAt < hold * 1000L) return line;
        if (line != null) line = null;

        // 事件，优先级从高到低 | events, highest priority first
        // 喝药用「千分比涨了 6% 以上」判（按绝对值判的话高等级角色喝小药水不触发），且必须在掉血之后回血
        // The potion test uses "per mille rose by 6% or more" (an absolute threshold misses small potions on a high
        // level character) and only counts as healing after having lost life
        String why = null;
        int hp = ctx[6], pm = ctx[0], mm = ctx[2], mon = ctx[4], elite = ctx[5], lv = ctx[7], mp = ctx[14];
        if (lastPm > 0 && pm > lastPm + 60 && pm < 990) why = "刚喝完一瓶药水，血回上来了";
        else if (pm > 0 && pm < 300 && lastPm >= 0 && pm < lastPm) why = "主人血量告急，快撑不住了";
        else if (mm > 0 && mm < 300) why = "你自己（佣兵）快被打死了";
        else if (elite > 0 && eliteDrawn != elite) why = "附近有精英怪（" + elite + " 只）";
        else if (lastLv != -1 && lv != lastLv) why = "刚走到一个新地方";
        else if (mon > lastMon + 4 && mon >= 6) why = "一拥而上，怪很多（" + mon + " 只）";
        else if (lastMp > 0 && mp < 150 && mp < lastMp) why = "主人快没蓝了";
        else if (now - lastEventAt > interval * 1000L) why = "随口念叨一句";

        lastPm = pm; lastLv = lv; lastMon = mon; eliteDrawn = elite; lastMp = mp;
        if (why == null || busy) return null;
        lastEventAt = now; pendingWhy = why;
        ask();
        return null;
    }

    private void ask() {
        busy = true;
        final StringBuilder env = new StringBuilder();
        env.append("此刻：");
        if (ctx[0] > 0) env.append("玩家血量 ").append(ctx[0] / 10).append("%，");
        if (ctx[14] > 0) env.append("玩家蓝量 ").append(ctx[14] / 10).append("%，");
        if (ctx[2] > 0) env.append("你的血量 ").append(ctx[2] / 10).append("%，");
        // 附近有哪几种怪（类号查 MonNames，最多三种），模型自己决定要不要提它们、提哪只
        // Which kinds of monsters are nearby (class id looked up in MonNames, up to three); the model decides
        // whether to mention them and which one
        StringBuilder kinds = new StringBuilder();
        for (int i = 0; i < 3; i++) {
            int n = ctx[11 + i]; if (n <= 0) continue;
            String nm = MonNames.get(ctx[8 + i]);
            if (nm == null) continue;
            if (kinds.length() > 0) kinds.append("、");
            kinds.append(nm).append(" ").append(n).append(" 只");
        }
        if (kinds.length() > 0) env.append("附近有 ").append(kinds).append("，");
        else if (ctx[4] > 0) env.append("附近有 ").append(ctx[4]).append(" 只怪，");
        if (ctx[5] > 0) env.append("其中 ").append(ctx[5]).append(" 只是精英，");
        env.append("。");
        final StringBuilder hist = new StringBuilder();
        if (!recent.isEmpty()) {
            hist.append("你最近说过（不要重复、不要意思相近）：");
            for (String s : recent) hist.append("「").append(s).append("」");
            hist.append("。");
        }
        // 说话方式：不固定称呼、不固定句式，让它像真人一样在吐槽 / 提醒 / 闲聊 / 自言自语之间自己挑。
        // 用户要求「不要总是主人主人地叫」「根据游戏实时内容调整」——所以只给情境和数据，让它自己决定说什么
        // How it talks: no fixed form of address, no fixed pattern; it picks between griping, warning, small talk and
        // thinking aloud like a real person. The user asked not to keep saying "master" and to react to what is
        // actually happening, so we give it the situation and the numbers and let it choose what to say
        String job = jobOf(ctx[15]);
        final String prompt = "你是暗黑破坏神2里跟着玩家一起冒险的一名" + self + "（" + (job != null ? job : "佣兵") + "），一路上会自己嘀咕、吐槽、提醒、闲聊。"
                + "语气：" + tone + "。风格：" + style + "。"
                + (extra != null && extra.length() > 0 ? extra + "。" : "")
                + "刚才发生的事：" + (pendingWhy == null ? "没什么特别的" : pendingWhy) + "。" + env
                + hist
                + "要求："
                + (you != null && you.length() > 0 ? "偶尔可以叫他「" + you + "」，但不要每句都这么开头；" : "不要用固定称呼开头；")
                + "说一句你现在真想说的话，可以是吐槽、抱怨、提醒、感慨、随口闲聊，别每句都一个套路；"
                + "不要解说画面、不要念数据、不要像客服；"
                + "只输出这句话本身：不要引号、不要解释、不要表情符号、不要重复上面说过的话。";
        new Thread(new Runnable() {
            public void run() {
                try {
                    String body = "{\"model\":\"" + model + "\",\"messages\":[{\"role\":\"user\",\"content\":\""
                            + esc(prompt) + "\"}],\"max_tokens\":90,\"temperature\":1.3}";
                    HttpURLConnection c = (HttpURLConnection) new URL(url).openConnection();
                    c.setRequestMethod("POST");
                    c.setConnectTimeout(8000);
                    c.setReadTimeout(15000);
                    c.setRequestProperty("Content-Type", "application/json");
                    c.setRequestProperty("Authorization", "Bearer " + key);
                    c.setDoOutput(true);
                    OutputStream o = c.getOutputStream();
                    o.write(body.getBytes("UTF-8"));
                    o.close();
                    int code = c.getResponseCode();
                    if (code != 200) {
                        if (debug > 0) Log.w(TAG, "HTTP " + code);
                        return;
                    }
                    BufferedReader r = new BufferedReader(new InputStreamReader(c.getInputStream(), "UTF-8"));
                    StringBuilder sb = new StringBuilder();
                    String ln;
                    while ((ln = r.readLine()) != null) sb.append(ln);
                    r.close();
                    String t = content(sb.toString());
                    if (t == null || t.length() == 0) return;
                    if (t.length() > 24) t = t.substring(0, 24);
                    line = t;
                    lastLineAt = System.currentTimeMillis();
                    // 同时也丢进游戏聊天栏（原版系统提示一行「[游戏] …」），头顶气泡 + 聊天记录双通道
                    // Also drop it into the game's chat bar (an original system notice line), so it shows both above
                    // his head and in the chat log
                    // 聊天栏那一行用他的真名（名表序号在 ctx[16]），查不到才退回「佣兵」
                    // The chat bar line uses his real name (name ordinal in ctx[16]), falling back to 佣兵
                    String who = MercNames.get(ctx[16]);
                    try { DmActivity.nativeMercSay((who != null ? who : "佣兵") + "：" + t); } catch (Throwable ignored) { }
                    recent.add(t);
                    while (recent.size() > 5) recent.remove(0);
                    if (debug > 0) Log.i(TAG, "佣兵说（" + pendingWhy + "）：" + t);
                } catch (Exception e) {
                    if (debug > 0) Log.w(TAG, "请求失败：" + e);
                } finally {
                    busy = false;
                }
            }
        }).start();
    }

    // 从 chat/completions 的 JSON 里抠出第一个 content 字符串（不引第三方 JSON 库）
    // Pull the first content string out of the chat/completions JSON (no third-party JSON library)
    private static String content(String s) {
        int i = s.indexOf("\"content\":\"");
        if (i < 0) return null;
        i += 11;
        StringBuilder out = new StringBuilder();
        int j = i;
        while (j < s.length()) {
            char ch = s.charAt(j);
            if (ch == '"') break;
            if (ch == '\\' && j + 1 < s.length()) {
                char e = s.charAt(j + 1);
                if (e == 'n') out.append('\n');
                else if (e == 't') out.append(' ');
                else if (e == '"' || e == '\\' || e == '/') out.append(e);
                else if (e == 'u' && j + 5 < s.length()) {
                    try { out.append((char) Integer.parseInt(s.substring(j + 2, j + 6), 16)); } catch (Exception ignored) { }
                    j += 4;
                }
                j += 2;
                continue;
            }
            out.append(ch);
            j++;
        }
        return out.toString();
    }

    // 佣兵类号 → 他到底是什么（271 罗格弓手 / 338 沙漠佣兵 / 359 铁狼法师 / 560·561 野蛮人）。
    // 用户反馈：模型瞎猜「你有盾」，其实是弓手 —— 所以把职业写死进提示词
    // Mercenary class id -> what he actually is. The user reported the model inventing a shield for an archer,
    // so the profession is spelled out in the prompt
    private static String jobOf(int cls) {
        switch (cls) {
            case 271: return "第一幕的罗格弓手，拿弓射箭，身上没有盾";
            case 338: return "第二幕的沙漠佣兵，用长柄武器";
            case 359: return "第三幕的铁狼法师，用剑和盾，会放法术";
            case 560: case 561: return "第五幕的野蛮人，近战，用剑或斧";
            default: return null;
        }
    }

    float bubbleDy() { return bubbleDy; }

    private static float parseFloat(String s, float d) {
        try { return Float.parseFloat(s.trim()); } catch (Exception e) { return d; }
    }

    private static int parseInt(String s, int d) {
        try { return Integer.parseInt(s.trim()); } catch (Exception e) { return d; }
    }

    private static String esc(String s) {
        return s.replace("\\", "\\\\").replace("\"", "\\\"").replace("\n", "\\n");
    }
}
