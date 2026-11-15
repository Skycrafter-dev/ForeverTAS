// The vanishing car's cause: Qt Quick 3D (6.9) gives a geometry that comes
// back into a scene (no Model used it for a while) a new, empty renderer
// node and never sends it the data. A RaceGeometry no Model uses, then used
// again, must be drawn again. Needs a display (xvfb-run).
#include "viewer/race_geometry.h"

#include <QGuiApplication>
#include <QImage>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QTimer>

#include <cstring>
#include <iostream>

namespace {

void Wait(int milliseconds) {
    QEventLoop loop;
    QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
    loop.exec();
}

}  // namespace

int main(int argc, char **argv) {
    QGuiApplication application(argc, argv);
    forevertas::viewer::RaceGeometry triangle;
    QByteArray vertices(9 * sizeof(float), 0);
    const float points[] = {-1, -1, 0, 1, -1, 0, 0, 1, 0};
    std::memcpy(vertices.data(), points, sizeof(points));
    triangle.setMesh(vertices, 12, QQuick3DGeometry::PrimitiveType::Triangles, false, {-1, -1, 0}, {1, 1, 0});
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("triangle"), &triangle);
    engine.loadData(R"(
import QtQuick
import QtQuick3D
Window {
    width: 160; height: 160; visible: true; color: "black"
    property bool used: true
    View3D {
        anchors.fill: parent
        environment: SceneEnvironment { clearColor: "black"; backgroundMode: SceneEnvironment.Color }
        PerspectiveCamera { z: 400 }
        Model {
            scale: Qt.vector3d(100, 100, 100)
            geometry: used ? triangle : null
            materials: DefaultMaterial {
                lighting: DefaultMaterial.NoLighting; diffuseColor: "red"; cullMode: Material.NoCulling
            }
        }
    }
})");
    auto *window = engine.rootObjects().isEmpty() ? nullptr
                                                  : qobject_cast<QQuickWindow *>(engine.rootObjects().front());
    if (window == nullptr) return 1;
    const auto drawn = [window] {
        Wait(300);
        const QImage image = window->grabWindow();
        return image.pixelColor(image.width() / 2, image.height() / 2).red() > 200;
    };
    if (!drawn()) {
        std::cerr << "the triangle was not drawn at all (no 3D here?)\n";
        return 1;
    }
    for (int round = 1; round <= 3; ++round) {
        window->setProperty("used", false);
        if (drawn()) {
            std::cerr << "the triangle stayed without its model\n";
            return 1;
        }
        window->setProperty("used", true);
        if (!drawn()) {
            std::cerr << "used again (round " << round << "), the geometry was not drawn: the vanishing car\n";
            return 1;
        }
    }
    std::cout << "a geometry used again after no model used it is drawn again\n";
    return 0;
}
