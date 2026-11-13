#include "viewer/render_module_view.h"

#include "viewer/race_viewer_controller.h"
#include "viewer/render_module_library.h"

#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>
#include <QQuickWindow>
#include <QRunnable>
#include <QSGRendererInterface>
#include <QSGSimpleTextureNode>
#include <QSGTexture>
#include <QSGTextureProvider>
#include <QtQuick/qsgtexture_platform.h>
#include <QThread>
#include <QtMath>
#include <rhi/qrhi.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>

#ifndef GL_FRAMEBUFFER_SRGB
#define GL_FRAMEBUFFER_SRGB 0x8DB9
#endif
#ifndef GL_PRIMITIVE_RESTART
#define GL_PRIMITIVE_RESTART 0x8F9D
#endif
#ifndef GL_PRIMITIVE_RESTART_FIXED_INDEX
#define GL_PRIMITIVE_RESTART_FIXED_INDEX 0x8D69
#endif
#ifndef GL_PROGRAM_POINT_SIZE
#define GL_PROGRAM_POINT_SIZE 0x8642
#endif
#ifndef GL_TEXTURE_CUBE_MAP_SEAMLESS
#define GL_TEXTURE_CUBE_MAP_SEAMLESS 0x884F
#endif
#ifndef GL_CLIP_DISTANCE0
#define GL_CLIP_DISTANCE0 0x3000
#endif
#ifndef GL_MULTISAMPLE
#define GL_MULTISAMPLE 0x809D
#endif

namespace forevertas::viewer {
namespace {

// The OpenGL state a module may leave changed that Qt does not set again
// itself after external commands.
class SavedGlState {
public:
    explicit SavedGlState(QOpenGLExtraFunctions *gl) : gl_(gl) {
        for (std::size_t index = 0; index < kCaps.size(); ++index) {
            enabled_[index] = gl_->glIsEnabled(kCaps[index]);
        }
        gl_->glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vertexArray_);
        gl_->glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTexture_);
        gl_->glGetIntegerv(GL_PACK_ALIGNMENT, &packAlignment_);
        gl_->glGetIntegerv(GL_UNPACK_ALIGNMENT, &unpackAlignment_);
        gl_->glGetIntegerv(GL_UNPACK_ROW_LENGTH, &unpackRowLength_);
        while (gl_->glGetError() != GL_NO_ERROR) {
        }
    }

    ~SavedGlState() {
        for (std::size_t index = 0; index < kCaps.size(); ++index) {
            if (enabled_[index]) {
                gl_->glEnable(kCaps[index]);
            } else {
                gl_->glDisable(kCaps[index]);
            }
        }
        gl_->glBindVertexArray(static_cast<GLuint>(vertexArray_));
        gl_->glActiveTexture(static_cast<GLenum>(activeTexture_));
        gl_->glPixelStorei(GL_PACK_ALIGNMENT, packAlignment_);
        gl_->glPixelStorei(GL_UNPACK_ALIGNMENT, unpackAlignment_);
        gl_->glPixelStorei(GL_UNPACK_ROW_LENGTH, unpackRowLength_);
        gl_->glUseProgram(0);
        while (gl_->glGetError() != GL_NO_ERROR) {
        }
    }

private:
    static constexpr std::array<GLenum, 11> kCaps{
            GL_FRAMEBUFFER_SRGB, GL_PRIMITIVE_RESTART,
            GL_PRIMITIVE_RESTART_FIXED_INDEX, GL_POLYGON_OFFSET_FILL,
            GL_SAMPLE_ALPHA_TO_COVERAGE, GL_PROGRAM_POINT_SIZE,
            GL_TEXTURE_CUBE_MAP_SEAMLESS, GL_CLIP_DISTANCE0, GL_MULTISAMPLE,
            GL_DITHER, GL_SCISSOR_TEST};

    QOpenGLExtraFunctions *gl_;
    std::array<GLboolean, kCaps.size()> enabled_{};
    GLint vertexArray_ = 0;
    GLint activeTexture_ = GL_TEXTURE0;
    GLint packAlignment_ = 4;
    GLint unpackAlignment_ = 4;
    GLint unpackRowLength_ = 0;
};

bool SameFrame(const FtrmFrame &a, const FtrmFrame &b) {
    return std::memcmp(&a.camera, &b.camera, sizeof a.camera) == 0 &&
           a.raceTimeMs == b.raceTimeMs && a.showCar == b.showCar;
}

QString ErrorText(const std::array<char, 1024> &error,
                  const QString &fallback) {
    const QString text = QString::fromUtf8(error.data());
    return text.isEmpty() ? fallback : text;
}

}  // namespace

// The render thread's side: the module, the frame it draws next, and the
// texture it draws into. Every call into the module after its load happens
// here, in beforeRendering, with Qt's OpenGL context current.
class RenderModuleRenderer : public QSGTextureProvider {
    Q_OBJECT

public:
    explicit RenderModuleRenderer(QQuickWindow *window) : window_(window) {}

    ~RenderModuleRenderer() override {
        // Runs on the render thread with the context current.
        QOpenGLContext *const context = QOpenGLContext::currentContext();
        QOpenGLExtraFunctions *const gl =
                context != nullptr ? context->extraFunctions() : nullptr;
        if (gl != nullptr) {
            deleteFramebuffers(gl);
        }
        const RenderModuleLibrary &library = LoadedRenderModule();
        if (incoming_ != nullptr) {
            library.destroy(incoming_);
        }
        // Without its context the module cannot free its OpenGL objects;
        // the context is going anyway.
        if (module_ != nullptr && gl != nullptr) {
            library.release(module_);
            library.destroy(module_);
        }
        texture_.reset();
        delete rhiTexture_;
    }

    QSGTexture *texture() const override { return texture_.get(); }

    // --- Called in sync, with the GUI thread blocked.

    void takeModule(FtrmModule *module, quint64 source) {
        if (incoming_ != nullptr) {
            LoadedRenderModule().destroy(incoming_);
        }
        incoming_ = module;
        incomingSource_ = source;
    }

    void setFrame(bool drawing, const FtrmFrame &frame, const QSize &size,
                  const std::vector<FtrmControl> &controls,
                  quint64 controlsRevision) {
        // Drawn again only when something it shows changed.
        if (drawing && !drawing_) {
            dirty_ = true;
        }
        drawing_ = drawing;
        if (!SameFrame(frame, frame_)) {
            frame_ = frame;
            dirty_ = true;
        }
        if (controlsRevision != controlsRevision_) {
            controls_ = controls;
            controlsRevision_ = controlsRevision;
            controlsSent_ = false;
            dirty_ = true;
        }
        if (drawing && size != size_ && !size.isEmpty()) {
            resizeTexture(size);
            dirty_ = true;
        }
    }

    // --- Called on the render thread.

    void render() {
        if (!drawing_ || (!dirty_ && incoming_ == nullptr) ||
            rhiTexture_ == nullptr) {
            return;
        }
        QOpenGLContext *const context = QOpenGLContext::currentContext();
        if (context == nullptr) {
            return;
        }
        QOpenGLExtraFunctions *const gl = context->extraFunctions();
        const RenderModuleLibrary &library = LoadedRenderModule();
        std::array<char, 1024> error{};

        window_->beginExternalCommands();
        {
            const SavedGlState saved(gl);
            if (incoming_ != nullptr) {
                upload(gl, library, &error);
            }
            if (module_ != nullptr && dirty_) {
                if (!controlsSent_) {
                    library.setControls(module_, controls_.data(),
                                        static_cast<int>(controls_.size()));
                    controlsSent_ = true;
                }
                draw(gl, library, &error);
            }
        }
        window_->endExternalCommands();
        dirty_ = false;
    }

signals:
    void uploaded(quint64 source, const QString &error);
    void drawn(quint64 source);
    void failed(quint64 source, const QString &error);

private:
    void upload(QOpenGLExtraFunctions *gl, const RenderModuleLibrary &library,
                std::array<char, 1024> *error) {
        FtrmModule *const module = incoming_;
        const quint64 source = incomingSource_;
        incoming_ = nullptr;
        if (module_ != nullptr) {
            library.release(module_);
            library.destroy(module_);
            module_ = nullptr;
        }
        GLint major = 0;
        GLint minor = 0;
        gl->glGetIntegerv(GL_MAJOR_VERSION, &major);
        gl->glGetIntegerv(GL_MINOR_VERSION, &minor);
        if (major < 3 || (major == 3 && minor < 3)) {
            library.destroy(module);
            emit uploaded(source,
                          tr("The In-game view needs OpenGL 3.3 or newer."));
            return;
        }
        if (!library.upload(module, error->data(),
                            static_cast<int>(error->size()))) {
            library.release(module);
            library.destroy(module);
            emit uploaded(source, ErrorText(*error, tr("The map could not be loaded.")));
            return;
        }
        module_ = module;
        source_ = source;
        controlsSent_ = false;
        dirty_ = true;
        emit uploaded(source, QString());
    }

    void draw(QOpenGLExtraFunctions *gl, const RenderModuleLibrary &library,
              std::array<char, 1024> *error) {
        const GLuint textureId =
                static_cast<GLuint>(rhiTexture_->nativeTexture().object);
        if (!ensureFramebuffers(gl, textureId)) {
            emit failed(source_, tr("The frame could not be allocated."));
            return;
        }
        const int width = size_.width();
        const int height = size_.height();
        if (!library.render(module_, &frame_, drawFramebuffer_, width,
                            height, error->data(),
                            static_cast<int>(error->size()))) {
            emit failed(source_,
                        ErrorText(*error, tr("The map could not be drawn.")));
            return;
        }
        // Into the texture top row first, as images are.
        gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, drawFramebuffer_);
        gl->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, textureFramebuffer_);
        gl->glDisable(GL_SCISSOR_TEST);
        gl->glBlitFramebuffer(0, 0, width, height, 0, height, width, 0,
                              GL_COLOR_BUFFER_BIT, GL_NEAREST);
        emit drawn(source_);
    }

    void resizeTexture(const QSize &size) {
        QRhi *const rhi = window_->rhi();
        if (rhi == nullptr) {
            return;
        }
        auto *const texture =
                rhi->newTexture(QRhiTexture::RGBA8, size, 1,
                                QRhiTexture::RenderTarget);
        if (!texture->create()) {
            delete texture;
            return;
        }
        // The scene graph may still draw the old one this frame.
        if (rhiTexture_ != nullptr) {
            rhiTexture_->deleteLater();
        }
        rhiTexture_ = texture;
        size_ = size;
        texture_.reset(QNativeInterface::QSGOpenGLTexture::fromNative(
                static_cast<GLuint>(texture->nativeTexture().object), window_,
                size));
        texture_->setFiltering(QSGTexture::Linear);
        emit textureChanged();
    }

    bool ensureFramebuffers(QOpenGLExtraFunctions *gl, GLuint textureId) {
        if (framebufferTexture_ == textureId && framebufferSize_ == size_) {
            return true;
        }
        deleteFramebuffers(gl);
        gl->glGenFramebuffers(1, &textureFramebuffer_);
        gl->glBindFramebuffer(GL_FRAMEBUFFER, textureFramebuffer_);
        gl->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                   GL_TEXTURE_2D, textureId, 0);
        const bool textureComplete =
                gl->glCheckFramebufferStatus(GL_FRAMEBUFFER) ==
                GL_FRAMEBUFFER_COMPLETE;

        gl->glGenRenderbuffers(1, &colorRenderbuffer_);
        gl->glBindRenderbuffer(GL_RENDERBUFFER, colorRenderbuffer_);
        gl->glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, size_.width(),
                                  size_.height());
        gl->glGenRenderbuffers(1, &depthRenderbuffer_);
        gl->glBindRenderbuffer(GL_RENDERBUFFER, depthRenderbuffer_);
        gl->glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8,
                                  size_.width(), size_.height());
        gl->glGenFramebuffers(1, &drawFramebuffer_);
        gl->glBindFramebuffer(GL_FRAMEBUFFER, drawFramebuffer_);
        gl->glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                      GL_RENDERBUFFER, colorRenderbuffer_);
        gl->glFramebufferRenderbuffer(GL_FRAMEBUFFER,
                                      GL_DEPTH_STENCIL_ATTACHMENT,
                                      GL_RENDERBUFFER, depthRenderbuffer_);
        const bool drawComplete =
                gl->glCheckFramebufferStatus(GL_FRAMEBUFFER) ==
                GL_FRAMEBUFFER_COMPLETE;
        gl->glBindRenderbuffer(GL_RENDERBUFFER, 0);
        if (!textureComplete || !drawComplete) {
            deleteFramebuffers(gl);
            return false;
        }
        framebufferTexture_ = textureId;
        framebufferSize_ = size_;
        return true;
    }

    void deleteFramebuffers(QOpenGLExtraFunctions *gl) {
        if (textureFramebuffer_ != 0u) {
            gl->glDeleteFramebuffers(1, &textureFramebuffer_);
        }
        if (drawFramebuffer_ != 0u) {
            gl->glDeleteFramebuffers(1, &drawFramebuffer_);
        }
        if (colorRenderbuffer_ != 0u) {
            gl->glDeleteRenderbuffers(1, &colorRenderbuffer_);
        }
        if (depthRenderbuffer_ != 0u) {
            gl->glDeleteRenderbuffers(1, &depthRenderbuffer_);
        }
        textureFramebuffer_ = drawFramebuffer_ = 0u;
        colorRenderbuffer_ = depthRenderbuffer_ = 0u;
        framebufferTexture_ = 0u;
        framebufferSize_ = {};
    }

    QQuickWindow *window_;
    FtrmModule *module_ = nullptr;
    quint64 source_ = 0;
    FtrmModule *incoming_ = nullptr;
    quint64 incomingSource_ = 0;

    bool drawing_ = false;
    bool dirty_ = false;
    FtrmFrame frame_{};
    std::vector<FtrmControl> controls_;
    quint64 controlsRevision_ = 0;
    bool controlsSent_ = false;

    QSize size_;
    QRhiTexture *rhiTexture_ = nullptr;
    std::unique_ptr<QSGTexture> texture_;
    GLuint textureFramebuffer_ = 0u;
    GLuint drawFramebuffer_ = 0u;
    GLuint colorRenderbuffer_ = 0u;
    GLuint depthRenderbuffer_ = 0u;
    GLuint framebufferTexture_ = 0u;
    QSize framebufferSize_;
};

namespace {

// Deletes the render side on the render thread.
class RendererCleanup : public QRunnable {
public:
    explicit RendererCleanup(RenderModuleRenderer *renderer)
        : renderer_(renderer) {}
    void run() override { delete renderer_; }

private:
    RenderModuleRenderer *renderer_;
};

}  // namespace

RenderModuleView::RenderModuleView(QQuickItem *parent) : QQuickItem(parent) {
    setFlag(ItemHasContents, true);
}

RenderModuleView::~RenderModuleView() {
    if (loader_ != nullptr) {
        loader_->wait();
        delete loader_;
    }
    if (prepared_ != nullptr) {
        LoadedRenderModule().destroy(prepared_);
    }
}

bool RenderModuleView::available() const {
    return LoadedRenderModule().loaded;
}

QString RenderModuleView::label() const { return LoadedRenderModule().label; }

bool RenderModuleView::active() const { return active_; }

void RenderModuleView::setActive(bool active) {
    if (active_ == active) {
        return;
    }
    active_ = active;
    emit activeChanged();
    startLoad();
    update();
}

QString RenderModuleView::packsDirectory() const { return packsDirectory_; }

void RenderModuleView::setPacksDirectory(const QString &directory) {
    if (packsDirectory_ == directory) {
        return;
    }
    packsDirectory_ = directory;
    ++source_;
    setReady(false);
    emit sourceChanged();
    startLoad();
}

QString RenderModuleView::mapPath() const { return mapPath_; }

void RenderModuleView::setMapPath(const QString &path) {
    if (mapPath_ == path) {
        return;
    }
    mapPath_ = path;
    ++source_;
    setReady(false);
    emit sourceChanged();
    startLoad();
}

QObject *RenderModuleView::viewer() const { return viewer_; }

void RenderModuleView::setViewer(QObject *viewer) {
    auto *const controller = qobject_cast<RaceViewerController *>(viewer);
    if (viewer_ == controller) {
        return;
    }
    if (viewer_ != nullptr) {
        disconnect(viewer_, nullptr, this, nullptr);
    }
    viewer_ = controller;
    if (controller != nullptr) {
        for (const auto signal :
             {&RaceViewerController::runsChanged,
              &RaceViewerController::selectedRunChanged,
              &RaceViewerController::timelineChanged,
              &RaceViewerController::manualInputChanged}) {
            connect(controller, signal, this,
                    &RenderModuleView::refreshControls);
        }
    }
    emit viewerChanged();
    refreshControls();
}

double RenderModuleView::raceTimeMs() const { return raceTimeMs_; }

void RenderModuleView::setRaceTimeMs(double timeMs) {
    const bool changed = raceTimeMs_ != timeMs;
    raceTimeMs_ = timeMs;
    setFrameValue(changed);
}

bool RenderModuleView::showCar() const { return showCar_; }

void RenderModuleView::setShowCar(bool show) {
    const bool changed = showCar_ != show;
    showCar_ = show;
    setFrameValue(changed);
}

QVector3D RenderModuleView::cameraPosition() const { return cameraPosition_; }

void RenderModuleView::setCameraPosition(const QVector3D &position) {
    const bool changed = cameraPosition_ != position;
    cameraPosition_ = position;
    setFrameValue(changed);
}

QVector3D RenderModuleView::cameraTarget() const { return cameraTarget_; }

void RenderModuleView::setCameraTarget(const QVector3D &target) {
    const bool changed = cameraTarget_ != target;
    cameraTarget_ = target;
    setFrameValue(changed);
}

QVector3D RenderModuleView::cameraUp() const { return cameraUp_; }

void RenderModuleView::setCameraUp(const QVector3D &up) {
    const bool changed = cameraUp_ != up;
    cameraUp_ = up;
    setFrameValue(changed);
}

float RenderModuleView::fieldOfView() const { return fieldOfView_; }

void RenderModuleView::setFieldOfView(float degrees) {
    const bool changed = fieldOfView_ != degrees;
    fieldOfView_ = degrees;
    setFrameValue(changed);
}

float RenderModuleView::clipNear() const { return clipNear_; }

void RenderModuleView::setClipNear(float distance) {
    const bool changed = clipNear_ != distance;
    clipNear_ = distance;
    setFrameValue(changed);
}

float RenderModuleView::clipFar() const { return clipFar_; }

void RenderModuleView::setClipFar(float distance) {
    const bool changed = clipFar_ != distance;
    clipFar_ = distance;
    setFrameValue(changed);
}

bool RenderModuleView::ready() const { return ready_; }

QString RenderModuleView::status() const { return status_; }

QSize RenderModuleView::frameSize() const { return frameSize_; }

QRectF RenderModuleView::framing() const { return framing_; }

void RenderModuleView::setFraming(const QRectF &framing) {
    if (framing_ == framing) {
        return;
    }
    framing_ = framing;
    emit frameChanged();
    update();
}

QSize RenderModuleView::imageSize() const {
    QSize size;
    QPointF origin;
    float fieldOfView = 0.0f;
    imageLayout(&size, &origin, &fieldOfView);
    return size;
}

QPointF RenderModuleView::imageOrigin() const {
    QSize size;
    QPointF origin;
    float fieldOfView = 0.0f;
    imageLayout(&size, &origin, &fieldOfView);
    return origin;
}

void RenderModuleView::imageLayout(QSize *size, QPointF *origin,
                                   float *fieldOfView) const {
    const QSize view = pixelSize();
    const qreal ratio = window() != nullptr ? window()->devicePixelRatio() : 1.0;
    const QRectF framed = framing_.isEmpty()
            ? QRectF(QPointF(0, 0), QSizeF(view))
            : QRectF(framing_.x() * ratio, framing_.y() * ratio,
                     framing_.width() * ratio, framing_.height() * ratio);
    const QPointF center = framed.center();
    // Symmetric around the framed part's centre, reaching every edge.
    const auto even = [](double value) {
        return std::max(2, static_cast<int>(std::ceil(value / 2.0)) * 2);
    };
    const int width = even(2.0 * std::max(center.x(), view.width() - center.x()));
    const int height = even(2.0 * std::max(center.y(), view.height() - center.y()));
    *size = QSize(width, height);
    *origin = QPointF(width / 2.0 - center.x(), height / 2.0 - center.y());
    const double halfAngle = qDegreesToRadians(static_cast<double>(fieldOfView_)) / 2.0;
    *fieldOfView = static_cast<float>(qRadiansToDegrees(
            2.0 * std::atan(std::tan(halfAngle) * height /
                            std::max(1.0, framed.height()))));
}

bool RenderModuleView::isTextureProvider() const { return true; }

QSGTextureProvider *RenderModuleView::textureProvider() const {
    // Called on the render thread, like updatePaintNode.
    ensureRenderer();
    return renderer_;
}

void RenderModuleView::ensureRenderer() const {
    if (renderer_ != nullptr || window() == nullptr) {
        return;
    }
    auto *const self = const_cast<RenderModuleView *>(this);
    renderer_ = new RenderModuleRenderer(window());
    connect(window(), &QQuickWindow::beforeRendering, renderer_,
            &RenderModuleRenderer::render, Qt::DirectConnection);
    connect(window(), &QQuickWindow::sceneGraphInvalidated, self,
            &RenderModuleView::cleanup, Qt::DirectConnection);
    connect(renderer_, &RenderModuleRenderer::uploaded, self,
            &RenderModuleView::uploadFinished, Qt::QueuedConnection);
    connect(renderer_, &RenderModuleRenderer::drawn, self,
            &RenderModuleView::frameDrawn, Qt::QueuedConnection);
    connect(renderer_, &RenderModuleRenderer::failed, self,
            &RenderModuleView::frameFailed, Qt::QueuedConnection);
}

QSGNode *RenderModuleView::updatePaintNode(QSGNode *node,
                                           UpdatePaintNodeData *) {
    // The GUI thread waits meanwhile: everything the frame needs is copied
    // here, so the module's frame and the 3D view's show the same moment.
    if (window() == nullptr || !available()) {
        delete node;
        return nullptr;
    }
    ensureRenderer();
    if (prepared_ != nullptr) {
        renderer_->takeModule(prepared_, preparedSource_);
        prepared_ = nullptr;
    }
    FtrmFrame frame{};
    const QVector3D *vectors[] = {&cameraPosition_, &cameraTarget_,
                                  &cameraUp_};
    float *fields[] = {frame.camera.position, frame.camera.target,
                       frame.camera.up};
    for (int i = 0; i < 3; ++i) {
        fields[i][0] = vectors[i]->x();
        fields[i][1] = vectors[i]->y();
        fields[i][2] = vectors[i]->z();
    }
    QSize imageSize;
    QPointF imageOrigin;
    float imageFieldOfView = fieldOfView_;
    imageLayout(&imageSize, &imageOrigin, &imageFieldOfView);
    frame.camera.verticalFovDegrees = imageFieldOfView;
    frame.camera.nearClip = clipNear_;
    frame.camera.farClip = clipFar_;
    frame.raceTimeMs = raceTimeMs_;
    frame.showCar = showCar_ ? 1 : 0;
    const bool drawing = active_ && loadingSource_ != 0 &&
                         width() > 0 && height() > 0;
    renderer_->setFrame(drawing, frame, imageSize, controls_,
                        controlsRevision_);

    QSGTexture *const texture = renderer_->texture();
    if (texture == nullptr || !ready_) {
        delete node;
        return nullptr;
    }
    auto *textureNode = static_cast<QSGSimpleTextureNode *>(node);
    if (textureNode == nullptr) {
        textureNode = new QSGSimpleTextureNode;
        textureNode->setFiltering(QSGTexture::Linear);
    }
    textureNode->setTexture(texture);
    textureNode->setRect(boundingRect());
    // The module's image reaches past the view.
    textureNode->setSourceRect(QRectF(imageOrigin, QSizeF(pixelSize())));
    return textureNode;
}

void RenderModuleView::geometryChange(const QRectF &newGeometry,
                                      const QRectF &oldGeometry) {
    QQuickItem::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() != oldGeometry.size()) {
        const QSize size = pixelSize();
        if (frameSize_ != size) {
            frameSize_ = size;
            emit readyChanged();
        }
        emit frameChanged();
        update();
    }
}

void RenderModuleView::releaseResources() {
    // The window's scene graph is going: the module's OpenGL objects go
    // with it, on the render thread.
    if (renderer_ != nullptr && window() != nullptr) {
        window()->scheduleRenderJob(new RendererCleanup(renderer_),
                                    QQuickWindow::BeforeSynchronizingStage);
        renderer_ = nullptr;
    }
    reloadLater();
}

void RenderModuleView::cleanup() {
    // Render thread, scene graph invalidated: the context is still current.
    delete renderer_;
    renderer_ = nullptr;
    QMetaObject::invokeMethod(this, &RenderModuleView::reloadLater,
                              Qt::QueuedConnection);
}

void RenderModuleView::reloadLater() {
    // A new scene graph needs the map uploaded again.
    if (prepared_ == nullptr) {
        loadingSource_ = 0;
    }
    setReady(false);
    QMetaObject::invokeMethod(this, &RenderModuleView::startLoad,
                              Qt::QueuedConnection);
}

QSize RenderModuleView::pixelSize() const {
    const qreal ratio = window() != nullptr ? window()->devicePixelRatio() : 1.0;
    return QSize(std::max(1, static_cast<int>(std::lround(width() * ratio))),
                 std::max(1, static_cast<int>(std::lround(height() * ratio))));
}

void RenderModuleView::startLoad() {
    if (!available() || !active_ || packsDirectory_.isEmpty() ||
        mapPath_.isEmpty() || loadingSource_ == source_ ||
        loader_ != nullptr) {
        return;
    }
    if (window() != nullptr && window()->rendererInterface() != nullptr &&
        window()->rendererInterface()->graphicsApi() !=
                QSGRendererInterface::OpenGL) {
        setStatus(tr("The In-game view needs OpenGL 3.3, which this "
                     "computer's graphics driver does not offer."));
        return;
    }
    const quint64 source = source_;
    loadingSource_ = source;
    setReady(false);
    setStatus(tr("Loading the map…"));
    const QByteArray packs = packsDirectory_.toUtf8();
    const QByteArray map = mapPath_.toUtf8();
    const QByteArray cache = RenderModuleCacheDirectory().toUtf8();
    auto context = std::make_shared<RenderModuleWorkerContext>();
    // The slow part of a load; the render thread uploads the result.
    loader_ = QThread::create([this, packs, map, cache, source, context] {
        const RenderModuleLibrary &library = LoadedRenderModule();
        std::array<char, 1024> error{};
        const bool ownContext = context->makeCurrent();
        FtrmModule *module =
                library.create(packs.constData(), cache.constData(),
                               error.data(), static_cast<int>(error.size()));
        if (module != nullptr &&
            !library.prepare(module, map.constData(), ownContext ? 1 : 0,
                             error.data(), static_cast<int>(error.size()))) {
            library.destroy(module);
            module = nullptr;
        }
        context->release();
        const QString message = module != nullptr
                ? QString()
                : ErrorText(error, tr("The map could not be loaded."));
        QMetaObject::invokeMethod(
                this,
                [this, module, message, source, context] {
                    // The context's surface goes on this thread.
                    loadPrepared(module, message, source);
                },
                Qt::QueuedConnection);
    });
    loader_->setObjectName(QStringLiteral("RenderModuleLoad"));
    loader_->start();
}

void RenderModuleView::loadPrepared(FtrmModule *module, const QString &error,
                                    quint64 source) {
    loader_->wait();
    delete loader_;
    loader_ = nullptr;
    if (source != source_) {
        if (module != nullptr) {
            LoadedRenderModule().destroy(module);
        }
        loadingSource_ = 0;
        startLoad();
        return;
    }
    if (module == nullptr) {
        // Tried again only when the packs or the map change.
        setStatus(error);
        return;
    }
    if (prepared_ != nullptr) {
        LoadedRenderModule().destroy(prepared_);
    }
    prepared_ = module;
    preparedSource_ = source;
    setStatus(tr("Loading the map…"));
    update();
}

void RenderModuleView::uploadFinished(quint64 source, const QString &error) {
    if (source != source_) {
        return;
    }
    if (!error.isEmpty()) {
        setReady(false);
        setStatus(error);
        return;
    }
    update();
}

void RenderModuleView::frameDrawn(quint64 source) {
    if (source != source_ || ready_) {
        return;
    }
    frameSize_ = pixelSize();
    setStatus(QString());
    setReady(true);
}

void RenderModuleView::frameFailed(quint64 source, const QString &error) {
    if (source != source_) {
        return;
    }
    setReady(false);
    setStatus(error);
}

void RenderModuleView::refreshControls() {
    std::vector<FtrmControl> controls =
            viewer_ != nullptr ? RenderModuleControls(viewer_->selectedRunInputs())
                               : std::vector<FtrmControl>{};
    const bool same = controls.size() == controls_.size() &&
            std::equal(controls.begin(), controls.end(), controls_.begin(),
                       [](const FtrmControl &a, const FtrmControl &b) {
                           return a.timeMs == b.timeMs &&
                                  a.control == b.control &&
                                  a.value == b.value;
                       });
    if (same) {
        return;
    }
    controls_ = std::move(controls);
    ++controlsRevision_;
    emit frameChanged();
    update();
}

void RenderModuleView::setFrameValue(bool changed) {
    if (!changed) {
        return;
    }
    emit frameChanged();
    if (active_) {
        update();
    }
}

void RenderModuleView::setStatus(const QString &status) {
    if (status_ != status) {
        status_ = status;
        emit statusChanged();
    }
}

void RenderModuleView::setReady(bool ready) {
    if (ready_ != ready) {
        ready_ = ready;
        emit readyChanged();
        update();
    }
}

}  // namespace forevertas::viewer

#include "render_module_view.moc"
