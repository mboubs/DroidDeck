#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Vulkan loader-layer interface, laid out as in vulkan_core.h and vk_layer.h; libblsession builds without Vulkan headers. */
typedef int32_t VkResult;
typedef uint64_t VkDeviceSize;
typedef void (*PFN_vkVoidFunction)(void);
typedef struct VkInstance_T *VkInstance;
typedef struct VkPhysicalDevice_T *VkPhysicalDevice;
typedef struct VkDevice_T *VkDevice;
typedef PFN_vkVoidFunction (*PFN_vkGetInstanceProcAddr)(VkInstance, const char *);
typedef PFN_vkVoidFunction (*PFN_vkGetDeviceProcAddr)(VkDevice, const char *);

#define VK_SUCCESS 0
#define VK_ERROR_OUT_OF_HOST_MEMORY (-1)
#define VK_ERROR_INITIALIZATION_FAILED (-3)
#define VK_MAX_MEMORY_TYPES 32
#define VK_MAX_MEMORY_HEAPS 16
#define VK_MEMORY_HEAP_DEVICE_LOCAL_BIT 0x1u
#define VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO 47
#define VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO 48
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT 1000237000
#define VK_LAYER_LINK_INFO 0
#define LAYER_NEGOTIATE_INTERFACE_STRUCT 1

typedef struct {
    uint32_t sType;
    const void *pNext;
} BaseIn;

typedef struct BaseOut {
    uint32_t sType;
    struct BaseOut *pNext;
} BaseOut;

typedef struct {
    uint32_t propertyFlags;
    uint32_t heapIndex;
} VkMemoryType;

typedef struct {
    VkDeviceSize size;
    uint32_t flags;
} VkMemoryHeap;

typedef struct {
    uint32_t memoryTypeCount;
    VkMemoryType memoryTypes[VK_MAX_MEMORY_TYPES];
    uint32_t memoryHeapCount;
    VkMemoryHeap memoryHeaps[VK_MAX_MEMORY_HEAPS];
} VkPhysicalDeviceMemoryProperties;

typedef struct {
    uint32_t sType;
    BaseOut *pNext;
    VkPhysicalDeviceMemoryProperties memoryProperties;
} VkPhysicalDeviceMemoryProperties2;

typedef struct {
    uint32_t sType;
    BaseOut *pNext;
    VkDeviceSize heapBudget[VK_MAX_MEMORY_HEAPS];
    VkDeviceSize heapUsage[VK_MAX_MEMORY_HEAPS];
} VkPhysicalDeviceMemoryBudgetPropertiesEXT;

typedef struct InstanceLink {
    struct InstanceLink *pNext;
    PFN_vkGetInstanceProcAddr pfnNextGetInstanceProcAddr;
    PFN_vkVoidFunction pfnNextGetPhysicalDeviceProcAddr;
} InstanceLink;

typedef struct {
    uint32_t sType;
    const void *pNext;
    uint32_t function;
    union {
        InstanceLink *pLayerInfo;
        void *other[2];
    } u;
} InstanceCreateInfo;

typedef struct DeviceLink {
    struct DeviceLink *pNext;
    PFN_vkGetInstanceProcAddr pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr pfnNextGetDeviceProcAddr;
} DeviceLink;

typedef struct {
    uint32_t sType;
    const void *pNext;
    uint32_t function;
    union {
        DeviceLink *pLayerInfo;
        void *other;
    } u;
} DeviceCreateInfo;

typedef struct {
    uint32_t sType;
    void *pNext;
    uint32_t loaderLayerInterfaceVersion;
    PFN_vkGetInstanceProcAddr pfnGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr pfnGetDeviceProcAddr;
    PFN_vkVoidFunction pfnGetPhysicalDeviceProcAddr;
} NegotiateLayerInterface;

typedef VkResult (*PFN_CreateInstance)(const void *, const void *, VkInstance *);
typedef void (*PFN_DestroyInstance)(VkInstance, const void *);
typedef VkResult (*PFN_CreateDevice)(VkPhysicalDevice, const void *, const void *, VkDevice *);
typedef void (*PFN_DestroyDevice)(VkDevice, const void *);
typedef void (*PFN_MemoryProperties)(VkPhysicalDevice, VkPhysicalDeviceMemoryProperties *);
typedef void (*PFN_MemoryProperties2)(VkPhysicalDevice, VkPhysicalDeviceMemoryProperties2 *);

typedef struct Instance {
    struct Instance *next;
    void *key;
    VkInstance handle;
    PFN_vkGetInstanceProcAddr gipa;
    PFN_DestroyInstance destroy;
    PFN_MemoryProperties properties;
    PFN_MemoryProperties2 properties2;
    PFN_MemoryProperties2 properties2_khr;
} Instance;

typedef struct Device {
    struct Device *next;
    void *key;
    PFN_vkGetDeviceProcAddr gdpa;
    PFN_DestroyDevice destroy;
} Device;

static pthread_mutex_t hc_lock = PTHREAD_MUTEX_INITIALIZER;
static Instance *hc_instances;
static Device *hc_devices;
static int hc_reported;

/* A dispatchable handle starts with its loader dispatch pointer, which a physical device shares with its instance. */
static void *hc_key(const void *handle)
{
    return *(void *const *)handle;
}

static Instance *hc_instance(const void *handle)
{
    void *key = hc_key(handle);
    pthread_mutex_lock(&hc_lock);
    Instance *found = hc_instances;
    while (found && found->key != key)
        found = found->next;
    pthread_mutex_unlock(&hc_lock);
    return found;
}

static Device *hc_device(const void *handle)
{
    void *key = hc_key(handle);
    pthread_mutex_lock(&hc_lock);
    Device *found = hc_devices;
    while (found && found->key != key)
        found = found->next;
    pthread_mutex_unlock(&hc_lock);
    return found;
}

static VkDeviceSize hc_cap(void)
{
    const char *text = getenv("DROIDDECK_VK_HEAP_CAP_MB");
    if (!text || *text < '1' || *text > '9')
        return 0;
    char *end;
    unsigned long long mb = strtoull(text, &end, 10);
    if (*end || mb == 0 || mb > (1ull << 24))
        return 0;
    return (VkDeviceSize)mb << 20;
}

static void hc_cap_properties(VkPhysicalDeviceMemoryProperties *properties, VkDeviceSize cap)
{
    uint32_t count = properties->memoryHeapCount < VK_MAX_MEMORY_HEAPS ? properties->memoryHeapCount : VK_MAX_MEMORY_HEAPS;
    for (uint32_t i = 0; i < count; i++) {
        VkMemoryHeap *heap = &properties->memoryHeaps[i];
        if (!(heap->flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) || heap->size <= cap)
            continue;
        if (!__atomic_exchange_n(&hc_reported, 1, __ATOMIC_RELAXED))
            fprintf(stderr, "droiddeck-heap-cap: GPU memory heap reported as %llu MB instead of %llu MB\n",
                    (unsigned long long)(cap >> 20), (unsigned long long)(heap->size >> 20));
        heap->size = cap;
    }
}

static void hc_cap_budget(VkPhysicalDeviceMemoryProperties2 *properties, VkDeviceSize cap)
{
    const VkPhysicalDeviceMemoryProperties *memory = &properties->memoryProperties;
    uint32_t count = memory->memoryHeapCount < VK_MAX_MEMORY_HEAPS ? memory->memoryHeapCount : VK_MAX_MEMORY_HEAPS;
    for (BaseOut *next = properties->pNext; next; next = next->pNext) {
        if (next->sType != VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT)
            continue;
        VkPhysicalDeviceMemoryBudgetPropertiesEXT *budget = (VkPhysicalDeviceMemoryBudgetPropertiesEXT *)next;
        for (uint32_t i = 0; i < count; i++)
            if ((memory->memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) && budget->heapBudget[i] > cap)
                budget->heapBudget[i] = cap;
    }
}

static void hc_GetPhysicalDeviceMemoryProperties(VkPhysicalDevice device, VkPhysicalDeviceMemoryProperties *properties)
{
    Instance *instance = hc_instance(device);
    if (!instance || !instance->properties)
        return;
    instance->properties(device, properties);
    VkDeviceSize cap = hc_cap();
    if (cap)
        hc_cap_properties(properties, cap);
}

static void hc_memory_properties2(PFN_MemoryProperties2 next, VkPhysicalDevice device, VkPhysicalDeviceMemoryProperties2 *properties)
{
    next(device, properties);
    VkDeviceSize cap = hc_cap();
    if (!cap)
        return;
    hc_cap_budget(properties, cap);
    hc_cap_properties(&properties->memoryProperties, cap);
}

static void hc_GetPhysicalDeviceMemoryProperties2(VkPhysicalDevice device, VkPhysicalDeviceMemoryProperties2 *properties)
{
    Instance *instance = hc_instance(device);
    PFN_MemoryProperties2 next = instance ? (instance->properties2 ? instance->properties2 : instance->properties2_khr) : NULL;
    if (next)
        hc_memory_properties2(next, device, properties);
}

static void hc_GetPhysicalDeviceMemoryProperties2KHR(VkPhysicalDevice device, VkPhysicalDeviceMemoryProperties2 *properties)
{
    Instance *instance = hc_instance(device);
    PFN_MemoryProperties2 next = instance ? (instance->properties2_khr ? instance->properties2_khr : instance->properties2) : NULL;
    if (next)
        hc_memory_properties2(next, device, properties);
}

static PFN_vkVoidFunction hc_GetInstanceProcAddr(VkInstance instance, const char *name);
static PFN_vkVoidFunction hc_GetDeviceProcAddr(VkDevice device, const char *name);

static VkResult hc_CreateInstance(const void *info, const void *allocator, VkInstance *out)
{
    InstanceCreateInfo *link = NULL;
    for (const BaseIn *next = info; next; next = next->pNext) {
        if (next->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO &&
            ((const InstanceCreateInfo *)next)->function == VK_LAYER_LINK_INFO) {
            link = (InstanceCreateInfo *)next;
            break;
        }
    }
    if (!link || !link->u.pLayerInfo)
        return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetInstanceProcAddr gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;
    PFN_CreateInstance create = (PFN_CreateInstance)gipa(NULL, "vkCreateInstance");
    if (!create)
        return VK_ERROR_INITIALIZATION_FAILED;
    VkResult result = create(info, allocator, out);
    if (result != VK_SUCCESS)
        return result;
    PFN_DestroyInstance destroy = (PFN_DestroyInstance)gipa(*out, "vkDestroyInstance");
    Instance *instance = calloc(1, sizeof(*instance));
    if (!instance) {
        if (destroy)
            destroy(*out, allocator);
        *out = NULL;
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    instance->key = hc_key(*out);
    instance->handle = *out;
    instance->gipa = gipa;
    instance->destroy = destroy;
    instance->properties = (PFN_MemoryProperties)gipa(*out, "vkGetPhysicalDeviceMemoryProperties");
    instance->properties2 = (PFN_MemoryProperties2)gipa(*out, "vkGetPhysicalDeviceMemoryProperties2");
    instance->properties2_khr = (PFN_MemoryProperties2)gipa(*out, "vkGetPhysicalDeviceMemoryProperties2KHR");
    pthread_mutex_lock(&hc_lock);
    instance->next = hc_instances;
    hc_instances = instance;
    pthread_mutex_unlock(&hc_lock);
    return result;
}

static void hc_DestroyInstance(VkInstance handle, const void *allocator)
{
    if (!handle)
        return;
    void *key = hc_key(handle);
    pthread_mutex_lock(&hc_lock);
    Instance **link = &hc_instances;
    while (*link && (*link)->key != key)
        link = &(*link)->next;
    Instance *instance = *link;
    if (instance)
        *link = instance->next;
    pthread_mutex_unlock(&hc_lock);
    if (!instance)
        return;
    if (instance->destroy)
        instance->destroy(handle, allocator);
    free(instance);
}

static VkResult hc_CreateDevice(VkPhysicalDevice physical, const void *info, const void *allocator, VkDevice *out)
{
    DeviceCreateInfo *link = NULL;
    for (const BaseIn *next = info; next; next = next->pNext) {
        if (next->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO &&
            ((const DeviceCreateInfo *)next)->function == VK_LAYER_LINK_INFO) {
            link = (DeviceCreateInfo *)next;
            break;
        }
    }
    if (!link || !link->u.pLayerInfo)
        return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetInstanceProcAddr gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr gdpa = link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;
    Instance *instance = hc_instance(physical);
    PFN_CreateDevice create = (PFN_CreateDevice)gipa(instance ? instance->handle : NULL, "vkCreateDevice");
    if (!create)
        return VK_ERROR_INITIALIZATION_FAILED;
    VkResult result = create(physical, info, allocator, out);
    if (result != VK_SUCCESS)
        return result;
    PFN_DestroyDevice destroy = (PFN_DestroyDevice)gdpa(*out, "vkDestroyDevice");
    Device *device = calloc(1, sizeof(*device));
    if (!device) {
        if (destroy)
            destroy(*out, allocator);
        *out = NULL;
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    device->key = hc_key(*out);
    device->gdpa = gdpa;
    device->destroy = destroy;
    pthread_mutex_lock(&hc_lock);
    device->next = hc_devices;
    hc_devices = device;
    pthread_mutex_unlock(&hc_lock);
    return result;
}

static void hc_DestroyDevice(VkDevice handle, const void *allocator)
{
    if (!handle)
        return;
    void *key = hc_key(handle);
    pthread_mutex_lock(&hc_lock);
    Device **link = &hc_devices;
    while (*link && (*link)->key != key)
        link = &(*link)->next;
    Device *device = *link;
    if (device)
        *link = device->next;
    pthread_mutex_unlock(&hc_lock);
    if (!device)
        return;
    if (device->destroy)
        device->destroy(handle, allocator);
    free(device);
}

static PFN_vkVoidFunction hc_GetDeviceProcAddr(VkDevice device, const char *name)
{
    if (!strcmp(name, "vkGetDeviceProcAddr"))
        return (PFN_vkVoidFunction)hc_GetDeviceProcAddr;
    if (!strcmp(name, "vkDestroyDevice"))
        return (PFN_vkVoidFunction)hc_DestroyDevice;
    Device *found = device ? hc_device(device) : NULL;
    return found ? found->gdpa(device, name) : NULL;
}

static PFN_vkVoidFunction hc_GetInstanceProcAddr(VkInstance instance, const char *name)
{
    if (!strcmp(name, "vkGetInstanceProcAddr"))
        return (PFN_vkVoidFunction)hc_GetInstanceProcAddr;
    if (!strcmp(name, "vkCreateInstance"))
        return (PFN_vkVoidFunction)hc_CreateInstance;
    if (!strcmp(name, "vkDestroyInstance"))
        return (PFN_vkVoidFunction)hc_DestroyInstance;
    if (!strcmp(name, "vkCreateDevice"))
        return (PFN_vkVoidFunction)hc_CreateDevice;
    if (!strcmp(name, "vkGetDeviceProcAddr"))
        return (PFN_vkVoidFunction)hc_GetDeviceProcAddr;
    Instance *found = instance ? hc_instance(instance) : NULL;
    if (!found)
        return NULL;
    PFN_vkVoidFunction next = found->gipa(instance, name);
    if (!next)
        return NULL;
    if (!strcmp(name, "vkGetPhysicalDeviceMemoryProperties"))
        return (PFN_vkVoidFunction)hc_GetPhysicalDeviceMemoryProperties;
    if (!strcmp(name, "vkGetPhysicalDeviceMemoryProperties2"))
        return (PFN_vkVoidFunction)hc_GetPhysicalDeviceMemoryProperties2;
    if (!strcmp(name, "vkGetPhysicalDeviceMemoryProperties2KHR"))
        return (PFN_vkVoidFunction)hc_GetPhysicalDeviceMemoryProperties2KHR;
    return next;
}

VkResult DroidDeckHeapCap_NegotiateLoaderLayerInterfaceVersion(NegotiateLayerInterface *version)
{
    if (!version || version->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT || version->loaderLayerInterfaceVersion < 2)
        return VK_ERROR_INITIALIZATION_FAILED;
    version->loaderLayerInterfaceVersion = 2;
    version->pfnGetInstanceProcAddr = hc_GetInstanceProcAddr;
    version->pfnGetDeviceProcAddr = hc_GetDeviceProcAddr;
    version->pfnGetPhysicalDeviceProcAddr = NULL;
    return VK_SUCCESS;
}
