#include "viewer/material_classifier.h"
#include "viewer/race_geometry.h"
#include "viewer/ray_tracing_scene.h"
#include "viewer/visual_scene_pipeline.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QSet>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

using forevertas::viewer::ClassifyMaterial;
using forevertas::viewer::MaterialSemanticContext;
using forevertas::viewer::ReplacementFor;
using forevertas::viewer::ReplacementMaterialClass;
using forevertas::viewer::StaticVisualBatch;
using forevervalidator::experimental::PhysicsSandboxRenderInstance;
using forevervalidator::experimental::PhysicsSandboxRenderLayer;
using forevervalidator::experimental::PhysicsSandboxRenderMaterial;
using forevervalidator::experimental::PhysicsSandboxRenderMesh;
using forevervalidator::experimental::PhysicsSandboxRenderScene;
using forevervalidator::experimental::PhysicsSandboxScenePurpose;

bool Check(bool condition, const char *message) {
    if (!condition) {
        std::cerr << message << '\n';
    }
    return condition;
}

PhysicsSandboxRenderMaterial Named(const char *name) {
    PhysicsSandboxRenderMaterial material;
    material.sourcePath = name;
    material.surfaceMaterialId = 0xffu;
    return material;
}

bool TestClassification() {
    struct Case {
        const char *name;
        ReplacementMaterialClass expected;
    };
    constexpr std::array<Case, 14> cases{{
            {"StadiumRoadAsphalt", ReplacementMaterialClass::Asphalt},
            {"ConcreteWall", ReplacementMaterialClass::Concrete},
            {"DirtGround", ReplacementMaterialClass::Dirt},
            {"GrassField", ReplacementMaterialClass::Grass},
            {"SteelRail", ReplacementMaterialClass::Metal},
            {"PaintedMetalRed", ReplacementMaterialClass::PaintedMetal},
            {"PlasticTrim", ReplacementMaterialClass::Plastic},
            {"RubberBarrier", ReplacementMaterialClass::Rubber},
            {"GlassWindow", ReplacementMaterialClass::Glass},
            {"SponsorSign", ReplacementMaterialClass::Signage},
            {"NeonEmissive", ReplacementMaterialClass::Emissive},
            {"WaterPool", ReplacementMaterialClass::Water},
            {"NeutralDefault", ReplacementMaterialClass::Neutral},
            {"UnclassifiedSurface", ReplacementMaterialClass::Unknown},
    }};
    bool okay = true;
    for (const Case &test : cases) {
        okay &= Check(
                ClassifyMaterial(Named(test.name)) == test.expected,
                test.name);
    }

    PhysicsSandboxRenderMaterial remapped =
            Named("Materials/Replacement/Grass");
    remapped.modelPath = "Models/Original/Concrete";
    okay &= Check(ClassifyMaterial(remapped) ==
                          ReplacementMaterialClass::Grass,
                  "final remap material did not take classification priority");
    PhysicsSandboxRenderMaterial water = Named("Generic");
    water.water = true;
    okay &= Check(ClassifyMaterial(water) ==
                          ReplacementMaterialClass::Water,
                  "water render metadata did not override generic paths");
    PhysicsSandboxRenderMaterial surfaceFallback =
            Named("UnclassifiedSurface");
    surfaceFallback.surfaceMaterialId = 2u;
    okay &= Check(ClassifyMaterial(surfaceFallback) ==
                          ReplacementMaterialClass::Grass,
                  "surface-material fallback did not classify grass");
    PhysicsSandboxRenderMaterial renderTarget =
            Named("UnclassifiedSurface");
    renderTarget.renderTarget = true;
    renderTarget.surfaceMaterialId = 4u;
    okay &= Check(ClassifyMaterial(renderTarget) ==
                          ReplacementMaterialClass::Neutral,
                  "render-target fallback was not deterministic");

    const auto contextual = [](std::uint8_t surface, const char *block,
                               const char *component = "") {
        PhysicsSandboxRenderMaterial material = Named("UnclassifiedSurface");
        material.surfaceMaterialId = surface;
        MaterialSemanticContext context;
        context.blockName = block;
        context.componentIdentity = component;
        return ClassifyMaterial(material, context);
    };
    okay &= Check(contextual(7u, "StadiumRoadMainTurbo") ==
                          ReplacementMaterialClass::Turbo,
                  "turbo surface did not receive its semantic override");
    okay &= Check(contextual(28u, "StadiumRoadMainCheckpoint") ==
                          ReplacementMaterialClass::Checkpoint,
                  "checkpoint panel did not receive its semantic override");
    okay &= Check(contextual(28u, "StadiumRoadMainFinishLine") ==
                          ReplacementMaterialClass::StartFinish,
                  "finish panel did not receive its semantic override");
    okay &= Check(contextual(16u, "StadiumRoadMain") ==
                          ReplacementMaterialClass::Asphalt,
                  "road provenance did not classify asphalt");
    okay &= Check(contextual(6u, "StadiumRoadDirtHigh") ==
                          ReplacementMaterialClass::Dirt,
                  "dirt-road provenance did not classify dirt");
    okay &= Check(contextual(2u, "StadiumGrass") ==
                          ReplacementMaterialClass::Grass,
                  "grass provenance did not classify grass");
    PhysicsSandboxRenderMaterial grassClip =
            Named("UnclassifiedSurface");
    grassClip.surfaceMaterialId = 2u;
    MaterialSemanticContext grassClipContext;
    grassClipContext.blockName = "StadiumGrassClip";
    grassClipContext.purpose = PhysicsSandboxScenePurpose::Clip;
    okay &= Check(ClassifyMaterial(grassClip, grassClipContext) ==
                          ReplacementMaterialClass::Grass,
                  "grass clip did not classify as ground grass");
    PhysicsSandboxRenderMaterial groundCover =
            Named("UnclassifiedSurface");
    groundCover.surfaceMaterialId = 0u;
    MaterialSemanticContext groundCoverContext;
    groundCoverContext.blockName = "StadiumRoadDirtHigh";
    groundCoverContext.grassGroundCover = true;
    okay &= Check(ClassifyMaterial(groundCover, groundCoverContext) ==
                          ReplacementMaterialClass::Grass,
                  "flat block ground cover did not classify as grass");
    okay &= Check(contextual(13u, "StadiumPool") ==
                          ReplacementMaterialClass::Water,
                  "pool provenance did not classify water");
    okay &= Check(contextual(28u, "", "Flags") ==
                          ReplacementMaterialClass::Signage,
                  "component identity did not classify signage");
    okay &= Check(contextual(22u, "StadiumRoadMainStartLine") ==
                          ReplacementMaterialClass::Emissive,
                  "start-line light did not classify as emissive");
    return okay;
}

bool TestReplacementParametersAndTextures() {
    bool okay = true;
    const auto emissive =
            ReplacementFor(ReplacementMaterialClass::Emissive);
    const auto grass =
            ReplacementFor(ReplacementMaterialClass::Grass);
    const auto asphalt =
            ReplacementFor(ReplacementMaterialClass::Asphalt);
    const auto dirt = ReplacementFor(ReplacementMaterialClass::Dirt);
    const auto concrete =
            ReplacementFor(ReplacementMaterialClass::Concrete);
    okay &= Check(emissive.emissiveStrength > 0.0f,
                  "emissive replacement has no emission");
    okay &= Check(!asphalt.applyVertexColors &&
                          !grass.applyVertexColors &&
                          !dirt.applyVertexColors &&
                          !concrete.applyVertexColors &&
                          std::fabs(asphalt.worldUvScale - 0.25f) < 0.001f &&
                          std::fabs(grass.worldUvScale - 0.25f) < 0.001f &&
                          std::fabs(dirt.worldUvScale - 0.25f) < 0.001f &&
                          std::fabs(concrete.worldUvScale - 0.25f) < 0.001f,
                  "driving surfaces do not use consistent world-space UVs");
    okay &= Check(
            !ReplacementFor(ReplacementMaterialClass::Neutral)
                     .applyVertexColors &&
                    ReplacementFor(ReplacementMaterialClass::Unknown)
                            .applyVertexColors,
            "replacement vertex-color policy differs from the ray tracer");

    constexpr std::array<const char *, 17> names{{
            "asphalt", "concrete", "dirt", "grass", "metal",
            "painted_metal", "plastic", "rubber", "glass", "signage",
            "emissive", "turbo", "checkpoint", "start_finish", "water",
            "neutral", "unknown"}};
    QSet<QByteArray> baseTextureHashes;
    const QString root = QStringLiteral(FOREVERTAS_SOURCE_DIR) +
            QStringLiteral("/assets/materials/");
    for (const char *name : names) {
        const QString basePath = root + QString::fromLatin1(name) +
                QStringLiteral("_base.png");
        const QImage baseImage(basePath);
        okay &= Check(!baseImage.isNull(),
                      "replacement base texture did not load");
        okay &= Check(baseImage.width() >= 512 &&
                              baseImage.height() >= 512,
                      "replacement texture is undersized");
        if (std::string(name) != "concrete") {
            QSet<QRgb> sampledBaseColors;
            for (int y = 0; y < baseImage.height(); y += 4) {
                for (int x = 0; x < baseImage.width(); x += 4) {
                    sampledBaseColors.insert(baseImage.pixel(x, y));
                }
            }
            okay &= Check(
                    sampledBaseColors.size() > 64,
                    "replacement base texture is a low-information placeholder");
        }
        QFile baseFile(basePath);
        okay &= Check(baseFile.open(QIODevice::ReadOnly),
                      "replacement texture bytes were not readable");
        if (baseFile.isOpen()) {
            baseTextureHashes.insert(QCryptographicHash::hash(
                    baseFile.readAll(), QCryptographicHash::Sha256));
        }
    }
    okay &= Check(baseTextureHashes.size() ==
                          static_cast<qsizetype>(names.size()),
                  "replacement base textures are not visibly distinct assets");
    const QDir materialDirectory(root);
    okay &= Check(
            materialDirectory.entryList(
                                     {QStringLiteral("*_normal.png")},
                                     QDir::Files)
                    .isEmpty(),
            "unused raster-only normal-map assets remain packaged");

    const QImage concreteBase(root +
                              QStringLiteral("concrete_base.png"));
    const auto isUniform = [](const QImage &image) {
        if (image.isNull()) {
            return false;
        }
        const QColor color = image.pixelColor(0, 0);
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                if (image.pixelColor(x, y) != color) {
                    return false;
                }
            }
        }
        return true;
    };
    okay &= Check(isUniform(concreteBase),
                  "concrete replacement is not flat gray");
    okay &= Check(concreteBase.pixelColor(0, 0) == QColor("#a4a69f"),
                  "concrete replacement does not use the requested flat values");

    const QImage grassBase(root + QStringLiteral("grass_base.png"));
    std::int64_t grassRed = 0;
    std::int64_t grassGreen = 0;
    std::int64_t grassBlue = 0;
    for (int y = 0; y < grassBase.height(); y += 4) {
        for (int x = 0; x < grassBase.width(); x += 4) {
            const QColor color = grassBase.pixelColor(x, y);
            grassRed += color.red();
            grassGreen += color.green();
            grassBlue += color.blue();
        }
    }
    okay &= Check(grassGreen * 4 > grassRed * 5 &&
                          grassGreen * 6 > grassBlue * 7,
                  "grass ground cover is not recognizably green");

    const QImage turboBase(root + QStringLiteral("turbo_base.png"));
    const auto countArrowPixels =
            [&turboBase](int left, int right, bool yellow) {
        int count = 0;
        for (int y = 0; y < turboBase.height(); ++y) {
            for (int x = left; x < right; ++x) {
                const QColor color = turboBase.pixelColor(x, y);
                const bool match =
                        yellow
                        ? color.red() > 130 && color.green() > 90 &&
                                  color.red() > color.blue() * 1.5 &&
                                  color.green() > color.blue() * 1.4
                        : color.blue() > 110 && color.green() > 100 &&
                                  color.blue() > color.red() * 1.3 &&
                                  color.green() > color.red() * 1.3;
                count += match ? 1 : 0;
            }
        }
        return count;
    };
    const int cyanTail = countArrowPixels(96, 160, false);
    const int cyanHead = countArrowPixels(256, 320, false);
    const int yellowTail = countArrowPixels(288, 352, true);
    const int yellowHead = countArrowPixels(448, 512, true);
    okay &= Check(cyanTail > cyanHead * 2 &&
                          yellowTail > yellowHead * 2,
                  "turbo chevrons do not point right");
    return okay;
}

bool TestClipPlanesAndPurposeFiltering() {
    using forevertas::viewer::CalculateCameraClipPlanes;
    using forevertas::viewer::IsDefaultVisualInstance;
    using forevertas::viewer::IsDefaultVisualPurpose;
    const auto closeCamera = CalculateCameraClipPlanes(
            {0.0f, 2.0f, 30.0f}, 30.0f, {-100.0f, -10.0f, -150.0f},
            {100.0f, 80.0f, 150.0f});
    const auto farCamera = CalculateCameraClipPlanes(
            {0.0f, 2.0f, 300.0f}, 300.0f, {-100.0f, -10.0f, -150.0f},
            {100.0f, 80.0f, 150.0f});
    const auto enclosingSky = CalculateCameraClipPlanes(
            {0.0f, 2.0f, 3.0f}, 3.0f, {-28000.0f, -15000.0f, -28000.0f},
            {29000.0f, 15000.0f, 29000.0f});
    bool okay =
            Check(closeCamera.nearPlane >= 0.1f &&
                          closeCamera.farPlane > closeCamera.nearPlane &&
                          closeCamera.farPlane < 5000.0f,
                  "camera clip planes retained a forced 5000-unit far plane");
    okay &= Check(farCamera.nearPlane > closeCamera.nearPlane &&
                          farCamera.farPlane > closeCamera.farPlane,
                  "camera clip planes did not respond to camera distance");
    okay &= Check(enclosingSky.farPlane > 5000.0f &&
                          enclosingSky.nearPlane < 1.0f &&
                          enclosingSky.farPlane / enclosingSky.nearPlane <=
                                  50001.0f,
                  "enclosing sky bounds made close camera use unusable planes");
    okay &= Check(
            IsDefaultVisualPurpose(PhysicsSandboxScenePurpose::PlacedBlock) &&
                    IsDefaultVisualPurpose(
                            PhysicsSandboxScenePurpose::Environment) &&
                    IsDefaultVisualPurpose(
                            PhysicsSandboxScenePurpose::Generated) &&
                    !IsDefaultVisualPurpose(PhysicsSandboxScenePurpose::Clip) &&
                    !IsDefaultVisualPurpose(
                            PhysicsSandboxScenePurpose::Helper) &&
                    !IsDefaultVisualPurpose(
                            PhysicsSandboxScenePurpose::CheckpointTrigger) &&
                    IsDefaultVisualInstance(
                            PhysicsSandboxScenePurpose::Clip,
                            "StadiumGrassClip") &&
                    !IsDefaultVisualInstance(
                            PhysicsSandboxScenePurpose::Clip,
                            "CollisionClip"),
            "default purpose filtering lost intentional grass clips");
    return okay;
}

PhysicsSandboxRenderMesh TriangleMesh() {
    PhysicsSandboxRenderMesh mesh;
    mesh.vertices.resize(3u);
    mesh.vertices[0].position = {0.0f, 0.0f, 0.0f};
    mesh.vertices[1].position = {1.0f, 0.0f, 0.0f};
    mesh.vertices[2].position = {0.0f, 0.0f, 1.0f};
    for (auto &vertex : mesh.vertices) {
        vertex.normal = {0.0f, 1.0f, 0.0f};
        vertex.tangent = {1.0f, 0.0f, 0.0f, 1.0f};
    }
    mesh.vertices[1].uv0 = {1.0f, 0.0f};
    mesh.vertices[2].uv0 = {0.0f, 1.0f};
    mesh.indices = {0u, 1u, 2u};
    mesh.subsets = {{0u, 3u, 0u}};
    mesh.boundsMin = {0.0f, 0.0f, 0.0f};
    mesh.boundsMax = {1.0f, 0.0f, 1.0f};
    mesh.hasNormals = true;
    mesh.hasTangents = true;
    mesh.hasUv0 = true;
    return mesh;
}

PhysicsSandboxRenderMesh GrassBladeMesh() {
    PhysicsSandboxRenderMesh mesh;
    mesh.vertices.resize(64u);
    mesh.indices.reserve(96u);
    constexpr float ZStep = 15.75f / 4.0f;
    for (std::uint32_t quad = 0u; quad < 16u; ++quad) {
        const float x = static_cast<float>(quad % 4u) * 8.0f;
        const float z = static_cast<float>(quad / 4u) * ZStep;
        const std::uint32_t base = quad * 4u;
        const std::array<forevervalidator::Vector3, 4> positions{{
                {x, 0.0f, z},
                {x + 8.0f, 0.0f, z},
                {x + 8.0f, 0.5f, z + ZStep},
                {x, 0.5f, z + ZStep}}};
        for (std::uint32_t corner = 0u; corner < 4u; ++corner) {
            auto &vertex = mesh.vertices[base + corner];
            vertex.position = positions[corner];
            vertex.normal = {0.0f, 1.0f, 0.0f};
            vertex.tangent = {1.0f, 0.0f, 0.0f, 1.0f};
        }
        mesh.indices.insert(mesh.indices.end(),
                            {base, base + 1u, base + 2u,
                             base, base + 2u, base + 3u});
    }
    mesh.subsets = {{0u, static_cast<std::uint32_t>(mesh.indices.size()), 0u}};
    mesh.boundsMin = {0.0f, 0.0f, 0.0f};
    mesh.boundsMax = {32.0f, 0.5f, 15.75f};
    mesh.hasNormals = true;
    mesh.hasTangents = true;
    mesh.hasUv0 = true;
    return mesh;
}

bool TestStaticBatching() {
    PhysicsSandboxRenderScene scene;
    scene.meshes.push_back(TriangleMesh());
    PhysicsSandboxRenderMesh groundCoverMesh = TriangleMesh();
    groundCoverMesh.vertices[1].position.x = 32.0f;
    groundCoverMesh.vertices[2].position.z = 32.0f;
    for (auto &vertex : groundCoverMesh.vertices) {
        vertex.normal = {0.312249f, 0.95f, 0.0f};
    }
    groundCoverMesh.boundsMax = {32.0f, 0.0f, 32.0f};
    PhysicsSandboxRenderMesh overlappingGroundCoverMesh = groundCoverMesh;
    scene.meshes.push_back(std::move(groundCoverMesh));
    scene.meshes.push_back(std::move(overlappingGroundCoverMesh));
    scene.meshes.push_back(GrassBladeMesh());
    PhysicsSandboxRenderMaterial turbo;
    turbo.surfaceMaterialId = 7u;
    scene.materials.push_back(turbo);
    PhysicsSandboxRenderMaterial concrete;
    concrete.surfaceMaterialId = 0u;
    scene.materials.push_back(concrete);
    PhysicsSandboxRenderMaterial dirt;
    dirt.surfaceMaterialId = 6u;
    scene.materials.push_back(dirt);

    PhysicsSandboxRenderInstance placed;
    placed.meshIndex = 0u;
    placed.materialIndex = 0u;
    placed.purpose = PhysicsSandboxScenePurpose::PlacedBlock;
    placed.provenance.blockName = "StadiumRoadMainTurbo";
    placed.worldTransform.basisX = {2.0f, 0.0f, 0.0f};
    placed.worldTransform.basisY = {0.0f, 3.0f, 0.0f};
    placed.worldTransform.basisZ = {0.0f, 0.0f, 4.0f};
    placed.worldTransform.translation = {10.0f, 20.0f, 30.0f};
    scene.instances.push_back(placed);
    scene.instances.push_back(placed);

    PhysicsSandboxRenderInstance clip = placed;
    clip.meshIndex = 1u;
    clip.materialIndex = 1u;
    clip.purpose = PhysicsSandboxScenePurpose::Clip;
    clip.provenance.blockName = "StadiumGrassClip";
    clip.worldTransform.basisX = {1.0f, 0.0f, 0.0f};
    clip.worldTransform.basisY = {0.0f, 1.0f, 0.0f};
    clip.worldTransform.basisZ = {0.0f, 0.0f, 1.0f};
    clip.worldTransform.translation = {-5.0f, 0.0f, 0.0f};
    scene.instances.push_back(clip);

    PhysicsSandboxRenderInstance overlappingClip = clip;
    overlappingClip.meshIndex = 2u;
    overlappingClip.worldTransform.translation = {11.0f, 0.0f, 0.0f};
    scene.instances.push_back(overlappingClip);

    PhysicsSandboxRenderInstance blades = clip;
    blades.meshIndex = 3u;
    blades.purpose = PhysicsSandboxScenePurpose::PlacedBlock;
    blades.provenance.blockName = "StadiumGrass";
    scene.instances.push_back(blades);

    PhysicsSandboxRenderInstance dirtGround = clip;
    dirtGround.materialIndex = 2u;
    dirtGround.purpose = PhysicsSandboxScenePurpose::PlacedBlock;
    dirtGround.provenance.blockName = "StadiumRoadDirtHigh";
    dirtGround.worldTransform.translation = {35.0f, 0.0f, 0.0f};
    scene.instances.push_back(dirtGround);

    PhysicsSandboxRenderInstance background = placed;
    background.renderLayer = PhysicsSandboxRenderLayer::Background;
    scene.instances.push_back(background);

    PhysicsSandboxRenderInstance hidden = placed;
    hidden.visible = false;
    scene.instances.push_back(hidden);

    PhysicsSandboxRenderInstance lowerLod = placed;
    lowerLod.lodLevel = 1u;
    lowerLod.worldTransform.translation = {70.0f, 20.0f, 30.0f};
    scene.instances.push_back(lowerLod);

    const auto result = forevertas::viewer::BuildStaticVisualBatches(scene);
    const auto repeat = forevertas::viewer::BuildStaticVisualBatches(scene);
    bool okay = Check(
            result.visibleSourceInstanceCount == 7u &&
                    result.defaultVisibleInstanceCount == 4u &&
                    result.defaultTriangleCount == 4u &&
                    result.duplicateInstanceCount == 1u &&
                    result.skippedBackgroundInstanceCount == 1u &&
                    result.skippedBackgroundTriangleCount == 1u &&
                    result.skippedGrassBladeInstanceCount == 1u &&
                    result.skippedGrassBladeTriangleCount == 32u &&
                    result.batches.size() == 3u,
            "static batch counts, blade removal, or duplicate suppression "
            "were incorrect, or a lower-detail LOD reached the preview");
    const auto turboBatch = std::find_if(
            result.batches.cbegin(), result.batches.cend(),
            [](const StaticVisualBatch &batch) {
                return batch.materialClass == ReplacementMaterialClass::Turbo;
            });
    okay &= Check(turboBatch != result.batches.cend(),
                  "turbo geometry did not reach a turbo batch");
    const auto grassClipBatch = std::find_if(
            result.batches.cbegin(), result.batches.cend(),
            [](const StaticVisualBatch &batch) {
                return batch.materialClass == ReplacementMaterialClass::Grass &&
                       batch.purpose == PhysicsSandboxScenePurpose::Clip &&
                       batch.defaultVisible;
            });
    okay &= Check(grassClipBatch != result.batches.cend(),
                  "intentional grass ground-cover clip did not reach the scene");
    if (grassClipBatch != result.batches.cend()) {
        constexpr std::size_t FloatCount = 17u;
        const auto *vertices = reinterpret_cast<const float *>(
                grassClipBatch->vertices.constData());
        const std::size_t vertexCount =
                static_cast<std::size_t>(grassClipBatch->vertices.size()) /
                (FloatCount * sizeof(float));
        bool uvInsideTile = true;
        bool hasUTangent = false;
        bool hasVTangent = false;
        bool hasFlatNormals = true;
        for (std::size_t vertexIndex = 0u; vertexIndex < vertexCount;
             ++vertexIndex) {
            const float *vertex = vertices + vertexIndex * FloatCount;
            uvInsideTile &= vertex[9] >= -0.001f &&
                            vertex[9] <= 1.001f &&
                            vertex[10] >= -0.001f &&
                            vertex[10] <= 1.001f;
            hasUTangent |= std::fabs(vertex[6]) > 0.9f;
            hasVTangent |= std::fabs(vertex[8]) > 0.9f;
            hasFlatNormals &= std::fabs(vertex[3]) < 0.001f &&
                    std::fabs(vertex[4] - 1.0f) < 0.001f &&
                    std::fabs(vertex[5]) < 0.001f;
        }
        const auto *indices = reinterpret_cast<const std::uint32_t *>(
                grassClipBatch->indices.constData());
        const std::size_t indexCount =
                static_cast<std::size_t>(grassClipBatch->indices.size()) /
                sizeof(std::uint32_t);
        float projectedArea = 0.0f;
        bool hasDegenerateTriangle = false;
        for (std::size_t index = 0u; index + 2u < indexCount; index += 3u) {
            const float *a = vertices + indices[index] * FloatCount;
            const float *b = vertices + indices[index + 1u] * FloatCount;
            const float *c = vertices + indices[index + 2u] * FloatCount;
            const float area =
                    std::fabs((b[0] - a[0]) * (c[2] - a[2]) -
                              (b[2] - a[2]) * (c[0] - a[0])) *
                    0.5f;
            projectedArea += area;
            hasDegenerateTriangle |= area < 0.0001f;
        }
        const auto repeatedGrassClipBatch = std::find_if(
                repeat.batches.cbegin(), repeat.batches.cend(),
                [](const StaticVisualBatch &batch) {
                    return batch.materialClass ==
                                   ReplacementMaterialClass::Grass &&
                           batch.purpose ==
                                   PhysicsSandboxScenePurpose::Clip &&
                           batch.defaultVisible;
                });
        okay &= Check(
                grassClipBatch->triangleCount > 1u &&
                        vertexCount > 3u && uvInsideTile && hasUTangent &&
                        hasVTangent && hasFlatNormals &&
                        !hasDegenerateTriangle &&
                        std::fabs(projectedArea - 896.0f) < 0.01f &&
                        repeatedGrassClipBatch != repeat.batches.cend() &&
                        grassClipBatch->vertices ==
                                repeatedGrassClipBatch->vertices &&
                        grassClipBatch->indices ==
                                repeatedGrassClipBatch->indices,
                "grass ground did not receive stable, overlap-free randomized "
                "four-meter tiles with geometric flat normals");
    }
    const auto dirtBatch = std::find_if(
            result.batches.cbegin(), result.batches.cend(),
            [](const StaticVisualBatch &batch) {
                return batch.materialClass == ReplacementMaterialClass::Dirt &&
                       batch.purpose ==
                               PhysicsSandboxScenePurpose::PlacedBlock &&
                       batch.defaultVisible;
            });
    const auto repeatedDirtBatch = std::find_if(
            repeat.batches.cbegin(), repeat.batches.cend(),
            [](const StaticVisualBatch &batch) {
                return batch.materialClass == ReplacementMaterialClass::Dirt &&
                       batch.purpose ==
                               PhysicsSandboxScenePurpose::PlacedBlock &&
                       batch.defaultVisible;
            });
    okay &= Check(
            dirtBatch != result.batches.cend() &&
                    dirtBatch->triangleCount > 1u &&
                    dirtBatch->vertices.size() >
                            static_cast<qsizetype>(3u * 17u * sizeof(float)) &&
                    repeatedDirtBatch != repeat.batches.cend() &&
                    dirtBatch->vertices == repeatedDirtBatch->vertices &&
                    dirtBatch->indices == repeatedDirtBatch->indices,
            "dirt ground did not receive stable randomized four-meter tiles");
    if (turboBatch != result.batches.cend()) {
        constexpr std::size_t FloatCount = 17u;
        const auto *vertices = reinterpret_cast<const float *>(
                turboBatch->vertices.constData());
        okay &= Check(
                turboBatch->sourceInstanceCount == 1u &&
                        turboBatch->triangleCount == 1u &&
                        turboBatch->indices.size() ==
                                static_cast<qsizetype>(3u *
                                                       sizeof(std::uint32_t)) &&
                        std::fabs(vertices[0] - 10.0f) < 0.001f &&
                        std::fabs(vertices[1] - 20.0f) < 0.001f &&
                        std::fabs(vertices[2] - 30.0f) < 0.001f &&
                        std::fabs(vertices[FloatCount] - 12.0f) < 0.001f &&
                        std::fabs(vertices[3] - 0.0f) < 0.001f &&
                        std::fabs(vertices[4] - 1.0f) < 0.001f &&
                        std::fabs(vertices[5] - 0.0f) < 0.001f &&
                        std::fabs(vertices[9] - 0.0f) < 0.001f &&
                        std::fabs(vertices[FloatCount + 9] - 1.0f) < 0.001f,
                "static batching did not preserve transforms, normals, or UVs");
    }
    const auto rayTracingScene =
            forevertas::viewer::BuildRayTracingScene(result.batches);
    const std::atomic_bool cancelled(true);
    okay &= Check(!forevertas::viewer::BuildRayTracingScene(result.batches, &cancelled),
                  "cancelled ray tracing build must not publish a partial scene");
    std::uint32_t expectedRayTracingTriangles = 0u;
    for (const StaticVisualBatch &batch : result.batches) {
        if (batch.defaultVisible) {
            expectedRayTracingTriangles +=
                    static_cast<std::uint32_t>(
                            batch.indices.size() /
                            static_cast<qsizetype>(
                                    3u * sizeof(std::uint32_t)));
        }
    }
    okay &= Check(
            rayTracingScene != nullptr &&
                    rayTracingScene->triangleCount ==
                            expectedRayTracingTriangles &&
                    rayTracingScene->triangleCount >=
                            result.defaultTriangleCount &&
                    rayTracingScene->vertexCount > 0u &&
                    rayTracingScene->bvhNodeCount > 0u &&
                    rayTracingScene->bvhNodeCount <
                            rayTracingScene->triangleCount * 2u &&
                    rayTracingScene->materialCount == 17u &&
                    rayTracingScene->triangles.size() ==
                            static_cast<qsizetype>(
                                    rayTracingScene->triangleCount * 16u) &&
                    rayTracingScene->bvhNodes.size() ==
                            static_cast<qsizetype>(
                                    rayTracingScene->bvhNodeCount * 48u),
            "ray tracing scene did not preserve the visible geometry or "
            "produce a compact GPU BVH");
    if (rayTracingScene != nullptr) {
        okay &= Check(
                rayTracingScene->materials.size() ==
                        static_cast<qsizetype>(17u * 4u * sizeof(float)),
                "ray tracing material table did not use the unified surface "
                "layout");
        if (rayTracingScene->materials.size() ==
            static_cast<qsizetype>(17u * 4u * sizeof(float))) {
            std::array<float, 17u * 4u> parameters{};
            std::memcpy(parameters.data(),
                        rayTracingScene->materials.constData(),
                        rayTracingScene->materials.size());
            for (std::uint32_t index = 0u; index < 17u; ++index) {
                const auto replacement = ReplacementFor(
                        static_cast<ReplacementMaterialClass>(index));
                const float *const material =
                        parameters.data() + index * 4u;
                okay &= Check(
                        std::fabs(material[0] - replacement.roughness) <
                                        0.0001f &&
                                std::fabs(material[1] -
                                          replacement.metalness) <
                                        0.0001f &&
                                std::fabs(material[2] -
                                          replacement.emissiveStrength) <
                                        0.0001f &&
                                (material[3] > 0.5f) ==
                                        replacement.applyVertexColors,
                        "ray tracing material table diverged from the shared "
                        "replacement contract");
            }
        }
    }
    return okay;
}

bool TestLargeEnvironmentGrassDoesNotTessellate() {
    constexpr float MinimumX = -20739.1f;
    constexpr float MaximumX = 23353.9f;
    constexpr float MinimumZ = -20733.5f;
    constexpr float MaximumZ = 24526.8f;

    PhysicsSandboxRenderMesh mesh = TriangleMesh();
    mesh.vertices[0].position = {MinimumX, 0.0f, MinimumZ};
    mesh.vertices[1].position = {MaximumX, 0.0f, MinimumZ};
    mesh.vertices[2].position = {MinimumX, 0.0f, MaximumZ};
    mesh.boundsMin = {MinimumX, 0.0f, MinimumZ};
    mesh.boundsMax = {MaximumX, 0.0f, MaximumZ};

    PhysicsSandboxRenderMaterial grass;
    grass.surfaceMaterialId = 2u;

    PhysicsSandboxRenderInstance environment;
    environment.meshIndex = 0u;
    environment.materialIndex = 0u;
    environment.purpose = PhysicsSandboxScenePurpose::Environment;
    environment.provenance.descriptorPath =
            "Bay\\Media\\Solid\\B1C002CAC3AD9D4D908D52046B47ACECE6";

    PhysicsSandboxRenderScene scene;
    scene.meshes.push_back(std::move(mesh));
    scene.materials.push_back(grass);
    scene.instances.push_back(environment);

    const auto result = forevertas::viewer::BuildStaticVisualBatches(scene);
    const auto repeat = forevertas::viewer::BuildStaticVisualBatches(scene);
    const auto batch = std::find_if(
            result.batches.cbegin(), result.batches.cend(),
            [](const StaticVisualBatch &entry) {
                return entry.materialClass == ReplacementMaterialClass::Grass &&
                       entry.purpose ==
                               PhysicsSandboxScenePurpose::Environment;
            });
    const auto repeated = std::find_if(
            repeat.batches.cbegin(), repeat.batches.cend(),
            [](const StaticVisualBatch &entry) {
                return entry.materialClass == ReplacementMaterialClass::Grass &&
                       entry.purpose ==
                               PhysicsSandboxScenePurpose::Environment;
            });
    if (batch == result.batches.cend()) {
        return Check(false,
                     "large grass-classified environment geometry was lost");
    }

    constexpr qsizetype VertexStride =
            static_cast<qsizetype>(17u * sizeof(float));
    const auto *vertices = reinterpret_cast<const float *>(
            batch->vertices.constData());
    const bool worldSpaceUvRepeats =
            batch->vertices.size() >= 3 * VertexStride &&
            (std::fabs(vertices[17u + 9u]) > 1.0f ||
             std::fabs(vertices[17u + 10u]) > 1.0f ||
             std::fabs(vertices[34u + 9u]) > 1.0f ||
             std::fabs(vertices[34u + 10u]) > 1.0f);
    return Check(
            result.defaultVisibleInstanceCount == 1u &&
                    result.defaultTriangleCount == 1u &&
                    result.batches.size() == 1u &&
                    batch->sourceInstanceCount == 1u &&
                    batch->triangleCount == 1u &&
                    batch->vertices.size() == 3 * VertexStride &&
                    batch->indices.size() ==
                            static_cast<qsizetype>(3u * sizeof(std::uint32_t)) &&
                    worldSpaceUvRepeats &&
                    repeated != repeat.batches.cend() &&
                    batch->vertices == repeated->vertices &&
                    batch->indices == repeated->indices,
            "large environment scenery was subdivided into texture-tile "
            "geometry instead of retaining one repeating-UV triangle");
}

bool TestIndexedGeometry() {
    constexpr int FloatCount = 17;
    std::array<float, FloatCount * 3> vertices{};
    vertices[0] = 0.0f;
    vertices[1] = 0.0f;
    vertices[2] = 0.0f;
    vertices[FloatCount] = 1.0f;
    vertices[FloatCount + 1] = 0.0f;
    vertices[FloatCount + 2] = 0.0f;
    vertices[FloatCount * 2] = 0.0f;
    vertices[FloatCount * 2 + 1] = 1.0f;
    vertices[FloatCount * 2 + 2] = 0.0f;
    const std::array<std::uint32_t, 3> indices{{0u, 1u, 2u}};

    forevertas::viewer::RaceGeometry geometry;
    geometry.setIndexedMesh(
            QByteArray(reinterpret_cast<const char *>(vertices.data()),
                       static_cast<qsizetype>(sizeof(vertices))),
            QByteArray(reinterpret_cast<const char *>(indices.data()),
                       static_cast<qsizetype>(sizeof(indices))),
            FloatCount * static_cast<int>(sizeof(float)),
            true,
            true,
            true,
            true,
            true,
            {0.0f, 0.0f, 0.0f},
            {1.0f, 1.0f, 0.0f},
            {{0, 3}});
    bool okay = Check(geometry.indexData().size() ==
                              static_cast<qsizetype>(sizeof(indices)),
                      "indexed geometry did not retain its index buffer");
    okay &= Check(geometry.attributeCount() == 7,
                  "indexed geometry did not expose every vertex attribute");
    constexpr std::array<QQuick3DGeometry::Attribute::Semantic, 7> semantics{{
            QQuick3DGeometry::Attribute::IndexSemantic,
            QQuick3DGeometry::Attribute::PositionSemantic,
            QQuick3DGeometry::Attribute::NormalSemantic,
            QQuick3DGeometry::Attribute::TangentSemantic,
            QQuick3DGeometry::Attribute::TexCoord0Semantic,
            QQuick3DGeometry::Attribute::TexCoord1Semantic,
            QQuick3DGeometry::Attribute::ColorSemantic}};
    constexpr std::array<int, 7> offsets{{
            0,
            0,
            3 * static_cast<int>(sizeof(float)),
            6 * static_cast<int>(sizeof(float)),
            9 * static_cast<int>(sizeof(float)),
            11 * static_cast<int>(sizeof(float)),
            13 * static_cast<int>(sizeof(float))}};
    constexpr std::array<QQuick3DGeometry::Attribute::ComponentType, 7>
            componentTypes{{
                    QQuick3DGeometry::Attribute::U32Type,
                    QQuick3DGeometry::Attribute::F32Type,
                    QQuick3DGeometry::Attribute::F32Type,
                    QQuick3DGeometry::Attribute::F32Type,
                    QQuick3DGeometry::Attribute::F32Type,
                    QQuick3DGeometry::Attribute::F32Type,
                    QQuick3DGeometry::Attribute::F32Type}};
    okay &= Check(geometry.stride() ==
                          FloatCount * static_cast<int>(sizeof(float)),
                  "indexed geometry stride was incorrect");
    for (int index = 0; index < geometry.attributeCount(); ++index) {
        const QQuick3DGeometry::Attribute attribute =
                geometry.attribute(index);
        okay &= Check(
                attribute.semantic == semantics[static_cast<std::size_t>(index)] &&
                        attribute.offset ==
                                offsets[static_cast<std::size_t>(index)] &&
                        attribute.componentType ==
                                componentTypes[static_cast<std::size_t>(index)],
                "indexed geometry attribute layout was incorrect");
    }
    okay &= Check(geometry.subsetCount() == 1 &&
                          geometry.subsetOffset(0) == 0 &&
                          geometry.subsetCount(0) == 3,
                  "indexed geometry subset was incorrect");
    geometry.clearMesh();
    okay &= Check(geometry.vertexData().isEmpty() &&
                          geometry.indexData().isEmpty(),
                  "geometry cleanup retained stale buffers");
    return okay;
}

bool TestRayTracingShaders() {
    QFile presentVertex(QStringLiteral(
            FOREVERTAS_SOURCE_DIR
            "/shaders/raytrace_present.vert"));
    QFile rayTracingCompute(QStringLiteral(
            FOREVERTAS_SOURCE_DIR "/shaders/raytrace.comp"));
    bool okay = Check(
            presentVertex.open(QIODevice::ReadOnly) &&
                    rayTracingCompute.open(QIODevice::ReadOnly),
            "ray tracing shader sources were not available");
    if (!okay) return false;

    const QByteArray presentSource = presentVertex.readAll();
    const QByteArray rayTracingSource = rayTracingCompute.readAll();
    okay &= Check(
            presentSource.contains("textureCoordinate = position;") &&
                    !presentSource.contains(
                            "textureCoordinate = position * 0.5"),
            "fullscreen ray tracing presentation did not preserve the "
            "complete viewport");
    okay &= Check(
            rayTracingSource.contains("realTimeRayTrace") &&
                    rayTracingSource.contains("sceneOccluded") &&
                    rayTracingSource.contains("reflectionHit") &&
                    !rayTracingSource.contains("pathTrace("),
            "ray tracing shader did not use the real-time game lighting "
            "path");
    okay &= Check(
            rayTracingSource.contains(
                    "vec2 screen = pixelPosition / "
                    "camera.viewportFrame.xy * 2.0 - 1.0;") &&
                    !rayTracingSource.contains("screen.y = -screen.y"),
            "ray tracing camera inverted the viewport Y coordinate");
    okay &= Check(
            rayTracingSource.contains(
                    "bool usesVertexColor = parameters.w > 0.5;") &&
                    rayTracingSource.contains(
                            "max(parameters.z, 0.0) * 2.0") &&
                    !rayTracingSource.contains("materialIndex > 3u") &&
                    !rayTracingSource.contains("materialIndex != 15u"),
            "ray tracing shader bypassed the shared material contract");
    return okay;
}

}  // namespace

int main(int argc, char **argv) {
    QGuiApplication application(argc, argv);
    bool okay = TestClassification();
    okay &= TestReplacementParametersAndTextures();
    okay &= TestClipPlanesAndPurposeFiltering();
    okay &= TestStaticBatching();
    okay &= TestLargeEnvironmentGrassDoesNotTessellate();
    okay &= TestIndexedGeometry();
    okay &= TestRayTracingShaders();
    return okay ? 0 : 1;
}
