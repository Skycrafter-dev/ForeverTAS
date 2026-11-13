#include "viewer/render_module_library.h"

#include <QCoreApplication>
#include <QDebug>
#include <QLibrary>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QStandardPaths>
#include <QStringList>

namespace forevertas::viewer {

using forevervalidator::experimental::PhysicsSandboxInputAction;
using forevervalidator::experimental::PhysicsSandboxInputEvent;
using forevervalidator::experimental::PhysicsSandboxInputValueKind;
using forevervalidator::experimental::PhysicsSandboxSwitchState;

const RenderModuleLibrary &LoadedRenderModule() {
    static const RenderModuleLibrary library = [] {
        RenderModuleLibrary result;
        QStringList candidates;
        const QString overridePath =
                qEnvironmentVariable("FOREVERTAS_RENDER_MODULE");
        if (!overridePath.isEmpty()) {
            candidates << overridePath;
        }
        const QString base = QCoreApplication::applicationDirPath();
        candidates << base + QStringLiteral("/forevertas-render-module")
                   << base + QStringLiteral("/../lib/forevertas-render-module");
        for (const QString &candidate : candidates) {
            // Kept loaded for the rest of the run.
            auto *const module = new QLibrary(candidate);
            if (!module->load()) {
                if (candidate == overridePath) {
                    qWarning().noquote() << "render module not loaded:"
                                         << module->errorString();
                }
                delete module;
                continue;
            }
            const auto resolve = [module](const char *name) {
                return module->resolve(name);
            };
            const auto version = reinterpret_cast<FtrmApiVersionFunction>(
                    resolve("ftrm_api_version"));
            const auto label = reinterpret_cast<FtrmModeLabelFunction>(
                    resolve("ftrm_mode_label"));
            result.create = reinterpret_cast<FtrmCreateFunction>(
                    resolve("ftrm_create"));
            result.prepare = reinterpret_cast<FtrmPrepareFunction>(
                    resolve("ftrm_prepare"));
            result.upload = reinterpret_cast<FtrmUploadFunction>(
                    resolve("ftrm_upload"));
            result.setControls = reinterpret_cast<FtrmSetControlsFunction>(
                    resolve("ftrm_set_controls"));
            result.render = reinterpret_cast<FtrmRenderFunction>(
                    resolve("ftrm_render"));
            result.release = reinterpret_cast<FtrmReleaseFunction>(
                    resolve("ftrm_release"));
            result.destroy = reinterpret_cast<FtrmDestroyFunction>(
                    resolve("ftrm_destroy"));
            result.exportVideo = reinterpret_cast<FtrmExportVideoFunction>(
                    resolve("ftrm_export_video"));
            if (version == nullptr || label == nullptr ||
                version() != FTRM_API_VERSION || result.create == nullptr ||
                result.prepare == nullptr || result.upload == nullptr ||
                result.setControls == nullptr || result.render == nullptr ||
                result.release == nullptr || result.destroy == nullptr) {
                if (candidate == overridePath) {
                    qWarning() << "render module not loaded: wrong version";
                }
                module->unload();
                delete module;
                result = {};
                continue;
            }
            result.label = QString::fromUtf8(label());
            result.loaded = true;
            break;
        }
        return result;
    }();
    return library;
}

bool RenderModuleOpenGlUsable() {
    RenderModuleWorkerContext context;
    const bool usable = context.makeCurrent();
    context.release();
    return usable;
}

QString RenderModuleCacheDirectory() {
    return QStandardPaths::writableLocation(QStandardPaths::CacheLocation) +
           QStringLiteral("/render-module");
}

std::vector<FtrmControl> RenderModuleControls(
        const std::vector<PhysicsSandboxInputEvent> &events) {
    std::vector<FtrmControl> controls;
    controls.reserve(events.size());
    for (const PhysicsSandboxInputEvent &event : events) {
        FtrmControl control{};
        control.timeMs = event.timeMs;
        switch (event.action) {
        case PhysicsSandboxInputAction::Accelerate:
            control.control = FTRM_ACCELERATE;
            break;
        case PhysicsSandboxInputAction::Brake:
            control.control = FTRM_BRAKE;
            break;
        case PhysicsSandboxInputAction::SteerLeft:
            control.control = FTRM_STEER_LEFT;
            break;
        case PhysicsSandboxInputAction::SteerRight:
            control.control = FTRM_STEER_RIGHT;
            break;
        case PhysicsSandboxInputAction::Steer:
            control.control = FTRM_STEER;
            break;
        case PhysicsSandboxInputAction::Gas:
            control.control = FTRM_GAS;
            break;
        case PhysicsSandboxInputAction::Respawn:
            control.control = FTRM_RESPAWN;
            break;
        default:
            // Race start and finish markers: the module's race knows them.
            continue;
        }
        if (event.value.kind == PhysicsSandboxInputValueKind::Analog) {
            control.value = static_cast<int>(event.value.analog);
        } else if (event.value.kind == PhysicsSandboxInputValueKind::Switch) {
            control.value = event.value.switchState !=
                                    PhysicsSandboxSwitchState::Released
                    ? 1
                    : 0;
        } else {
            control.value = 1;
        }
        controls.push_back(control);
    }
    return controls;
}

RenderModuleWorkerContext::RenderModuleWorkerContext()
    : surface_(std::make_unique<QOffscreenSurface>()) {
    surface_->setFormat(QSurfaceFormat::defaultFormat());
    surface_->create();
}

RenderModuleWorkerContext::~RenderModuleWorkerContext() = default;

bool RenderModuleWorkerContext::makeCurrent() {
    if (!surface_->isValid()) {
        return false;
    }
    // The newest compatibility context first (it has compute shaders for
    // the light-map bake), then a plain 3.3 core one.
    for (const bool core : {false, true}) {
        QSurfaceFormat format = QSurfaceFormat::defaultFormat();
        if (core) {
            format.setVersion(3, 3);
            format.setProfile(QSurfaceFormat::CoreProfile);
        }
        auto context = std::make_unique<QOpenGLContext>();
        context->setFormat(format);
        if (!context->create() || !context->makeCurrent(surface_.get())) {
            continue;
        }
        const QSurfaceFormat actual = context->format();
        if (actual.majorVersion() > 3 ||
            (actual.majorVersion() == 3 && actual.minorVersion() >= 3)) {
            context_ = std::move(context);
            return true;
        }
        context->doneCurrent();
    }
    return false;
}

void RenderModuleWorkerContext::release() {
    if (context_ != nullptr) {
        context_->doneCurrent();
        context_.reset();
    }
}

}  // namespace forevertas::viewer
