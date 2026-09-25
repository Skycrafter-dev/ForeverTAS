#include "viewer/map_identity.h"

#include <QCryptographicHash>
#include <QtEndian>

#include <cstdint>
#include <cstring>

namespace forevertas::viewer {

QString CollisionSceneKey(
        const forevervalidator::experimental::PhysicsSandboxSceneView &scene) {
    QCryptographicHash hash(QCryptographicHash::Sha256);
    QByteArray chunk;
    chunk.reserve(64 * 1024);
    const auto flush = [&hash, &chunk]() {
        if (!chunk.isEmpty()) {
            hash.addData(QByteArrayView(chunk));
            chunk.clear();
        }
    };
    const auto appendU32 = [&chunk, &flush](std::uint32_t value) {
        const quint32 bigEndian = qToBigEndian(static_cast<quint32>(value));
        chunk.append(
                reinterpret_cast<const char *>(&bigEndian),
                sizeof(bigEndian));
        if (chunk.size() >= 64 * 1024) flush();
    };
    const auto appendFloat = [&appendU32](float value) {
        std::uint32_t bits = 0;
        static_assert(sizeof(bits) == sizeof(value));
        std::memcpy(&bits, &value, sizeof(bits));
        appendU32(bits);
    };

    const std::uint64_t triangleCount = scene.collisionTriangles.size();
    appendU32(static_cast<std::uint32_t>(triangleCount >> 32u));
    appendU32(static_cast<std::uint32_t>(triangleCount));
    for (const auto &triangle : scene.collisionTriangles) {
        const auto appendVertex = [&appendFloat](
                const forevervalidator::Vector3 &vertex) {
            appendFloat(vertex.x);
            appendFloat(vertex.y);
            appendFloat(vertex.z);
        };
        appendVertex(triangle.a);
        appendVertex(triangle.b);
        appendVertex(triangle.c);
    }
    flush();
    return QStringLiteral("collision-sha256:")
            + QString::fromLatin1(hash.result().toHex());
}

}  // namespace forevertas::viewer
