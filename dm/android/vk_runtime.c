// Vulkan 驱动运行期加载：系统 libvulkan 或 adrenotools 加载的自定义驱动（Turnip）。
// Runtime loading of the Vulkan driver: either the system libvulkan or a custom driver (Turnip) via adrenotools.
//
// 流程 / Flow:
//   1. 第一次用到 Vulkan 时 init：按 DM_GPU_DRIVER 决定 dlopen 系统 loader 还是走 adrenotools；
//   2. vkCreateInstance 之后用驱动的 vkGetInstanceProcAddr 解析全部 117 个入口；
//   3. vkCreateDevice 之后用 vkGetDeviceProcAddr 补上仍然缺的；
//   4. 任何入口解析失败都直接报出来（名字 + 当前驱动），不做静默回退。
//   1. On the first Vulkan call, init: DM_GPU_DRIVER decides between the system loader and adrenotools;
//   2. after vkCreateInstance every entry point is resolved through the driver's vkGetInstanceProcAddr;
//   3. after vkCreateDevice whatever is still missing is resolved through vkGetDeviceProcAddr;
//   4. a missing entry point is reported loudly (name + driver), never silently skipped.
#include "vk_runtime.h"

#include <android/log.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>

#define LOGI(...) do { __android_log_print(ANDROID_LOG_INFO, "dm", "[vk] " __VA_ARGS__); \
                        fprintf(stderr, "[vk] " __VA_ARGS__); fputc('\n', stderr); } while (0)
#define LOGE(...) do { __android_log_print(ANDROID_LOG_ERROR, "dm", "[vk] " __VA_ARGS__); \
                        fprintf(stderr, "[vk] " __VA_ARGS__); fputc('\n', stderr); } while (0)

// vk_shim.c 生成的全局函数指针里，只有 vkCreateInstance 在这里提前用；其余通过 slots 表访问
// Of the generated globals only vkCreateInstance is used here; the rest are reached through the slot table
extern PFN_vkCreateInstance p_vkCreateInstance;
extern PFN_vkCreateDevice p_vkCreateDevice;

// adrenotools 的 feature flag（include/adrenotools/priv.h）
// adrenotools feature flag (include/adrenotools/priv.h)
enum { ADRENOTOOLS_DRIVER_CUSTOM = 1 << 0 };

typedef void *(*adreno_open_fn)(int, int, const char *, const char *, const char *, const char *, const char *, void **);

static char g_internal[1024];       // /data/user/0/<pkg>/files
static char g_hook_dir[1024];       // nativeLibraryDir（libdm.so 所在目录）
static char g_status[512] = "not initialised";

static void *g_loader;              // dlopen 的 handle（系统 loader 或 adrenotools 的 handle）
static void *g_adrenotools;
static PFN_vkGetInstanceProcAddr g_gipa;
static PFN_vkGetDeviceProcAddr g_gdpa;
static VkInstance g_instance;
static VkDevice g_device;
static int g_selected_turnip;
static int g_inited;

static void dirname_of(const char *path, char *out, size_t cap) {
    const char *slash = strrchr(path, '/');
    size_t n = slash ? (size_t)(slash - path) : 0;
    if (n >= cap) n = cap - 1;
    memcpy(out, path, n);
    out[n] = 0;
}

// 从 /proc/self/maps 里找 libdm.so 的路径，推出 nativeLibraryDir。
// 这样不必在 JNI 层多传一个参数，也不依赖 Java 侧的实现细节。
// Find libdm.so in /proc/self/maps and derive nativeLibraryDir from it, so no extra JNI plumbing is needed.
static int find_hook_dir(void) {
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return 0;
    char line[4096];
    while (fgets(line, sizeof line, f)) {
        char *p = strstr(line, "/libdm.so");
        if (!p) continue;
        // 行格式：地址 权限 偏移 设备 inode 路径；路径从第一个 '/' 开始
        char *path = strchr(line, '/');
        if (path && path <= p) {
            dirname_of(path, g_hook_dir, sizeof g_hook_dir);
            fclose(f);
            LOGI("hook dir = %s", g_hook_dir);
            return 1;
        }
    }
    fclose(f);
    return 0;
}

static void resolve_all(const char *phase) {
    unsigned ok = 0, missing = 0;
    for (unsigned i = 0; i < dm_vk_slot_count; ++i) {
        void **slot = dm_vk_slots[i].ptr;
        if (*slot) { ok++; continue; }
        PFN_vkVoidFunction p = NULL;
        if (g_gipa && g_instance) p = g_gipa(g_instance, dm_vk_slots[i].name);
        if (!p && g_gdpa && g_device) p = g_gdpa(g_device, dm_vk_slots[i].name);
        if (p) { *slot = (void *)p; ok++; }
        else { missing++; LOGE("%s: 入口解析失败 %s (driver=%s)", phase, dm_vk_slots[i].name, g_status); }
    }
    LOGI("%s: 解析 %u/%u，缺 %u（%s）", phase, ok, dm_vk_slot_count, missing, g_status);
}

static int init_driver(void) {
    if (g_inited) return g_gipa != NULL;
    g_inited = 1;

    if (!g_hook_dir[0] && !find_hook_dir())
        LOGI("没找到 libdm.so 的路径，hooks 目录留空（系统驱动不需要它）");

    const char *want = getenv("DM_GPU_DRIVER");
    const int require = getenv("DM_GPU_DRIVER_REQUIRE") && atoi(getenv("DM_GPU_DRIVER_REQUIRE"));
    const int use_turnip = want && (!strcmp(want, "turnip") || !strcmp(want, "Turnip") || !strcmp(want, "TURNIP"));

    if (use_turnip) {
        if (!g_internal[0]) {
            LOGE("DM_GPU_DRIVER=turnip 但没有内部目录（dm_vk_set_internal_dir 没被调用）");
        } else {
            char driver_dir[1200];
            // adrenotools 内部是 dir + name 直接拼接（driver.cpp:65 的 stat），中间不加斜杠，
            // 所以这里必须带结尾斜杠，否则它会去找 .../turnipvulkan.purple.so 并静默返回 NULL。
            // adrenotools concatenates dir + name without a separator, so the trailing slash is required.
            snprintf(driver_dir, sizeof driver_dir, "%s/gpu/turnip/", g_internal);
            g_adrenotools = dlopen("libadrenotools.so", RTLD_NOW | RTLD_LOCAL);
            if (!g_adrenotools) {
                LOGE("libadrenotools.so 加载失败: %s", dlerror());
            } else {
                adreno_open_fn open_fn = (adreno_open_fn)dlsym(g_adrenotools, "adrenotools_open_libvulkan");
                if (!open_fn) {
                    LOGE("adrenotools_open_libvulkan 找不到: %s", dlerror());
                } else {
                    LOGI("尝试 Turnip: hook=%s driver=%s", g_hook_dir, driver_dir);
                    void *handle = open_fn(RTLD_NOW, ADRENOTOOLS_DRIVER_CUSTOM, NULL,
                                           g_hook_dir, driver_dir, "vulkan.purple.so", NULL, NULL);
                    if (!handle) {
                        // 诊断：adrenotools 内部失败时不报原因，这里直接 dlopen 驱动本体与它的依赖，
                        // 把 dlerror() 打出来（最常见的是依赖了 App 不允许加载的平台私有库）。
                        // Diagnostics: adrenotools reports nothing on failure, so probe the driver and its
                        // dependencies directly and log dlerror() (usually a platform-private library an
                        // app is not allowed to load).
                        char so[1400];
                        snprintf(so, sizeof so, "%s/vulkan.purple.so", driver_dir);
                        dlerror();
                        void *probe = dlopen(so, RTLD_NOW | RTLD_LOCAL);
                        const char *probe_err = dlerror();
                        LOGE("诊断：dlopen(%s) -> %p, dlerror=%s", so, probe, probe_err ? probe_err : "(none)");
                        const char *deps[] = {"libhardware.so", "libnativewindow.so", "libsync.so", NULL};
                        for (int i = 0; deps[i]; ++i) {
                            dlerror();
                            void *h = dlopen(deps[i], RTLD_NOW | RTLD_LOCAL);
                            const char *e = dlerror();
                            LOGE("诊断：dlopen(%s) -> %p, dlerror=%s", deps[i], h, e ? e : "(none)");
                        }
                        LOGE("adrenotools_open_libvulkan 返回 NULL（驱动文件不在 %s？）", driver_dir);
                    } else {
                        PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(handle, "vkGetInstanceProcAddr");
                        if (!gipa) {
                            LOGE("自定义 loader 里取不到 vkGetInstanceProcAddr: %s", dlerror());
                        } else {
                            g_loader = handle;
                            g_gipa = gipa;
                            g_selected_turnip = 1;
                            snprintf(g_status, sizeof g_status, "requested=turnip selected=turnip");
                            LOGI("驱动选择：Turnip（adrenotools）");
                        }
                    }
                }
            }
        }
        if (!g_gipa && require) {
            LOGE("DM_GPU_DRIVER_REQUIRE=1 且 Turnip 没加载起来 —— 不回落系统驱动，直接失败");
            return 0;
        }
        if (!g_gipa) LOGE("Turnip 未就绪，回落到系统驱动（要严格失败请设 DM_GPU_DRIVER_REQUIRE=1）");
    }

    if (!g_gipa) {
        void *lib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
        if (!lib) { LOGE("系统 libvulkan.so 加载失败: %s", dlerror()); return 0; }
        PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
        if (!gipa) { LOGE("系统 loader 里取不到 vkGetInstanceProcAddr: %s", dlerror()); return 0; }
        g_loader = lib;
        g_gipa = gipa;
        g_selected_turnip = 0;
        snprintf(g_status, sizeof g_status, "requested=%s selected=system", (use_turnip ? "turnip" : "system"));
        LOGI("驱动选择：系统（libvulkan.so）");
    }

    // 先把实例级入口解析掉：vkCreateInstance 必须在建实例之前拿到
    // Resolve the instance-level entry points first: vkCreateInstance is needed before any instance exists
    if (!p_vkCreateInstance) {
        PFN_vkVoidFunction p = g_gipa(NULL, "vkCreateInstance");
        if (!p) { LOGE("取不到 vkCreateInstance"); return 0; }
        p_vkCreateInstance = (PFN_vkCreateInstance)p;
    }
    return 1;
}

void dm_vk_set_internal_dir(const char *dir) {
    if (!dir) return;
    snprintf(g_internal, sizeof g_internal, "%s", dir);
    LOGI("internal dir = %s", g_internal);
}

const char *dm_vk_status_line(void) { return g_status; }

VkResult dm_vk_create_instance(const VkInstanceCreateInfo *create, const VkAllocationCallbacks *alloc, VkInstance *out) {
    if (!init_driver()) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = p_vkCreateInstance(create, alloc, out);
    if (r != VK_SUCCESS) { LOGE("vkCreateInstance 失败: %d", (int)r); return r; }
    g_instance = *out;
    resolve_all("instance");
    return r;
}

VkResult dm_vk_create_device(VkPhysicalDevice physical, const VkDeviceCreateInfo *create, const VkAllocationCallbacks *alloc, VkDevice *out) {
    dm_vk_require((void **)&p_vkCreateDevice, "vkCreateDevice");
    VkResult r = p_vkCreateDevice(physical, create, alloc, out);
    if (r != VK_SUCCESS) { LOGE("vkCreateDevice 失败: %d", (int)r); return r; }
    g_device = *out;
    if (!g_gdpa) {
        PFN_vkVoidFunction p = g_gipa ? g_gipa(g_instance, "vkGetDeviceProcAddr") : NULL;
        if (p) g_gdpa = (PFN_vkGetDeviceProcAddr)p;
    }
    resolve_all("device");
    return r;
}

PFN_vkVoidFunction dm_vk_gipa(VkInstance instance, const char *name) {
    if (!g_inited && !init_driver()) return NULL;
    return g_gipa ? g_gipa(instance, name) : NULL;
}

PFN_vkVoidFunction dm_vk_gdpa(VkDevice device, const char *name) {
    if (!g_inited && !init_driver()) return NULL;
    return g_gdpa ? g_gdpa(device, name) : NULL;
}

void dm_vk_require(void **slot, const char *name) {
    if (*slot) return;
    if (!g_inited) init_driver();
    PFN_vkVoidFunction p = NULL;
    if (g_gipa && g_instance) p = g_gipa(g_instance, name);
    if (!p && g_gdpa && g_device) p = g_gdpa(g_device, name);
    if (!p) {
        LOGE("致命：入口 %s 解析不到（%s）", name, g_status);
        abort();
    }
    *slot = (void *)p;
}

void dm_vk_call_void(void **slot, const char *name) { dm_vk_require(slot, name); }
