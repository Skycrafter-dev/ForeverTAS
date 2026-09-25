#include "app/update_hardware.h"

#include <QLibrary>
#include <QRegularExpression>
#include <QVector>
#include <QtGui/qtgui-config.h>

#if QT_FEATURE_vulkan == 1
#include <QVulkanFunctions>
#include <QVulkanInstance>
#endif

namespace forevertas::app {

UpdateHardware DetectUpdateHardware() {
    UpdateHardware hardware;
#if defined(Q_OS_WIN)
    QLibrary cudaDriver(QStringLiteral("nvcuda"));
#else
    QLibrary cudaDriver(QStringLiteral("libcuda.so.1"));
#endif
    if (cudaDriver.load()) {
        using Init = int (*)(unsigned int);
        using GetCount = int (*)(int *);
        using GetDevice = int (*)(int *, int);
        using GetAttribute = int (*)(int *, int, int);
        const auto init = reinterpret_cast<Init>(cudaDriver.resolve("cuInit"));
        const auto count = reinterpret_cast<GetCount>(
                cudaDriver.resolve("cuDeviceGetCount"));
        const auto device = reinterpret_cast<GetDevice>(
                cudaDriver.resolve("cuDeviceGet"));
        const auto attribute = reinterpret_cast<GetAttribute>(
                cudaDriver.resolve("cuDeviceGetAttribute"));
        int deviceCount = 0;
        int firstDevice = 0;
        int major = 0;
        int minor = 0;
        if (init && count && device && attribute &&
            init(0) == 0 && count(&deviceCount) == 0 && deviceCount > 0 &&
            device(&firstDevice, 0) == 0 &&
            attribute(&major, 75, firstDevice) == 0 &&
            attribute(&minor, 76, firstDevice) == 0) {
            hardware.nvidiaComputeCapability = major * 10 + minor;
        }
    }

#if QT_FEATURE_vulkan == 1
    QVulkanInstance instance;
    if (instance.create()) {
        auto *const functions = instance.functions();
        uint32_t count = 0;
        if (functions->vkEnumeratePhysicalDevices(
                    instance.vkInstance(), &count, nullptr) == VK_SUCCESS &&
            count > 0) {
            QVector<VkPhysicalDevice> devices(count);
            if (functions->vkEnumeratePhysicalDevices(
                        instance.vkInstance(), &count,
                        devices.data()) == VK_SUCCESS) {
                static const QRegularExpression supportedName(
                        QStringLiteral("\\bRadeon\\s+RX\\s+[79][0-9]{3}\\b"),
                        QRegularExpression::CaseInsensitiveOption);
                for (const auto device : devices) {
                    VkPhysicalDeviceProperties properties{};
                    functions->vkGetPhysicalDeviceProperties(device, &properties);
                    if (properties.vendorID == 0x1002 &&
                        supportedName.match(QString::fromUtf8(
                                properties.deviceName)).hasMatch()) {
                        hardware.amdRadeonRx7000Or9000 = true;
                        break;
                    }
                }
            }
        }
    }
#endif
    return hardware;
}

} // namespace forevertas::app
