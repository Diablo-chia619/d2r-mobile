// 极简 libhardware 桩 | minimal libhardware stub
//
// Turnip（vulkan.purple.so）在 DT_NEEDED 里依赖平台私有库 libhardware.so，而普通 App 的
// 命名空间看不到它，adrenotools 建好命名空间后仍会以 "library libhardware.so not found" 失败。
// 实测驱动只从它引用一个符号：hw_get_module，用于找 gralloc HAL。这里返回 -ENOENT，
// Mesa 会继续走 AHardwareBuffer 路径（Android 上本来就是主路径），不需要真正的 HAL 实现。
//
// The Turnip driver needs libhardware.so, a platform-private library an app namespace cannot see.
// The driver imports exactly one symbol from it, hw_get_module (gralloc lookup). Returning -ENOENT
// lets Mesa fall back to the AHardwareBuffer path, which is the normal path on Android anyway.
#include <stddef.h>

struct hw_module_t;

int hw_get_module(const char *id, const struct hw_module_t **module) {
    (void)id;
    if (module) {
        *module = NULL;
    }
    return -2; // -ENOENT
}

// 少数老代码会调这个变体 | a few older call sites use this variant
int hw_get_module_by_class(const char *class_id, const char *inst, const struct hw_module_t **module) {
    (void)class_id;
    (void)inst;
    if (module) {
        *module = NULL;
    }
    return -2; // -ENOENT
}
