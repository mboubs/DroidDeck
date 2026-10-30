import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "linuxfs/preload/heapcap.c"
MANIFEST = ROOT / "linuxfs/overlay/usr/share/vulkan/implicit_layer.d/droiddeck_heap_cap.json"
HARNESS = r"""
#include HEAPCAP
#include <assert.h>

#define GB (1ull << 30)

static void *dispatch_table[4];
static struct { void *dispatch; } instance_object = {dispatch_table}, physical_object = {dispatch_table};
static void *device_table[4];
static struct { void *dispatch; } device_object = {device_table};
static int destroyed_instance, destroyed_device, driver_function;

static VkResult driver_CreateInstance(const void *info, const void *allocator, VkInstance *out)
{
    (void)info; (void)allocator;
    *out = (VkInstance)&instance_object;
    return VK_SUCCESS;
}

static void driver_DestroyInstance(VkInstance instance, const void *allocator) { (void)instance; (void)allocator; destroyed_instance++; }

static void driver_properties(VkPhysicalDeviceMemoryProperties *memory)
{
    memset(memory, 0, sizeof(*memory));
    memory->memoryHeapCount = 2;
    memory->memoryHeaps[0].size = 9 * GB;
    memory->memoryHeaps[0].flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;
    memory->memoryHeaps[1].size = 9 * GB;
}

static void driver_GetPhysicalDeviceMemoryProperties(VkPhysicalDevice device, VkPhysicalDeviceMemoryProperties *memory)
{
    (void)device;
    driver_properties(memory);
}

static void driver_GetPhysicalDeviceMemoryProperties2(VkPhysicalDevice device, VkPhysicalDeviceMemoryProperties2 *properties)
{
    (void)device;
    driver_properties(&properties->memoryProperties);
    for (BaseOut *next = properties->pNext; next; next = next->pNext) {
        if (next->sType != VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT)
            continue;
        VkPhysicalDeviceMemoryBudgetPropertiesEXT *budget = (VkPhysicalDeviceMemoryBudgetPropertiesEXT *)next;
        budget->heapBudget[0] = 8 * GB;
        budget->heapBudget[1] = 8 * GB;
        budget->heapUsage[0] = 1 * GB;
    }
}

static VkResult driver_CreateDevice(VkPhysicalDevice physical, const void *info, const void *allocator, VkDevice *out)
{
    (void)physical; (void)info; (void)allocator;
    *out = (VkDevice)&device_object;
    return VK_SUCCESS;
}

static void driver_DestroyDevice(VkDevice device, const void *allocator) { (void)device; (void)allocator; destroyed_device++; }
static void driver_Function(void) { driver_function++; }

static PFN_vkVoidFunction driver_GetInstanceProcAddr(VkInstance instance, const char *name)
{
    if (!strcmp(name, "vkCreateInstance")) return (PFN_vkVoidFunction)driver_CreateInstance;
    if (!instance) return NULL;
    if (!strcmp(name, "vkDestroyInstance")) return (PFN_vkVoidFunction)driver_DestroyInstance;
    if (!strcmp(name, "vkCreateDevice")) return (PFN_vkVoidFunction)driver_CreateDevice;
    if (!strcmp(name, "vkGetPhysicalDeviceMemoryProperties")) return (PFN_vkVoidFunction)driver_GetPhysicalDeviceMemoryProperties;
    if (!strcmp(name, "vkGetPhysicalDeviceMemoryProperties2")) return (PFN_vkVoidFunction)driver_GetPhysicalDeviceMemoryProperties2;
    if (!strcmp(name, "vkEnumeratePhysicalDevices")) return (PFN_vkVoidFunction)driver_Function;
    return NULL;
}

static PFN_vkVoidFunction driver_GetDeviceProcAddr(VkDevice device, const char *name)
{
    (void)device;
    if (!strcmp(name, "vkDestroyDevice")) return (PFN_vkVoidFunction)driver_DestroyDevice;
    if (!strcmp(name, "vkQueueSubmit")) return (PFN_vkVoidFunction)driver_Function;
    return NULL;
}

int main(void)
{
    NegotiateLayerInterface negotiate = {LAYER_NEGOTIATE_INTERFACE_STRUCT, NULL, 2, NULL, NULL, NULL};
    assert(DroidDeckHeapCap_NegotiateLoaderLayerInterfaceVersion(&negotiate) == VK_SUCCESS);
    NegotiateLayerInterface old = {LAYER_NEGOTIATE_INTERFACE_STRUCT, NULL, 1, NULL, NULL, NULL};
    assert(DroidDeckHeapCap_NegotiateLoaderLayerInterfaceVersion(&old) != VK_SUCCESS);
    PFN_vkGetInstanceProcAddr gipa = negotiate.pfnGetInstanceProcAddr;

    InstanceLink link = {NULL, driver_GetInstanceProcAddr, NULL};
    InstanceCreateInfo chain = {VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO, NULL, VK_LAYER_LINK_INFO, {&link}};
    BaseIn create_info = {1, &chain};
    VkInstance instance = NULL;
    assert(((PFN_CreateInstance)gipa(NULL, "vkCreateInstance"))(&create_info, NULL, &instance) == VK_SUCCESS);
    assert(instance == (VkInstance)&instance_object);
    assert(chain.u.pLayerInfo == NULL);

    assert(gipa(instance, "vkGetPhysicalDeviceMemoryProperties2KHR") == NULL);
    assert(gipa(instance, "vkEnumeratePhysicalDevices") == (PFN_vkVoidFunction)driver_Function);
    PFN_MemoryProperties properties = (PFN_MemoryProperties)gipa(instance, "vkGetPhysicalDeviceMemoryProperties");
    PFN_MemoryProperties2 properties2 = (PFN_MemoryProperties2)gipa(instance, "vkGetPhysicalDeviceMemoryProperties2");
    VkPhysicalDevice physical = (VkPhysicalDevice)&physical_object;

    VkPhysicalDeviceMemoryProperties memory;
    unsetenv("DROIDDECK_VK_HEAP_CAP_MB");
    properties(physical, &memory);
    assert(memory.memoryHeaps[0].size == 9 * GB);

    setenv("DROIDDECK_VK_HEAP_CAP_MB", "2048", 1);
    properties(physical, &memory);
    assert(memory.memoryHeaps[0].size == 2 * GB);
    assert(memory.memoryHeaps[1].size == 9 * GB);

    VkPhysicalDeviceMemoryBudgetPropertiesEXT budget = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT, NULL, {0}, {0}};
    VkPhysicalDeviceMemoryProperties2 memory2 = {1000059006, (BaseOut *)&budget, {0}};
    properties2(physical, &memory2);
    assert(memory2.memoryProperties.memoryHeaps[0].size == 2 * GB);
    assert(budget.heapBudget[0] == 2 * GB);
    assert(budget.heapBudget[1] == 8 * GB);
    assert(budget.heapUsage[0] == 1 * GB);

    setenv("DROIDDECK_VK_HEAP_CAP_MB", "16384", 1);
    properties(physical, &memory);
    assert(memory.memoryHeaps[0].size == 9 * GB);
    for (const char **bad = (const char *[]){"0", "-1", "02048", "2g", "", NULL}; *bad; bad++) {
        setenv("DROIDDECK_VK_HEAP_CAP_MB", *bad, 1);
        properties(physical, &memory);
        assert(memory.memoryHeaps[0].size == 9 * GB);
    }

    DeviceLink device_link = {NULL, driver_GetInstanceProcAddr, driver_GetDeviceProcAddr};
    DeviceCreateInfo device_chain = {VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO, NULL, VK_LAYER_LINK_INFO, {&device_link}};
    BaseIn device_info = {3, &device_chain};
    VkDevice device = NULL;
    assert(((PFN_CreateDevice)gipa(instance, "vkCreateDevice"))(physical, &device_info, NULL, &device) == VK_SUCCESS);
    PFN_vkGetDeviceProcAddr gdpa = negotiate.pfnGetDeviceProcAddr;
    assert(gdpa(device, "vkQueueSubmit") == (PFN_vkVoidFunction)driver_Function);
    assert(gdpa(device, "vkNothing") == NULL);
    ((PFN_DestroyDevice)gdpa(device, "vkDestroyDevice"))(device, NULL);
    assert(destroyed_device == 1);
    assert(hc_devices == NULL);

    ((PFN_DestroyInstance)gipa(instance, "vkDestroyInstance"))(instance, NULL);
    assert(destroyed_instance == 1);
    assert(hc_instances == NULL);
    assert(gipa(instance, "vkEnumeratePhysicalDevices") == NULL);

    InstanceCreateInfo unlinked = {VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO, NULL, 1, {NULL}};
    BaseIn bare = {1, &unlinked};
    assert(((PFN_CreateInstance)gipa(NULL, "vkCreateInstance"))(&bare, NULL, &instance) == VK_ERROR_INITIALIZATION_FAILED);
    puts("ok");
    return 0;
}
"""


class HeapCapManifestTest(unittest.TestCase):
    def test_the_layer_loads_from_the_session_library_only_when_a_game_asks(self):
        layer = json.loads(MANIFEST.read_text())["layer"]
        self.assertEqual(layer["library_path"], "/usr/local/lib/libblsession.so")
        entry = layer["functions"]["vkNegotiateLoaderLayerInterfaceVersion"]
        self.assertIn("VkResult %s(" % entry, SOURCE.read_text())
        self.assertEqual(layer["enable_environment"], {"DROIDDECK_VK_HEAP_CAP": "1"})
        self.assertIn("disable_environment", layer)


@unittest.skipUnless(shutil.which("gcc"), "needs gcc")
class HeapCapLayerTest(unittest.TestCase):
    def test_device_local_heap_and_budget_are_capped_and_everything_else_passes_through(self):
        tmp = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, tmp, ignore_errors=True)
        (tmp / "harness.c").write_text(HARNESS)
        made = subprocess.run(["gcc", "-Wall", "-Werror", "-pthread", '-DHEAPCAP="%s"' % SOURCE,
                               "-o", str(tmp / "harness"), str(tmp / "harness.c")], capture_output=True, text=True)
        self.assertEqual(made.returncode, 0, made.stderr)
        ran = subprocess.run([str(tmp / "harness")], capture_output=True, text=True, env=dict(os.environ))
        self.assertEqual(ran.returncode, 0, ran.stderr)
        self.assertEqual(ran.stdout, "ok\n")
        self.assertIn("droiddeck-heap-cap: GPU memory heap reported as 2048 MB instead of 9216 MB", ran.stderr)


if __name__ == "__main__":
    unittest.main()
