#ifndef FOREVERTAS_VIEWER_MAP_IDENTITY_H
#define FOREVERTAS_VIEWER_MAP_IDENTITY_H

#include <QString>

#include <forevervalidator/experimental/physics_sandbox.h>

namespace forevertas::viewer {

QString CollisionSceneKey(
        const forevervalidator::experimental::PhysicsSandboxSceneView &scene);

}  // namespace forevertas::viewer

#endif
