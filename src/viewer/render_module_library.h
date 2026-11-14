#ifndef FOREVERTAS_VIEWER_RENDER_MODULE_LIBRARY_H
#define FOREVERTAS_VIEWER_RENDER_MODULE_LIBRARY_H

#include "viewer/render_module_api.h"

#include <QString>

#include <memory>
#include <vector>

#include <forevervalidator/experimental/physics_sandbox.h>

class QOffscreenSurface;
class QOpenGLContext;

namespace forevertas::viewer {

// The optional render module (render_module_api.h), loaded once: next to
// the executable, in the library folder beside it (AppImage), or where
// FOREVERTAS_RENDER_MODULE points.
struct RenderModuleLibrary {
    bool loaded = false;
    QString label;
    FtrmCreateFunction create = nullptr;
    FtrmPrepareFunction prepare = nullptr;
    FtrmUploadFunction upload = nullptr;
    FtrmSetControlsFunction setControls = nullptr;
    FtrmRenderFunction render = nullptr;
    FtrmReleaseFunction release = nullptr;
    FtrmDestroyFunction destroy = nullptr;
    // Null when the module cannot export video.
    FtrmExportVideoFunction exportVideo = nullptr;
    // Null for modules older than the car diagnostics.
    FtrmCarStateFunction carState = nullptr;
};

const RenderModuleLibrary &LoadedRenderModule();

// Whether this computer offers OpenGL 3.3 or newer, which the module draws
// with. Needs the application object.
bool RenderModuleOpenGlUsable();

// Where the module keeps work worth reusing between runs.
QString RenderModuleCacheDirectory();

// A run's controls as the module takes them.
std::vector<FtrmControl> RenderModuleControls(
        const std::vector<forevervalidator::experimental::PhysicsSandboxInputEvent>
                &events);

// An OpenGL context of ForeverTAS's own for a worker thread, which the
// module bakes and exports with: one the module made itself could clash
// with the window's (two OpenGL drivers in one process). Created and
// destroyed on the GUI thread; used on one worker thread.
class RenderModuleWorkerContext {
public:
    RenderModuleWorkerContext();
    ~RenderModuleWorkerContext();
    RenderModuleWorkerContext(const RenderModuleWorkerContext &) = delete;
    RenderModuleWorkerContext &operator=(const RenderModuleWorkerContext &) = delete;

    // On the worker thread: a context current there, or false.
    bool makeCurrent();
    // On the worker thread, when done with it.
    void release();

private:
    std::unique_ptr<QOffscreenSurface> surface_;
    std::unique_ptr<QOpenGLContext> context_;
};

}  // namespace forevertas::viewer

#endif
