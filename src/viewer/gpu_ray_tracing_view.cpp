#include "viewer/gpu_ray_tracing_view.h"

#include "viewer/material_classifier.h"
#include "viewer/race_viewer_controller.h"

#include <QQuaternion>
#include <QVariantMap>
#include <QtMath>

#include <algorithm>
#include <cmath>
#include <utility>

#if FOREVERTAS_GPU_RAY_TRACING
#include <QFile>
#include <QImage>
#include <QDebug>
#include <rhi/qrhi.h>
#include <rhi/qshader.h>

#include <array>
#include <memory>
#include <vector>
#endif

namespace forevertas::viewer {

#if FOREVERTAS_GPU_RAY_TRACING
namespace {

constexpr int kMaterialCount = 17;
constexpr int kMaximumCarEllipsoids = 16;

QShader LoadShader(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly)
            ? QShader::fromSerialized(file.readAll())
            : QShader();
}

QString ResourcePath(const QString &url) {
    return url.startsWith(QStringLiteral("qrc:"))
            ? url.sliced(3)
            : url;
}

QVector3D SafeNormalized(const QVector3D &value,
                         const QVector3D &fallback) {
    return value.lengthSquared() > 1.0e-8f ? value.normalized() : fallback;
}

bool NearlyEqual(const QVector3D &a, const QVector3D &b) {
    return (a - b).lengthSquared() < 1.0e-8f;
}

bool NearlyEqual(const QQuaternion &a, const QQuaternion &b) {
    return std::abs(QQuaternion::dotProduct(a, b)) > 0.999999f;
}

struct CarEllipsoid {
    QVector3D center;
    QVector3D radii;
    QQuaternion inverseRotation;
};

struct alignas(16) RayTracingUniforms {
    std::array<float, 4> cameraOrigin{};
    std::array<float, 4> cameraForward{};
    std::array<float, 4> cameraRight{};
    std::array<float, 4> cameraUp{};
    std::array<float, 4> sunDirection{};
    std::array<float, 4> viewportFrame{};
    std::array<float, 4> renderInfo{};
    std::array<float, 4> carColor{};
    std::array<std::array<float, 4>, kMaximumCarEllipsoids>
            ellipsoidCenter{};
    std::array<std::array<float, 4>, kMaximumCarEllipsoids>
            ellipsoidRadii{};
    std::array<std::array<float, 4>, kMaximumCarEllipsoids>
            ellipsoidInverseRotation{};
};

class GpuRayTracingRenderer final : public QQuickRhiItemRenderer {
public:
    void initialize(QRhiCommandBuffer *commandBuffer) override;
    void synchronize(QQuickRhiItem *item) override;
    void render(QRhiCommandBuffer *commandBuffer) override;

private:
    void releaseResources();
    bool createSceneResources(QRhiCommandBuffer *commandBuffer);
    bool createOutputResources();
    RayTracingUniforms uniforms(const QSize &outputSize) const;

    QRhi *rhi_ = nullptr;
    std::shared_ptr<const RayTracingSceneData> scene_;
    std::shared_ptr<const RayTracingSceneData> uploadedScene_;
    std::vector<CarEllipsoid> carEllipsoids_;
    QVector3D cameraPosition_{};
    QVector3D cameraTarget_{};
    QVector3D cameraUp_{0.0f, 1.0f, 0.0f};
    float fieldOfView_ = 55.0f;
    float viewportAspect_ = 1.0f;
    bool active_ = false;
    bool failureReported_ = false;
    bool sceneDirty_ = true;
    bool outputDirty_ = true;
    QSize outputSize_;

    std::unique_ptr<QRhiBuffer> vertexBuffer_;
    std::unique_ptr<QRhiBuffer> triangleBuffer_;
    std::unique_ptr<QRhiBuffer> bvhBuffer_;
    std::unique_ptr<QRhiBuffer> materialBuffer_;
    std::unique_ptr<QRhiBuffer> uniformBuffer_;
    std::unique_ptr<QRhiTexture> materialTextures_;
    std::unique_ptr<QRhiTexture> environmentTexture_;
    std::unique_ptr<QRhiTexture> outputTexture_;
    std::unique_ptr<QRhiSampler> materialSampler_;
    std::unique_ptr<QRhiSampler> environmentSampler_;
    std::unique_ptr<QRhiSampler> outputSampler_;
    std::unique_ptr<QRhiShaderResourceBindings> computeBindings_;
    std::unique_ptr<QRhiShaderResourceBindings> presentBindings_;
    std::unique_ptr<QRhiComputePipeline> computePipeline_;
    std::unique_ptr<QRhiGraphicsPipeline> presentPipeline_;
};

void GpuRayTracingRenderer::releaseResources() {
    presentPipeline_.reset();
    computePipeline_.reset();
    presentBindings_.reset();
    computeBindings_.reset();
    outputSampler_.reset();
    environmentSampler_.reset();
    materialSampler_.reset();
    outputTexture_.reset();
    environmentTexture_.reset();
    materialTextures_.reset();
    uniformBuffer_.reset();
    materialBuffer_.reset();
    bvhBuffer_.reset();
    triangleBuffer_.reset();
    vertexBuffer_.reset();
    uploadedScene_.reset();
    outputSize_ = {};
}

void GpuRayTracingRenderer::initialize(
        QRhiCommandBuffer *commandBuffer) {
    Q_UNUSED(commandBuffer);
    if (rhi_ != rhi()) {
        releaseResources();
        rhi_ = rhi();
        sceneDirty_ = true;
        outputDirty_ = true;
    }
    if (outputSize_ != renderTarget()->pixelSize()) {
        outputSize_ = renderTarget()->pixelSize();
        outputDirty_ = true;
    }
}

void GpuRayTracingRenderer::synchronize(QQuickRhiItem *rhiItem) {
    auto *const item = static_cast<GpuRayTracingView *>(rhiItem);
    const std::shared_ptr<const RayTracingSceneData> nextScene =
            item->active() && item->viewerController() != nullptr
            ? item->viewerController()->rayTracingScene()
            : nullptr;
    if (scene_ != nextScene) {
        scene_ = nextScene;
        sceneDirty_ = true;
    }

    std::vector<CarEllipsoid> nextEllipsoids;
    if (item->viewerController() != nullptr) {
        const RaceViewerController *const viewer =
                item->viewerController();
        const QQuaternion carRotation = viewer->carRotation().normalized();
        const QVector3D carPosition = viewer->carPosition();
        const QVariantList ellipsoidItems = viewer->carEllipsoids();
        nextEllipsoids.reserve(static_cast<std::size_t>(
                std::min<qsizetype>(
                        ellipsoidItems.size(),
                        kMaximumCarEllipsoids)));
        for (qsizetype index = 0;
             index < ellipsoidItems.size() &&
             index < kMaximumCarEllipsoids;
             ++index) {
            const QVariantMap map = ellipsoidItems[index].toMap();
            const QVector3D localPosition =
                    map.value(QStringLiteral("position"))
                            .value<QVector3D>();
            const QQuaternion localRotation =
                    map.value(QStringLiteral("rotation"))
                            .value<QQuaternion>();
            CarEllipsoid ellipsoid;
            ellipsoid.center =
                    carPosition +
                    carRotation.rotatedVector(localPosition);
            ellipsoid.radii =
                    map.value(QStringLiteral("radii"))
                            .value<QVector3D>();
            ellipsoid.inverseRotation =
                    (carRotation * localRotation).normalized().inverted();
            nextEllipsoids.push_back(ellipsoid);
        }
    }

    const QVector3D nextPosition = item->cameraPosition();
    const QVector3D nextTarget = item->cameraTarget();
    const QVector3D nextUp = item->cameraUp();
    const float nextFieldOfView = item->fieldOfView();
    const float nextViewportAspect =
            item->height() > 0.0
            ? static_cast<float>(item->width() / item->height())
            : 1.0f;
    const bool carChanged =
            nextEllipsoids.size() != carEllipsoids_.size() ||
            !std::equal(
                    nextEllipsoids.begin(), nextEllipsoids.end(),
                    carEllipsoids_.begin(),
                    [](const CarEllipsoid &a, const CarEllipsoid &b) {
                        return NearlyEqual(a.center, b.center) &&
                               NearlyEqual(a.radii, b.radii) &&
                               NearlyEqual(a.inverseRotation,
                                           b.inverseRotation);
                    });
    if (!NearlyEqual(cameraPosition_, nextPosition) ||
        !NearlyEqual(cameraTarget_, nextTarget) ||
        !NearlyEqual(cameraUp_, nextUp) ||
        !qFuzzyCompare(fieldOfView_, nextFieldOfView) ||
        !qFuzzyCompare(viewportAspect_, nextViewportAspect) ||
        carChanged) {
        cameraPosition_ = nextPosition;
        cameraTarget_ = nextTarget;
        cameraUp_ = nextUp;
        fieldOfView_ = nextFieldOfView;
        viewportAspect_ = nextViewportAspect;
        carEllipsoids_ = std::move(nextEllipsoids);
    }
    active_ = item->active();
}

bool GpuRayTracingRenderer::createSceneResources(
        QRhiCommandBuffer *commandBuffer) {
    if (scene_ == nullptr || scene_->triangleCount == 0u ||
        scene_->bvhNodeCount == 0u) {
        if (!failureReported_) {
            qWarning() << "GPU ray tracing has no traceable scene";
            failureReported_ = true;
        }
        return false;
    }

    computePipeline_.reset();
    computeBindings_.reset();
    presentPipeline_.reset();
    presentBindings_.reset();

    auto createBuffer =
            [this](const QByteArray &bytes)
            -> std::unique_ptr<QRhiBuffer> {
        std::unique_ptr<QRhiBuffer> buffer(
                rhi_->newBuffer(QRhiBuffer::Immutable,
                                QRhiBuffer::StorageBuffer,
                                static_cast<quint32>(bytes.size())));
        return buffer->create() ? std::move(buffer) : nullptr;
    };
    vertexBuffer_ = createBuffer(scene_->vertices);
    triangleBuffer_ = createBuffer(scene_->triangles);
    bvhBuffer_ = createBuffer(scene_->bvhNodes);
    materialBuffer_ = createBuffer(scene_->materials);
    uniformBuffer_.reset(rhi_->newBuffer(
            QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer,
            static_cast<quint32>(sizeof(RayTracingUniforms))));
    if (vertexBuffer_ == nullptr || triangleBuffer_ == nullptr ||
        bvhBuffer_ == nullptr || materialBuffer_ == nullptr ||
        !uniformBuffer_->create()) {
        if (!failureReported_) {
            qWarning() << "GPU ray tracing could not create scene buffers";
            failureReported_ = true;
        }
        return false;
    }

    const QRhiTexture::Flags mipmappedTextureFlags =
            QRhiTexture::sRGB | QRhiTexture::MipMapped |
            QRhiTexture::UsedWithGenerateMips;
    materialTextures_.reset(rhi_->newTextureArray(
            QRhiTexture::RGBA8, kMaterialCount, QSize(512, 512), 1,
            mipmappedTextureFlags));
    environmentTexture_.reset(rhi_->newTexture(
            QRhiTexture::RGBA8, QSize(2048, 1024), 1,
            mipmappedTextureFlags));
    if (!materialTextures_->create() ||
        !environmentTexture_->create()) {
        if (!failureReported_) {
            qWarning() << "GPU ray tracing could not create source textures";
            failureReported_ = true;
        }
        return false;
    }

    materialSampler_.reset(rhi_->newSampler(
            QRhiSampler::Linear, QRhiSampler::Linear,
            QRhiSampler::Linear, QRhiSampler::Repeat,
            QRhiSampler::Repeat));
    environmentSampler_.reset(rhi_->newSampler(
            QRhiSampler::Linear, QRhiSampler::Linear,
            QRhiSampler::Linear, QRhiSampler::Repeat,
            QRhiSampler::ClampToEdge));
    outputSampler_.reset(rhi_->newSampler(
            QRhiSampler::Linear, QRhiSampler::Linear,
            QRhiSampler::None, QRhiSampler::ClampToEdge,
            QRhiSampler::ClampToEdge));
    if (!materialSampler_->create() || !environmentSampler_->create() ||
        !outputSampler_->create()) {
        if (!failureReported_) {
            qWarning() << "GPU ray tracing could not create samplers";
            failureReported_ = true;
        }
        return false;
    }

    QRhiResourceUpdateBatch *const updates =
            rhi_->nextResourceUpdateBatch();
    updates->uploadStaticBuffer(vertexBuffer_.get(), scene_->vertices);
    updates->uploadStaticBuffer(triangleBuffer_.get(),
                                scene_->triangles);
    updates->uploadStaticBuffer(bvhBuffer_.get(), scene_->bvhNodes);
    updates->uploadStaticBuffer(materialBuffer_.get(),
                                scene_->materials);

    std::vector<QRhiTextureUploadEntry> textureEntries;
    textureEntries.reserve(kMaterialCount);
    for (int index = 0; index < kMaterialCount; ++index) {
        const ReplacementMaterial replacement = ReplacementFor(
                static_cast<ReplacementMaterialClass>(index));
        QImage image(ResourcePath(replacement.baseTexture));
        image = image.convertToFormat(QImage::Format_RGBA8888)
                        .scaled(512, 512, Qt::IgnoreAspectRatio,
                                Qt::SmoothTransformation);
        textureEntries.emplace_back(
                index, 0,
                QRhiTextureSubresourceUploadDescription(image));
    }
    QRhiTextureUploadDescription textureUpload;
    textureUpload.setEntries(textureEntries.cbegin(),
                             textureEntries.cend());
    updates->uploadTexture(materialTextures_.get(), textureUpload);
    updates->generateMips(materialTextures_.get());

    QImage environment(QStringLiteral(
            ":/environment/day_sky.png"));
    environment =
            environment.convertToFormat(QImage::Format_RGBA8888)
                    .scaled(2048, 1024, Qt::IgnoreAspectRatio,
                            Qt::SmoothTransformation);
    updates->uploadTexture(environmentTexture_.get(), environment);
    updates->generateMips(environmentTexture_.get());
    commandBuffer->resourceUpdate(updates);

    uploadedScene_ = scene_;
    sceneDirty_ = false;
    outputDirty_ = true;
    return true;
}

bool GpuRayTracingRenderer::createOutputResources() {
    if (!outputSize_.isValid() || outputSize_.isEmpty()) return false;

    outputTexture_.reset(rhi_->newTexture(
            QRhiTexture::RGBA32F, outputSize_, 1,
            QRhiTexture::UsedWithLoadStore));
    if (!outputTexture_->create()) {
        if (!failureReported_) {
            qWarning() << "GPU ray tracing could not create output texture"
                       << outputSize_;
            failureReported_ = true;
        }
        return false;
    }

    computeBindings_.reset(rhi_->newShaderResourceBindings());
    computeBindings_->setBindings({
            QRhiShaderResourceBinding::uniformBuffer(
                    0, QRhiShaderResourceBinding::ComputeStage,
                    uniformBuffer_.get()),
            QRhiShaderResourceBinding::bufferLoad(
                    1, QRhiShaderResourceBinding::ComputeStage,
                    bvhBuffer_.get()),
            QRhiShaderResourceBinding::bufferLoad(
                    2, QRhiShaderResourceBinding::ComputeStage,
                    triangleBuffer_.get()),
            QRhiShaderResourceBinding::bufferLoad(
                    3, QRhiShaderResourceBinding::ComputeStage,
                    vertexBuffer_.get()),
            QRhiShaderResourceBinding::bufferLoad(
                    4, QRhiShaderResourceBinding::ComputeStage,
                    materialBuffer_.get()),
            QRhiShaderResourceBinding::sampledTexture(
                    5, QRhiShaderResourceBinding::ComputeStage,
                    materialTextures_.get(), materialSampler_.get()),
            QRhiShaderResourceBinding::sampledTexture(
                    6, QRhiShaderResourceBinding::ComputeStage,
                    environmentTexture_.get(),
                    environmentSampler_.get()),
            QRhiShaderResourceBinding::imageLoadStore(
                    7, QRhiShaderResourceBinding::ComputeStage,
                    outputTexture_.get(), 0),
    });
    if (!computeBindings_->create()) {
        if (!failureReported_) {
            qWarning() << "GPU ray tracing could not create compute bindings";
            failureReported_ = true;
        }
        return false;
    }

    computePipeline_.reset(rhi_->newComputePipeline());
    computePipeline_->setShaderStage({
            QRhiShaderStage::Compute,
            LoadShader(QStringLiteral(
                    ":/raytracing/shaders/raytrace.comp.qsb"))});
    computePipeline_->setShaderResourceBindings(
            computeBindings_.get());
    if (!computePipeline_->create()) {
        if (!failureReported_) {
            qWarning() << "GPU ray tracing could not create compute pipeline";
            failureReported_ = true;
        }
        return false;
    }

    presentBindings_.reset(rhi_->newShaderResourceBindings());
    presentBindings_->setBindings({
            QRhiShaderResourceBinding::sampledTexture(
                    0, QRhiShaderResourceBinding::FragmentStage,
                    outputTexture_.get(),
                    outputSampler_.get()),
    });
    if (!presentBindings_->create()) {
        if (!failureReported_) {
            qWarning() << "GPU ray tracing could not create present bindings";
            failureReported_ = true;
        }
        return false;
    }

    presentPipeline_.reset(rhi_->newGraphicsPipeline());
    presentPipeline_->setShaderStages({
            {QRhiShaderStage::Vertex,
             LoadShader(QStringLiteral(
                     ":/raytracing/shaders/raytrace_present.vert.qsb"))},
            {QRhiShaderStage::Fragment,
             LoadShader(QStringLiteral(
                     ":/raytracing/shaders/raytrace_present.frag.qsb"))},
    });
    presentPipeline_->setShaderResourceBindings(
            presentBindings_.get());
    presentPipeline_->setRenderPassDescriptor(
            renderTarget()->renderPassDescriptor());
    if (!presentPipeline_->create()) {
        if (!failureReported_) {
            qWarning() << "GPU ray tracing could not create present pipeline";
            failureReported_ = true;
        }
        return false;
    }

    outputDirty_ = false;
    failureReported_ = false;
    return true;
}

RayTracingUniforms GpuRayTracingRenderer::uniforms(
        const QSize &outputSize) const {
    const QVector3D origin = cameraPosition_;
    const QVector3D forward = SafeNormalized(
            cameraTarget_ - cameraPosition_,
            QVector3D(0.0f, 0.0f, -1.0f));
    const QVector3D right = SafeNormalized(
            QVector3D::crossProduct(forward, cameraUp_),
            QVector3D(1.0f, 0.0f, 0.0f));
    const QVector3D up = SafeNormalized(
            QVector3D::crossProduct(right, forward),
            QVector3D(0.0f, 1.0f, 0.0f));

    RayTracingUniforms value;
    value.cameraOrigin = {origin.x(), origin.y(), origin.z(), 0.0f};
    value.cameraForward =
            {forward.x(), forward.y(), forward.z(), 0.0f};
    value.cameraRight = {right.x(), right.y(), right.z(), 0.0f};
    value.cameraUp = {up.x(), up.y(), up.z(), 0.0f};
    const QVector3D sun = QVector3D(-0.45f, 0.82f, -0.35f).normalized();
    value.sunDirection = {sun.x(), sun.y(), sun.z(), 0.0f};
    value.viewportFrame = {
            static_cast<float>(outputSize.width()),
            static_cast<float>(outputSize.height()),
            0.0f,
            static_cast<float>(scene_->triangleCount)};
    value.renderInfo = {
            std::tan(qDegreesToRadians(fieldOfView_ * 0.5f)),
            static_cast<float>(scene_->bvhNodeCount),
            static_cast<float>(carEllipsoids_.size()),
            viewportAspect_};
    value.carColor = {0.12f, 0.78f, 0.68f, 0.24f};
    for (std::size_t index = 0; index < carEllipsoids_.size();
         ++index) {
        const CarEllipsoid &ellipsoid = carEllipsoids_[index];
        value.ellipsoidCenter[index] = {
                ellipsoid.center.x(), ellipsoid.center.y(),
                ellipsoid.center.z(), 0.0f};
        value.ellipsoidRadii[index] = {
                ellipsoid.radii.x(), ellipsoid.radii.y(),
                ellipsoid.radii.z(), 0.0f};
        value.ellipsoidInverseRotation[index] = {
                ellipsoid.inverseRotation.x(),
                ellipsoid.inverseRotation.y(),
                ellipsoid.inverseRotation.z(),
                ellipsoid.inverseRotation.scalar()};
    }
    return value;
}

void GpuRayTracingRenderer::render(
        QRhiCommandBuffer *commandBuffer) {
    const QColor clearColor = QColor::fromRgbF(0.35f, 0.62f, 0.82f);
    if (!active_ || scene_ == nullptr ||
        !rhi_->isFeatureSupported(QRhi::Compute) ||
        !rhi_->isFeatureSupported(QRhi::TextureArrays) ||
        !rhi_->isTextureFormatSupported(QRhiTexture::RGBA32F)) {
        if (uploadedScene_ && uploadedScene_ != scene_) releaseResources();
        commandBuffer->beginPass(
                renderTarget(), clearColor, {1.0f, 0});
        commandBuffer->endPass();
        return;
    }

    if (sceneDirty_ || uploadedScene_ != scene_) {
        if (!createSceneResources(commandBuffer)) return;
    }
    if (outputDirty_ || outputTexture_ == nullptr) {
        if (!createOutputResources()) return;
    }

    const RayTracingUniforms uniformData = uniforms(outputSize_);
    QRhiResourceUpdateBatch *const updates =
            rhi_->nextResourceUpdateBatch();
    updates->updateDynamicBuffer(
            uniformBuffer_.get(), 0,
            static_cast<quint32>(sizeof(uniformData)), &uniformData);

    commandBuffer->beginComputePass(updates);
    commandBuffer->setComputePipeline(computePipeline_.get());
    commandBuffer->setShaderResources(computeBindings_.get());
    commandBuffer->dispatch((outputSize_.width() + 7) / 8,
                            (outputSize_.height() + 7) / 8, 1);
    commandBuffer->endComputePass();

    commandBuffer->beginPass(
            renderTarget(), Qt::black, {1.0f, 0});
    commandBuffer->setGraphicsPipeline(presentPipeline_.get());
    commandBuffer->setViewport(QRhiViewport(
            0, 0, outputSize_.width(), outputSize_.height()));
    commandBuffer->setShaderResources(presentBindings_.get());
    commandBuffer->draw(3);
    commandBuffer->endPass();

    update();
}

}  // namespace
#endif

GpuRayTracingView::GpuRayTracingView(QQuickItem *parent)
#if FOREVERTAS_GPU_RAY_TRACING
    : QQuickRhiItem(parent) {
    setSampleCount(1);
    setColorBufferFormat(TextureFormat::RGBA8);
#else
    : QQuickItem(parent) {
#endif
}

QObject *GpuRayTracingView::viewer() const { return viewer_; }

void GpuRayTracingView::setViewer(QObject *viewer) {
    auto *const controller = qobject_cast<RaceViewerController *>(viewer);
    if (viewer_ == controller) return;
    if (viewer_) disconnect(viewer_, nullptr, this, nullptr);
    viewer_ = controller;
    if (viewer_) {
        connect(viewer_, &RaceViewerController::rayTracingSceneChanged,
                this, [this]() { update(); });
        connect(viewer_, &RaceViewerController::sceneChanged, this, [this]() {
            if (active_ && supported()) viewer_->requestRayTracingScene();
            update();
        });
        if (active_ && supported()) viewer_->requestRayTracingScene();
    }
    emit viewerChanged();
    update();
}

bool GpuRayTracingView::active() const { return active_; }

void GpuRayTracingView::setActive(bool active) {
    if (active_ == active) return;
    active_ = active;
    if (active_ && supported() && viewer_) viewer_->requestRayTracingScene();
    emit activeChanged();
    update();
}

bool GpuRayTracingView::supported() const {
#if FOREVERTAS_GPU_RAY_TRACING
    return true;
#else
    return false;
#endif
}

QString GpuRayTracingView::status() const {
#if FOREVERTAS_GPU_RAY_TRACING
    return tr("Real-time GPU ray tracing: BVH ray traversal, "
              "ray-traced reflections and hard shadows.");
#else
    return tr("GPU ray tracing requires Qt 6.7 or newer and the "
              "Qt Shader Tools module.");
#endif
}

QVector3D GpuRayTracingView::cameraPosition() const {
    return cameraPosition_;
}

void GpuRayTracingView::setCameraPosition(const QVector3D &position) {
    if (NearlyEqual(cameraPosition_, position)) return;
    cameraPosition_ = position;
    emit cameraChanged();
    update();
}

QVector3D GpuRayTracingView::cameraTarget() const {
    return cameraTarget_;
}

void GpuRayTracingView::setCameraTarget(const QVector3D &target) {
    if (NearlyEqual(cameraTarget_, target)) return;
    cameraTarget_ = target;
    emit cameraChanged();
    update();
}

QVector3D GpuRayTracingView::cameraUp() const {
    return cameraUp_;
}

void GpuRayTracingView::setCameraUp(
        const QVector3D &direction) {
    if (NearlyEqual(cameraUp_, direction)) return;
    cameraUp_ = direction;
    emit cameraChanged();
    update();
}

float GpuRayTracingView::fieldOfView() const { return fieldOfView_; }

void GpuRayTracingView::setFieldOfView(float value) {
    if (qFuzzyCompare(fieldOfView_, value)) return;
    fieldOfView_ = value;
    emit cameraChanged();
    update();
}

RaceViewerController *GpuRayTracingView::viewerController() const {
    return viewer_;
}

#if FOREVERTAS_GPU_RAY_TRACING
QQuickRhiItemRenderer *GpuRayTracingView::createRenderer() {
    return new GpuRayTracingRenderer;
}
#endif

}  // namespace forevertas::viewer
