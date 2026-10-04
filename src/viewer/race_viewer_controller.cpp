#include "viewer/race_viewer_controller.h"

#include "app/search_completion.h"
#include "conditions/condition_program.h"
#include "mutations/input_event_formatter.h"
#include "mutations/input_event_utils.h"
#include "replay_file_io.h"
#include "time_format.h"
#include "viewer/material_classifier.h"
#include "viewer/map_identity.h"

#include <forevervalidator/camera.h>
#include <forevervalidator/experimental/physics_sandbox.h>
#include <forevervalidator/native.h>

#include <QCryptographicHash>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QMetaObject>
#include <QMatrix4x4>
#include <QJsonDocument>
#include <QQmlEngine>
#include <QColor>
#include <QRegularExpression>
#include <QSettings>
#include <QThread>
#include <QVariantMap>
#include <QtEndian>
#include <QtConcurrentRun>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <utility>

namespace forevertas::viewer {

namespace {

constexpr auto kTelemetryScriptKey = "viewer/telemetryScript";
constexpr auto kVisualStylesKey = "viewer/visualStyles";
constexpr double kUnavailableTelemetryNumber =
        std::numeric_limits<double>::quiet_NaN();

const QString &DefaultTelemetryScript() {
    static const QString script = QStringLiteral(
            "Camera pos: X {camera.x:2}   Y {camera.y:2}   Z {camera.z:2}\n"
            "{target.readout}\n{conditions.acceptance}");
    return script;
}

const QString &LegacyTelemetryScript() {
    static const QString script = QStringLiteral(
            "Camera pos: X {camera.x:2}   Y {camera.y:2}   Z {camera.z:2}");
    return script;
}

RaceViewerFrame SampleTelemetryFrame(
        const std::vector<RaceViewerFrame> &frames,
        qint64 timeMs) {
    if (frames.empty()) {
        return {};
    }
    const auto upper = std::lower_bound(
            frames.begin(), frames.end(), timeMs,
            [](const RaceViewerFrame &frame, qint64 time) {
                return frame.timeMs < time;
            });
    if (upper == frames.begin()) {
        return *upper;
    }
    if (upper == frames.end()) {
        return frames.back();
    }
    if (upper->timeMs == timeMs) {
        return *upper;
    }
    const RaceViewerFrame &after = *upper;
    const RaceViewerFrame &before = *(upper - 1);
    const qint64 interval = after.timeMs - before.timeMs;
    const float blend = interval > 0
            ? static_cast<float>(timeMs - before.timeMs) /
                    static_cast<float>(interval)
            : 0.0f;
    RaceViewerFrame result = before;
    result.timeMs = timeMs;
    result.position = before.position * (1.0f - blend) +
            after.position * blend;
    result.linearSpeed = before.linearSpeed * (1.0f - blend) +
            after.linearSpeed * blend;
    result.signedSpeed = before.signedSpeed * (1.0f - blend) +
            after.signedSpeed * blend;
    result.accelerate = before.accelerate * (1.0f - blend) +
            after.accelerate * blend;
    result.brake = before.brake * (1.0f - blend) + after.brake * blend;
    result.steering = before.steering * (1.0f - blend) +
            after.steering * blend;
    return result;
}

struct TelemetryValue {
    TelemetryValue(QVariant value, bool numeric, bool available = true,
                   QColor color = {})
        : value(std::move(value)), numeric(numeric), available(available),
          color(std::move(color)) {}

    QVariant value;
    bool numeric = false;
    bool available = true;
    QColor color;
};

std::optional<TelemetryValue> ResolveTelemetryValue(
        const QString &name,
        const QVector3D &cameraPosition,
        const RaceViewerFrame &frame,
        const QString &runName,
        qint64 tick,
        const QVariantMap &context) {
    const auto number = [](double value) {
        return TelemetryValue{value, true};
    };
    if (name == QStringLiteral("camera.x"))
        return number(cameraPosition.x());
    if (name == QStringLiteral("camera.y"))
        return number(cameraPosition.y());
    if (name == QStringLiteral("camera.z"))
        return number(cameraPosition.z());
    if (name == QStringLiteral("car.x"))
        return number(frame.position.x());
    if (name == QStringLiteral("car.y"))
        return number(frame.position.y());
    if (name == QStringLiteral("car.z"))
        return number(frame.position.z());
    if (name == QStringLiteral("car.velocity.x"))
        return number(frame.linearSpeed.x());
    if (name == QStringLiteral("car.velocity.y"))
        return number(frame.linearSpeed.y());
    if (name == QStringLiteral("car.velocity.z"))
        return number(frame.linearSpeed.z());
    if (name == QStringLiteral("car.speed"))
        return number(frame.linearSpeed.length());
    if (name == QStringLiteral("car.speedKph"))
        return number(frame.linearSpeed.length() * 3.6);
    if (name == QStringLiteral("input.accelerate"))
        return number(frame.accelerate);
    if (name == QStringLiteral("input.brake"))
        return number(frame.brake);
    if (name == QStringLiteral("input.steering"))
        return number(frame.steering);
    if (name == QStringLiteral("race.checkpoints"))
        return number(frame.checkpointsCollected);
    if (name == QStringLiteral("race.totalCheckpoints"))
        return number(frame.checkpointsTotal);
    if (name == QStringLiteral("race.laps"))
        return number(frame.completedLaps);
    if (name == QStringLiteral("race.totalLaps"))
        return number(frame.totalLaps);
    if (name == QStringLiteral("race.finished")) {
        return TelemetryValue{
                frame.raceCompleted ? QStringLiteral("true")
                                    : QStringLiteral("false"),
                false};
    }
    if (name == QStringLiteral("time.ms"))
        return number(frame.timeMs);
    if (name == QStringLiteral("time.s"))
        return number(static_cast<double>(frame.timeMs) / 1000.0);
    if (name == QStringLiteral("tick"))
        return number(tick);
    if (name == QStringLiteral("run.name"))
        return TelemetryValue{runName, false};
    if (name == QStringLiteral("stunt.score")) {
        return frame.stuntsScore
                ? number(*frame.stuntsScore)
                : TelemetryValue{QVariant{}, true, false};
    }
    if (name == QStringLiteral("stunt.deadline") ||
        name == QStringLiteral("target.score")) {
        const QVariant value = context.value(name);
        return value.isValid()
                ? number(value.toDouble())
                : TelemetryValue{QVariant{}, true, false};
    }
    if (name == QStringLiteral("target.name") ||
        name == QStringLiteral("target.summary") ||
        name == QStringLiteral("target.now") ||
        name == QStringLiteral("target.readout")) {
        return TelemetryValue{context.value(name).toString(), false};
    }
    if (name == QStringLiteral("conditions.acceptance")) {
        const QVariantMap condition = context.value(name).toMap();
        return TelemetryValue{
                condition.value(QStringLiteral("text")).toString(), false,
                true, condition.value(QStringLiteral("color")).value<QColor>()};
    }
    return std::nullopt;
}

QString FormatTelemetryNumber(double value, int precision) {
    if (!std::isfinite(value)) {
        return QString::number(value);
    }
    const double threshold = 0.5 * std::pow(10.0, -precision);
    if (std::abs(value) < threshold) {
        value = 0.0;
    }
    return QString::number(value, 'f', precision);
}

class TelemetryExpression {
public:
    TelemetryExpression(const QString &expression,
                        const QVector3D &cameraPosition,
                        const RaceViewerFrame &frame,
                        const QString &runName,
                        qint64 tick,
                        const QVariantMap &context)
        : input_(expression), cameraPosition_(cameraPosition), frame_(frame),
          runName_(runName), tick_(tick), context_(context) {}

    double evaluate() {
        if (input_.size() > 256) {
            error_ = QStringLiteral("Math expression is too long");
            return kUnavailableTelemetryNumber;
        }
        const double value = expression();
        spaces();
        if (error_.isEmpty() && position_ != input_.size())
            error_ = QStringLiteral("Unexpected character in math expression");
        return value;
    }

    QString error() const { return error_; }

private:
    void spaces() {
        while (position_ < input_.size() && input_.at(position_).isSpace())
            ++position_;
    }

    bool take(QChar character) {
        spaces();
        if (position_ < input_.size() && input_.at(position_) == character) {
            ++position_;
            return true;
        }
        return false;
    }

    double expression() {
        double value = term();
        while (error_.isEmpty()) {
            if (take(QLatin1Char('+')))
                value += term();
            else if (take(QLatin1Char('-')))
                value -= term();
            else
                break;
        }
        return value;
    }

    double term() {
        double value = unary();
        while (error_.isEmpty()) {
            if (take(QLatin1Char('*')))
                value *= unary();
            else if (take(QLatin1Char('/'))) {
                const double divisor = unary();
                value = divisor == 0.0
                        ? kUnavailableTelemetryNumber : value / divisor;
            } else
                break;
        }
        return value;
    }

    double unary() {
        if (take(QLatin1Char('+')))
            return unary();
        if (take(QLatin1Char('-')))
            return -unary();
        return primary();
    }

    double primary() {
        spaces();
        if (++depth_ > 32) {
            error_ = QStringLiteral("Math expression is nested too deeply");
            return kUnavailableTelemetryNumber;
        }
        double value = kUnavailableTelemetryNumber;
        if (take(QLatin1Char('('))) {
            value = expression();
            if (!take(QLatin1Char(')')) && error_.isEmpty())
                error_ = QStringLiteral("Missing ')' in math expression");
        } else if (position_ < input_.size() &&
                   (input_.at(position_).isDigit() ||
                    input_.at(position_) == QLatin1Char('.'))) {
            const qsizetype start = position_;
            bool digit = false;
            while (position_ < input_.size() && input_.at(position_).isDigit()) {
                digit = true;
                ++position_;
            }
            if (position_ < input_.size() &&
                input_.at(position_) == QLatin1Char('.')) {
                ++position_;
                while (position_ < input_.size() && input_.at(position_).isDigit()) {
                    digit = true;
                    ++position_;
                }
            }
            if (!digit) {
                error_ = QStringLiteral("Invalid number in math expression");
            } else {
                if (position_ < input_.size() &&
                    (input_.at(position_) == QLatin1Char('e') ||
                     input_.at(position_) == QLatin1Char('E'))) {
                    ++position_;
                    if (position_ < input_.size() &&
                        (input_.at(position_) == QLatin1Char('+') ||
                         input_.at(position_) == QLatin1Char('-')))
                        ++position_;
                    const qsizetype exponentStart = position_;
                    while (position_ < input_.size() && input_.at(position_).isDigit())
                        ++position_;
                    if (position_ == exponentStart)
                        error_ = QStringLiteral("Invalid exponent in math expression");
                }
                if (error_.isEmpty()) {
                    bool okay = false;
                    value = input_.mid(start, position_ - start).toDouble(&okay);
                    if (!okay || !std::isfinite(value))
                        error_ = QStringLiteral("Invalid number in math expression");
                }
            }
        } else if (position_ < input_.size() && input_.at(position_).isLetter()) {
            const qsizetype start = position_++;
            while (position_ < input_.size() &&
                   (input_.at(position_).isLetterOrNumber() ||
                    input_.at(position_) == QLatin1Char('.')))
                ++position_;
            const QString name = input_.mid(start, position_ - start);
            if (take(QLatin1Char('('))) {
                const double first = expression();
                const bool hasSecond = take(QLatin1Char(','));
                const double second = hasSecond
                        ? expression() : kUnavailableTelemetryNumber;
                if (!take(QLatin1Char(')')) && error_.isEmpty())
                    error_ = QStringLiteral("Missing ')' in math function");
                if (name == QStringLiteral("abs") && !hasSecond)
                    value = std::abs(first);
                else if (name == QStringLiteral("sqrt") && !hasSecond)
                    value = std::sqrt(first);
                else if (name == QStringLiteral("round") && !hasSecond)
                    value = std::round(first);
                else if (name == QStringLiteral("min") && hasSecond)
                    value = std::isfinite(first) && std::isfinite(second)
                            ? std::min(first, second)
                            : kUnavailableTelemetryNumber;
                else if (name == QStringLiteral("max") && hasSecond)
                    value = std::isfinite(first) && std::isfinite(second)
                            ? std::max(first, second)
                            : kUnavailableTelemetryNumber;
                else if (error_.isEmpty())
                    error_ = QStringLiteral("Unknown math function %1").arg(name);
            } else {
                const auto resolved = ResolveTelemetryValue(
                        name, cameraPosition_, frame_, runName_, tick_, context_);
                if (!resolved)
                    error_ = QStringLiteral("Unknown telemetry field {%1}").arg(name);
                else if (!resolved->numeric)
                    error_ = QStringLiteral("Telemetry field {%1} is not numeric").arg(name);
                else if (resolved->available)
                    value = resolved->value.toDouble();
            }
        } else if (error_.isEmpty()) {
            error_ = QStringLiteral("Expected a number or field in math expression");
        }
        --depth_;
        return value;
    }

    QString input_;
    QVector3D cameraPosition_;
    const RaceViewerFrame &frame_;
    const QString &runName_;
    qint64 tick_;
    const QVariantMap &context_;
    qsizetype position_ = 0;
    int depth_ = 0;
    QString error_;
};

struct TelemetryRenderResult {
    QString text;
    QString error;
};

TelemetryRenderResult RenderTelemetryTemplate(
        const QString &script,
        const QVector3D &cameraPosition,
        const RaceViewerFrame &frame,
        const QString &runName,
        qint64 tick,
        const QVariantMap &context = {},
        bool richText = false) {
    TelemetryRenderResult result;
    static const QRegularExpression fieldExpression(
            QStringLiteral("^[A-Za-z][A-Za-z0-9.]*$"));
    for (qsizetype index = 0; index < script.size();) {
        if (script.at(index) == QLatin1Char('{') &&
            index + 1 < script.size() &&
            script.at(index + 1) == QLatin1Char('{')) {
            result.text += QLatin1Char('{');
            index += 2;
            continue;
        }
        if (script.at(index) == QLatin1Char('}') &&
            index + 1 < script.size() &&
            script.at(index + 1) == QLatin1Char('}')) {
            result.text += QLatin1Char('}');
            index += 2;
            continue;
        }
        if (script.at(index) != QLatin1Char('{')) {
            if (script.at(index) == QLatin1Char('}')) {
                result.error = QStringLiteral(
                        "Unexpected '}' at character %1").arg(index + 1);
                return result;
            }
            const QChar character = script.at(index++);
            result.text += richText && character == QLatin1Char('&')
                    ? QStringLiteral("&amp;")
                    : richText && character == QLatin1Char('<')
                      ? QStringLiteral("&lt;")
                      : richText && character == QLatin1Char('>')
                        ? QStringLiteral("&gt;") : QString(character);
            continue;
        }
        const qsizetype close = script.indexOf(
                QLatin1Char('}'), index + 1);
        if (close < 0) {
            result.error = QStringLiteral(
                    "Unclosed '{' at character %1").arg(index + 1);
            return result;
        }
        const QString token = script.mid(index + 1, close - index - 1);
        const qsizetype colon = token.lastIndexOf(QLatin1Char(':'));
        const QString source = (colon < 0 ? token : token.left(colon)).trimmed();
        const QString precisionText = colon < 0 ? QString{} : token.mid(colon + 1);
        if (source.isEmpty() || (colon >= 0 &&
            (precisionText.size() != 1 || precisionText.at(0) < QLatin1Char('0') ||
             precisionText.at(0) > QLatin1Char('6')))) {
            result.error = QStringLiteral("Invalid telemetry field {%1}").arg(token);
            return result;
        }
        std::optional<TelemetryValue> value;
        if (fieldExpression.match(source).hasMatch()) {
            value = ResolveTelemetryValue(source, cameraPosition, frame,
                                          runName, tick, context);
            if (!value) {
                result.error = QStringLiteral(
                        "Unknown telemetry field {%1}").arg(source);
                return result;
            }
        } else {
            TelemetryExpression math(source, cameraPosition, frame,
                                     runName, tick, context);
            const double numeric = math.evaluate();
            if (!math.error().isEmpty()) {
                result.error = math.error();
                return result;
            }
            value = TelemetryValue{numeric, true, std::isfinite(numeric)};
        }
        if (!precisionText.isEmpty() && !value->numeric) {
            result.error = QStringLiteral(
                    "Telemetry field {%1} does not accept precision")
                    .arg(source);
            return result;
        }
        QString formatted;
        if (value->numeric) {
            const int precision = precisionText.isEmpty()
                    ? 2
                    : precisionText.toInt();
            formatted = value->available
                    ? FormatTelemetryNumber(value->value.toDouble(), precision)
                    : QStringLiteral("--");
        } else {
            formatted = value->value.toString();
        }
        if (richText) {
            formatted = formatted.toHtmlEscaped();
            if (value->color.isValid()) {
                formatted = QStringLiteral("<span style=\"color:%1\">%2</span>")
                        .arg(value->color.name(QColor::HexRgb), formatted);
            }
        }
        result.text += formatted;
        index = close + 1;
    }
    while (result.text.endsWith(QLatin1Char('\n')))
        result.text.chop(1);
    if (richText)
        result.text.replace(QLatin1Char('\n'), QStringLiteral("<br/>"));
    return result;
}

}  // namespace

class ManualDriveRuntime {
public:
    struct Snapshot {
        Snapshot(std::int64_t time,
                 std::size_t frames,
                 forevervalidator::experimental::PhysicsSandboxState value)
            : timeMs(time), frameCount(frames), state(std::move(value)) {}
        std::int64_t timeMs;
        std::size_t frameCount;
        forevervalidator::experimental::PhysicsSandboxState state;
    };

    ManualDriveRuntime(
            forevervalidator::experimental::PhysicsSandbox sandboxValue,
            forevervalidator::experimental::PhysicsSandboxState initialValue,
            std::vector<
                    forevervalidator::experimental::PhysicsSandboxInputEvent>
                    fixedValue,
            forevervalidator::experimental::PhysicsSandboxStateView stateValue,
            std::uint32_t horizonMs)
        : sandbox(std::move(sandboxValue)),
          initialState(std::move(initialValue)),
          fixedInputs(std::move(fixedValue)),
          state(stateValue),
          simulationHorizonMs(horizonMs) {}

    forevervalidator::experimental::PhysicsSandbox sandbox;
    forevervalidator::experimental::PhysicsSandboxState initialState;
    std::vector<
            forevervalidator::experimental::PhysicsSandboxInputEvent>
            fixedInputs;
    std::vector<
            forevervalidator::experimental::PhysicsSandboxInputEvent>
            driverInputs;
    forevervalidator::experimental::PhysicsSandboxStateView state{};
    std::vector<
            forevervalidator::experimental::PhysicsSandboxInputEvent>
            simulatedInputs;
    std::vector<RaceViewerFrame> simulatedFrames;
    std::vector<Snapshot> snapshots;
    std::uint32_t simulationHorizonMs = 0u;
};

class RaceCameraResources {
public:
    forevervalidator::VehicleModel vehicleModel =
            forevervalidator::VehicleModel::Unknown;
    forevervalidator::camera::RaceCameraEnvironment vehicleEnvironment;
    std::optional<forevervalidator::camera::RaceCameraEnvironment>
            sharedFarEnvironment;
    QVector3D hoodLocalPosition{0.0f, 1.1f, 0.75f};
};

class RaceCameraRuntime {
public:
    explicit RaceCameraRuntime(
            std::shared_ptr<const RaceCameraResources> resources)
        : resources_(std::move(resources)) {}

    bool Evaluate(const RaceViewerRun &run,
                  int preset,
                  qint64 timeMs,
                  QVector3D *position,
                  QQuaternion *rotation,
                  QVector3D *target,
                  double *fieldOfView) {
        if (resources_ == nullptr || run.frames.empty() ||
            position == nullptr || rotation == nullptr ||
            target == nullptr || fieldOfView == nullptr) {
            return false;
        }
        const qint64 clampedTime = std::clamp<qint64>(
                timeMs, run.frames.front().timeMs,
                run.frames.back().timeMs);
        const RaceViewerFrame sampled = SampleFrame(run.frames, clampedTime);
        if (preset == 3) {
            // RaceViewerFrame::rotation is already the Qt local-to-world
            // rotation used by the rendered car. A Qt camera looks down its
            // local -Z axis while the car faces local +Z, so the only required
            // adjustment is the fixed 180-degree camera-axis rotation.
            const QQuaternion vehicleRotation = sampled.rotation.normalized();
            *position = sampled.position +
                    vehicleRotation.rotatedVector(
                            resources_->hoodLocalPosition);
            *rotation = vehicleRotation *
                    QQuaternion::fromAxisAndAngle(0.0f, 1.0f, 0.0f, 180.0f);
            *target = *position +
                    rotation->rotatedVector(QVector3D(0.0f, 0.0f, -10.0f));
            *fieldOfView = 75.0;
            session_.reset();
            runId_ = run.id;
            preset_ = preset;
            lastTimeMs_ = clampedTime;
            nextFrameIndex_ = 0u;
            return true;
        }

        if (preset != preset_ || run.id != runId_ ||
            !session_ || clampedTime < lastTimeMs_) {
            if (!Reset(run, preset)) {
                return false;
            }
        }
        if (clampedTime == lastTimeMs_ && outputValid_) {
            WriteOutput(position, rotation, target, fieldOfView);
            return true;
        }
        while (nextFrameIndex_ < run.frames.size() &&
               run.frames[nextFrameIndex_].timeMs <= clampedTime) {
            EvaluateFrame(run.frames[nextFrameIndex_]);
            ++nextFrameIndex_;
        }
        if (lastTimeMs_ < clampedTime) {
            EvaluateFrame(sampled);
        }
        if (!outputValid_) {
            return false;
        }
        WriteOutput(position, rotation, target, fieldOfView);
        return true;
    }

private:
    struct SessionSelection {
        const forevervalidator::camera::RaceCameraEnvironment *environment =
                nullptr;
        forevervalidator::camera::RaceCameraProfile profile =
                forevervalidator::camera::RaceCameraProfile::Race;
        std::optional<std::size_t> resourceIndex;
    };

    static RaceViewerFrame SampleFrame(
            const std::vector<RaceViewerFrame> &frames, qint64 timeMs) {
        const auto upper = std::lower_bound(
                frames.begin(), frames.end(), timeMs,
                [](const RaceViewerFrame &frame, qint64 time) {
                    return frame.timeMs < time;
                });
        if (upper == frames.begin()) {
            RaceViewerFrame result = *upper;
            result.timeMs = timeMs;
            return result;
        }
        if (upper == frames.end()) {
            RaceViewerFrame result = frames.back();
            result.timeMs = timeMs;
            return result;
        }
        if (upper->timeMs == timeMs) {
            return *upper;
        }
        const RaceViewerFrame &after = *upper;
        const RaceViewerFrame &before = *(upper - 1);
        const qint64 interval = after.timeMs - before.timeMs;
        const float blend = interval > 0
                ? static_cast<float>(timeMs - before.timeMs) /
                          static_cast<float>(interval)
                : 0.0f;
        RaceViewerFrame result = before;
        result.timeMs = timeMs;
        result.position = before.position * (1.0f - blend) +
                after.position * blend;
        result.rotation = QQuaternion::slerp(
                before.rotation, after.rotation, blend).normalized();
        result.linearSpeed = before.linearSpeed * (1.0f - blend) +
                after.linearSpeed * blend;
        result.signedSpeed = before.signedSpeed * (1.0f - blend) +
                after.signedSpeed * blend;
        result.accelerate = before.accelerate * (1.0f - blend) +
                after.accelerate * blend;
        result.brake = before.brake * (1.0f - blend) +
                after.brake * blend;
        result.steering = before.steering * (1.0f - blend) +
                after.steering * blend;
        return result;
    }

    static std::optional<std::size_t> FindResource(
            const forevervalidator::camera::RaceCameraEnvironment &environment,
            forevervalidator::camera::RaceCameraProfile profile,
            std::string_view text) {
        for (std::size_t index = 0u;
             index < environment.ResourceCount(profile); ++index) {
            if (environment.ResourceName(profile, index).find(text) !=
                std::string_view::npos) {
                return index;
            }
        }
        return std::nullopt;
    }

    std::optional<SessionSelection> SelectSession(int preset) const {
        using forevervalidator::VehicleModel;
        using forevervalidator::camera::RaceCameraProfile;
        const auto &vehicle = resources_->vehicleEnvironment;
        if (preset == 2) {
            if (vehicle.HasProfile(RaceCameraProfile::Race2)) {
                return SessionSelection{
                        &vehicle, RaceCameraProfile::Race2, std::nullopt};
            }
            if (vehicle.HasProfile(RaceCameraProfile::Race)) {
                return SessionSelection{
                        &vehicle, RaceCameraProfile::Race,
                        FindResource(vehicle, RaceCameraProfile::Race,
                                     "Raprochee")};
            }
            return std::nullopt;
        }
        if (vehicle.HasProfile(RaceCameraProfile::Race)) {
            std::optional<std::size_t> resource;
            if (resources_->vehicleModel == VehicleModel::SnowCar) {
                resource = FindResource(
                        vehicle, RaceCameraProfile::Race, "Normale2");
            } else if (resources_->vehicleModel == VehicleModel::RallyCar) {
                resource = FindResource(
                        vehicle, RaceCameraProfile::Race, "Rally");
            } else if (resources_->vehicleModel == VehicleModel::DesertCar) {
                const std::size_t count =
                        vehicle.ResourceCount(RaceCameraProfile::Race);
                if (count != 0u) {
                    resource = count - 1u;
                }
            }
            return SessionSelection{
                    &vehicle, RaceCameraProfile::Race, resource};
        }
        if (resources_->sharedFarEnvironment &&
            resources_->sharedFarEnvironment->HasProfile(
                    RaceCameraProfile::Race)) {
            const auto &shared = *resources_->sharedFarEnvironment;
            const std::size_t count =
                    shared.ResourceCount(RaceCameraProfile::Race);
            return SessionSelection{
                    &shared, RaceCameraProfile::Race,
                    count == 0u
                            ? std::optional<std::size_t>{}
                            : std::optional<std::size_t>{count - 1u}};
        }
        return std::nullopt;
    }

    static forevervalidator::camera::RaceCameraVehicleState ToCameraState(
            const RaceViewerFrame &frame) {
        forevervalidator::camera::RaceCameraVehicleState result;
        result.targetId = 1u;
        result.timeMs = static_cast<std::uint32_t>(
                std::clamp<std::int64_t>(
                        frame.timeMs, std::int64_t{0},
                        static_cast<std::int64_t>(
                                std::numeric_limits<std::uint32_t>::max())));
        result.transform.position = {
                frame.position.x(), frame.position.y(), frame.position.z()};
        // RaceViewerFrame stores Qt's column-basis local-to-world rotation.
        // Validator's camera controller consumes the game basis-row
        // convention, so transpose it just as WriteOutput transposes the
        // result in the opposite direction.
        const QQuaternion rotation = frame.rotation.conjugated().normalized();
        result.transform.rotation = {rotation.scalar(), rotation.x(),
                                     rotation.y(), rotation.z()};
        result.linearSpeed = {frame.linearSpeed.x(), frame.linearSpeed.y(),
                              frame.linearSpeed.z()};
        result.signedSpeed = frame.signedSpeed;
        result.steering = frame.steering;
        result.accelerate = frame.accelerate;
        result.brake = frame.brake;
        result.turbo = frame.turbo;
        result.cameraFlightTransition = frame.cameraFlightTransition;
        result.burning = frame.burning;
        result.gearChanged = frame.gearChanged;
        result.wheelContact = frame.wheelContact;
        result.wheelHasSurface = frame.wheelHasSurface;
        result.cameraSupportUp = {
                frame.cameraSupportUp.x(), frame.cameraSupportUp.y(),
                frame.cameraSupportUp.z()};
        return result;
    }

    bool Reset(const RaceViewerRun &run, int preset) {
        const std::optional<SessionSelection> selection =
                SelectSession(preset);
        if (!selection || selection->environment == nullptr) {
            return false;
        }
        if (selection->resourceIndex) {
            session_ = std::make_unique<
                    forevervalidator::camera::RaceCameraSession>(
                    *selection->environment, selection->profile,
                    *selection->resourceIndex);
        } else {
            session_ = std::make_unique<
                    forevervalidator::camera::RaceCameraSession>(
                    *selection->environment, selection->profile);
        }
        const forevervalidator::camera::RaceCameraVehicleState initial =
                ToCameraState(run.frames.front());
        session_->Reset(initial);
        runId_ = run.id;
        preset_ = preset;
        lastTimeMs_ = std::numeric_limits<qint64>::min();
        nextFrameIndex_ = 0u;
        outputValid_ = false;
        return true;
    }

    void EvaluateFrame(const RaceViewerFrame &frame) {
        forevervalidator::camera::RaceCameraQuery query;
        query.vehicle = ToCameraState(frame);
        output_ = session_->Evaluate(query);
        outputValid_ = true;
        lastTimeMs_ = frame.timeMs;
    }

    void WriteOutput(QVector3D *position,
                     QQuaternion *rotation,
                     QVector3D *target,
                     double *fieldOfView) const {
        *position = QVector3D(output_.transform.position.x,
                              output_.transform.position.y,
                              output_.transform.position.z);
        const QQuaternion validatorRotation(
                output_.transform.rotation.w,
                output_.transform.rotation.x,
                output_.transform.rotation.y,
                output_.transform.rotation.z);
        // Validator preserves the game camera basis-row convention. Qt uses
        // basis columns; conjugation performs the required transpose.
        *rotation = validatorRotation.conjugated().normalized() *
                QQuaternion::fromAxisAndAngle(0.0f, 1.0f, 0.0f, 180.0f);
        *target = *position +
                rotation->rotatedVector(QVector3D(0.0f, 0.0f, -10.0f));
        *fieldOfView = std::clamp<double>(
                output_.lens.fieldOfViewDegrees, 20.0, 150.0);
    }

    std::shared_ptr<const RaceCameraResources> resources_;
    std::unique_ptr<forevervalidator::camera::RaceCameraSession> session_;
    QString runId_;
    int preset_ = 0;
    qint64 lastTimeMs_ = std::numeric_limits<qint64>::min();
    std::size_t nextFrameIndex_ = 0u;
    forevervalidator::camera::RaceCameraOutput output_{};
    bool outputValid_ = false;
};

namespace {

using forevervalidator::DiscriminatedResult;
using forevervalidator::experimental::PhysicsSandboxCollisionTriangle;
using forevervalidator::experimental::PhysicsSandboxInputAction;
using forevervalidator::experimental::PhysicsSandboxInputEvent;
using forevervalidator::experimental::PhysicsSandboxInputValueKind;
using forevervalidator::experimental::PhysicsSandboxState;
using forevervalidator::experimental::PhysicsSandboxStateView;
using forevervalidator::experimental::PhysicsSandboxSwitchState;

constexpr std::uint32_t kViewerTickDurationMs = 10u;

template<typename T, typename Error>
T Require(DiscriminatedResult<T, Error> result, const char *operation) {
    if (!result) {
        std::string message = operation;
        if (!result.Error().diagnostic.empty()) {
            message += ": ";
            message += result.Error().diagnostic;
        }
        throw std::runtime_error(std::move(message));
    }
    return std::move(result).Value();
}

QVector3D ToQt(const forevervalidator::Vector3 &value) {
    return {value.x, value.y, value.z};
}

std::string VehicleCameraPackName(forevervalidator::VehicleModel model) {
    using forevervalidator::VehicleModel;
    switch (model) {
    case VehicleModel::SnowCar:
        return "Alpine";
    case VehicleModel::DesertCar:
        return "Speed";
    case VehicleModel::RallyCar:
        return "Rally";
    case VehicleModel::IslandCar:
        return "Island";
    case VehicleModel::CoastCar:
        return "Coast";
    case VehicleModel::BayCar:
        return "Bay";
    case VehicleModel::StadiumCar:
        return "Stadium";
    case VehicleModel::Unknown:
        break;
    }
    return {};
}

QVector3D HoodCameraPosition(
        const forevervalidator::experimental::PhysicsSandboxSceneView &scene) {
    float top = 1.1f;
    float front = 0.75f;
    for (const auto &ellipsoid : scene.carEllipsoids) {
        const QQuaternion rotation(
                ellipsoid.rotationW, ellipsoid.rotationX,
                ellipsoid.rotationY, ellipsoid.rotationZ);
        const QVector3D x = rotation.rotatedVector(
                QVector3D(ellipsoid.radii.x, 0.0f, 0.0f));
        const QVector3D y = rotation.rotatedVector(
                QVector3D(0.0f, ellipsoid.radii.y, 0.0f));
        const QVector3D z = rotation.rotatedVector(
                QVector3D(0.0f, 0.0f, ellipsoid.radii.z));
        const QVector3D extent(
                std::fabs(x.x()) + std::fabs(y.x()) + std::fabs(z.x()),
                std::fabs(x.y()) + std::fabs(y.y()) + std::fabs(z.y()),
                std::fabs(x.z()) + std::fabs(y.z()) + std::fabs(z.z()));
        top = std::max(top, ellipsoid.position.y + extent.y());
        front = std::max(front, ellipsoid.position.z + extent.z());
    }
    return QVector3D(
            0.0f,
            std::clamp(top - 0.28f, 0.75f, 1.8f),
            std::clamp(front * 0.62f, 0.35f, 1.5f));
}

std::shared_ptr<const RaceCameraResources> LoadCameraResources(
        const QString &packsDirectory,
        forevervalidator::VehicleModel vehicleModel,
        const forevervalidator::experimental::PhysicsSandboxSceneView &scene) {
    const std::string packName = VehicleCameraPackName(vehicleModel);
    if (packName.empty()) {
        return {};
    }
    auto loaded = forevervalidator::LoadInstalledRaceCameraEnvironment(
            packsDirectory.toUtf8().toStdString(), packName);
    if (!loaded) {
        return {};
    }
    auto resources = std::make_shared<RaceCameraResources>();
    resources->vehicleModel = vehicleModel;
    resources->vehicleEnvironment = std::move(loaded).Value();
    resources->hoodLocalPosition = HoodCameraPosition(scene);
    using forevervalidator::camera::RaceCameraProfile;
    if (!resources->vehicleEnvironment.HasProfile(
                RaceCameraProfile::Race)) {
        auto shared = forevervalidator::LoadInstalledRaceCameraEnvironment(
                packsDirectory.toUtf8().toStdString(), "Alpine");
        if (shared) {
            resources->sharedFarEnvironment = std::move(shared).Value();
        }
    }
    return resources;
}

bool IsDriverInput(PhysicsSandboxInputAction action) {
    return action == PhysicsSandboxInputAction::Accelerate ||
            action == PhysicsSandboxInputAction::Gas ||
            action == PhysicsSandboxInputAction::Brake ||
            action == PhysicsSandboxInputAction::Steer ||
            action == PhysicsSandboxInputAction::SteerLeft ||
            action == PhysicsSandboxInputAction::SteerRight ||
            action == PhysicsSandboxInputAction::Respawn;
}

bool IsSteeringInput(PhysicsSandboxInputAction action) {
    return action == PhysicsSandboxInputAction::Steer ||
            action == PhysicsSandboxInputAction::SteerLeft ||
            action == PhysicsSandboxInputAction::SteerRight;
}

bool IsLongitudinalInput(PhysicsSandboxInputAction action) {
    return action == PhysicsSandboxInputAction::Accelerate ||
            action == PhysicsSandboxInputAction::Gas ||
            action == PhysicsSandboxInputAction::Brake;
}

bool SwitchInputActiveAt(
        const std::vector<PhysicsSandboxInputEvent> &events,
        PhysicsSandboxInputAction action,
        std::int32_t timeMs) {
    bool active = false;
    for (const PhysicsSandboxInputEvent &event : events) {
        if (event.timeMs > timeMs) {
            break;
        }
        if (event.action == action &&
            event.value.kind == PhysicsSandboxInputValueKind::Switch) {
            active = event.value.switchState !=
                    PhysicsSandboxSwitchState::Released;
        }
    }
    return active;
}

forevervalidator::AnalogInputState AnalogInputAt(
        const std::vector<PhysicsSandboxInputEvent> &events,
        PhysicsSandboxInputAction action,
        std::int32_t timeMs) {
    forevervalidator::AnalogInputState value = 0;
    for (const PhysicsSandboxInputEvent &event : events) {
        if (event.timeMs > timeMs) {
            break;
        }
        if (event.action == action &&
            event.value.kind == PhysicsSandboxInputValueKind::Analog) {
            value = event.value.analog;
        }
    }
    return value;
}

PhysicsSandboxInputEvent ManualSwitchEvent(
        std::int32_t timeMs,
        PhysicsSandboxInputAction action,
        bool active) {
    PhysicsSandboxInputEvent event;
    event.timeMs = timeMs;
    event.action = action;
    event.value.kind = PhysicsSandboxInputValueKind::Switch;
    event.value.switchState = active
            ? PhysicsSandboxSwitchState::Pressed
            : PhysicsSandboxSwitchState::Released;
    return event;
}

PhysicsSandboxInputEvent ManualAnalogInputEvent(
        std::int32_t timeMs,
        PhysicsSandboxInputAction action,
        forevervalidator::AnalogInputState value) {
    PhysicsSandboxInputEvent event;
    event.timeMs = timeMs;
    event.action = action;
    event.value.kind = PhysicsSandboxInputValueKind::Analog;
    event.value.analog = value;
    return event;
}

QString SandboxErrorText(
        const forevervalidator::experimental::PhysicsSandboxError &error) {
    return error.diagnostic.empty()
            ? QStringLiteral("physics simulation failed")
            : QString::fromUtf8(error.diagnostic);
}

std::vector<PhysicsSandboxInputEvent> ViewerFixedInputs(
        const std::vector<PhysicsSandboxInputEvent> &canonicalInputs) {
    std::vector<PhysicsSandboxInputEvent> fixedInputs;
    fixedInputs.reserve(canonicalInputs.size());
    std::copy_if(
            canonicalInputs.begin(),
            canonicalInputs.end(),
            std::back_inserter(fixedInputs),
            [](const PhysicsSandboxInputEvent &event) {
                return !IsDriverInput(event.action);
            });
    return fixedInputs;
}

struct FilledVertex {
    float x;
    float y;
    float z;
    float r;
    float g;
    float b;
    float a;
};

struct WireVertex {
    float x;
    float y;
    float z;
};

struct ViewerTriangle {
    QVector3D a;
    QVector3D b;
    QVector3D c;
};

void ExpandBounds(const QVector3D &point,
                  QVector3D &minimum,
                  QVector3D &maximum) {
    minimum.setX(std::min(minimum.x(), point.x()));
    minimum.setY(std::min(minimum.y(), point.y()));
    minimum.setZ(std::min(minimum.z(), point.z()));
    maximum.setX(std::max(maximum.x(), point.x()));
    maximum.setY(std::max(maximum.y(), point.y()));
    maximum.setZ(std::max(maximum.z(), point.z()));
}

constexpr int kCarPaletteCount = 16;

std::array<float, 4u> FaceColor(const ViewerTriangle &triangle,
                                int carPalette) {
    QVector3D normal = QVector3D::crossProduct(
            triangle.b - triangle.a, triangle.c - triangle.a);
    if (normal.lengthSquared() > 0.0f) {
        normal.normalize();
    }
    const float x = std::fabs(normal.x());
    const float y = std::fabs(normal.y());
    const float z = std::fabs(normal.z());
    switch (carPalette) {
    case -2: {
        const float shade = 0.69f + 0.22f * y + 0.09f * z;
        return {shade, shade, shade, 1.0f};
    }
    case 0:
        // Preserve the original orange collision-car palette exactly.
        return {0.78f + 0.16f * y,
                0.26f + 0.16f * z,
                0.08f + 0.10f * x,
                1.0f};
    case 1:
        return {0.08f + 0.10f * x,
                0.26f + 0.16f * z,
                0.78f + 0.16f * y,
                1.0f};
    case 2:
        return {0.10f + 0.10f * x,
                0.65f + 0.22f * y,
                0.20f + 0.14f * z,
                1.0f};
    case 3:
        return {0.55f + 0.20f * y,
                0.18f + 0.10f * x,
                0.72f + 0.18f * z,
                1.0f};
    case 4:
        return {0.72f + 0.18f * y,
                0.58f + 0.18f * z,
                0.08f + 0.08f * x,
                1.0f};
    case 5:
        return {0.08f + 0.08f * x,
                0.62f + 0.20f * y,
                0.68f + 0.18f * z,
                1.0f};
    case 6:
        return {0.80f + 0.16f * y, 0.23f + 0.12f * z,
                0.25f + 0.12f * x, 1.0f};
    case 7:
        return {0.31f + 0.12f * x, 0.39f + 0.14f * z,
                0.81f + 0.16f * y, 1.0f};
    case 8:
        return {0.56f + 0.16f * y, 0.70f + 0.14f * z,
                0.12f + 0.10f * x, 1.0f};
    case 9:
        return {0.77f + 0.15f * y, 0.33f + 0.13f * z,
                0.59f + 0.12f * x, 1.0f};
    case 10:
        return {0.13f + 0.12f * x, 0.56f + 0.13f * z,
                0.77f + 0.16f * y, 1.0f};
    case 11:
        return {0.61f + 0.13f * y, 0.50f + 0.12f * z,
                0.22f + 0.10f * x, 1.0f};
    case 12:
        return {0.72f + 0.13f * y, 0.18f + 0.11f * z,
                0.14f + 0.10f * x, 1.0f};
    case 13:
        return {0.45f + 0.12f * x, 0.31f + 0.12f * z,
                0.73f + 0.15f * y, 1.0f};
    case 14:
        return {0.11f + 0.10f * x, 0.56f + 0.13f * z,
                0.43f + 0.12f * y, 1.0f};
    case 15:
        return {0.72f + 0.13f * y, 0.25f + 0.11f * z,
                0.57f + 0.13f * x, 1.0f};
    default:
        return {0.26f + 0.20f * x,
                0.36f + 0.26f * y,
                0.43f + 0.20f * z,
                1.0f};
    }
}

RaceViewerMeshBuffers BuildMeshBuffers(
        const std::vector<ViewerTriangle> &triangles,
        int carPalette) {
    RaceViewerMeshBuffers result;
    if (triangles.empty()) {
        return result;
    }
    constexpr qsizetype FilledBytesPerTriangle =
            static_cast<qsizetype>(3u * sizeof(FilledVertex));
    constexpr qsizetype WireBytesPerTriangle =
            static_cast<qsizetype>(6u * sizeof(WireVertex));
    if (triangles.size() > static_cast<std::size_t>(
                std::numeric_limits<qsizetype>::max() /
                std::max(FilledBytesPerTriangle, WireBytesPerTriangle))) {
        throw std::runtime_error("viewer geometry is too large");
    }

    result.filled.resize(static_cast<qsizetype>(triangles.size()) *
                         FilledBytesPerTriangle);
    result.wire.resize(static_cast<qsizetype>(triangles.size()) *
                       WireBytesPerTriangle);
    auto *filled = reinterpret_cast<FilledVertex *>(result.filled.data());
    auto *wire = reinterpret_cast<WireVertex *>(result.wire.data());
    result.boundsMin = triangles.front().a;
    result.boundsMax = triangles.front().a;

    const auto addWire = [&](const QVector3D &point) {
        *wire++ = {point.x(), point.y(), point.z()};
    };
    for (const ViewerTriangle &triangle : triangles) {
        const std::array<float, 4u> color =
                FaceColor(triangle, carPalette);
        const std::array<QVector3D, 3u> points{
                triangle.a, triangle.b, triangle.c};
        for (const QVector3D &point : points) {
            *filled++ = {point.x(), point.y(), point.z(),
                         color[0], color[1], color[2], color[3]};
            ExpandBounds(point, result.boundsMin, result.boundsMax);
        }
        addWire(triangle.a);
        addWire(triangle.b);
        addWire(triangle.b);
        addWire(triangle.c);
        addWire(triangle.c);
        addWire(triangle.a);
    }
    return result;
}

RaceViewerMeshBuffers BuildTrajectoryMesh(
        const std::vector<RaceViewerFrame> &frames,
        float radius) {
    std::vector<ViewerTriangle> triangles;
    if (frames.empty()) {
        return {};
    }
    if (frames.size() - 1u > triangles.max_size() / 12u) {
        throw std::length_error("trajectory geometry is too large");
    }
    triangles.reserve(std::max<std::size_t>(1u, frames.size() - 1u) * 12u);
    const auto addQuad = [&triangles](const QVector3D &a,
                                     const QVector3D &b,
                                     const QVector3D &c,
                                     const QVector3D &d) {
        triangles.push_back({a, b, c});
        triangles.push_back({a, c, d});
    };
    for (std::size_t index = 1u; index < frames.size(); ++index) {
        const QVector3D start = frames[index - 1u].position;
        const QVector3D end = frames[index].position;
        QVector3D direction = end - start;
        if (direction.lengthSquared() < 0.000001f) {
            continue;
        }
        direction.normalize();
        const QVector3D reference =
                std::fabs(QVector3D::dotProduct(
                                  direction, QVector3D(0.0f, 1.0f, 0.0f))) <
                        0.9f
                ? QVector3D(0.0f, 1.0f, 0.0f)
                : QVector3D(1.0f, 0.0f, 0.0f);
        const QVector3D side =
                QVector3D::crossProduct(direction, reference).normalized() *
                radius;
        const QVector3D normal =
                QVector3D::crossProduct(side, direction).normalized() *
                radius;
        const std::array<QVector3D, 4u> startCorners{
                start + side + normal,
                start - side + normal,
                start - side - normal,
                start + side - normal};
        const std::array<QVector3D, 4u> endCorners{
                end + side + normal,
                end - side + normal,
                end - side - normal,
                end + side - normal};
        addQuad(startCorners[0],
                startCorners[1],
                startCorners[2],
                startCorners[3]);
        addQuad(endCorners[3],
                endCorners[2],
                endCorners[1],
                endCorners[0]);
        for (std::size_t sideIndex = 0u; sideIndex < 4u; ++sideIndex) {
            const std::size_t next = (sideIndex + 1u) % 4u;
            addQuad(startCorners[sideIndex],
                    endCorners[sideIndex],
                    endCorners[next],
                    startCorners[next]);
        }
    }
    if (triangles.empty()) {
        const QVector3D center = frames.front().position;
        const QVector3D x(radius, 0.0f, 0.0f);
        const QVector3D y(0.0f, radius, 0.0f);
        const QVector3D z(0.0f, 0.0f, radius);
        const std::array<QVector3D, 8u> corners{
                center - x - y - z,
                center + x - y - z,
                center + x + y - z,
                center - x + y - z,
                center - x - y + z,
                center + x - y + z,
                center + x + y + z,
                center - x + y + z};
        addQuad(corners[0], corners[1], corners[2], corners[3]);
        addQuad(corners[7], corners[6], corners[5], corners[4]);
        addQuad(corners[0], corners[4], corners[5], corners[1]);
        addQuad(corners[1], corners[5], corners[6], corners[2]);
        addQuad(corners[2], corners[6], corners[7], corners[3]);
        addQuad(corners[3], corners[7], corners[4], corners[0]);
    }
    return BuildMeshBuffers(triangles, 2);
}

RaceViewerMeshBuffers BuildTrajectoryLineMesh(
        const std::vector<RaceViewerFrame> &frames,
        float stationaryMarkerRadius,
        bool dashed = false) {
    RaceViewerMeshBuffers result;
    if (frames.empty()) {
        return result;
    }
    std::vector<WireVertex> vertices;
    if (frames.size() - 1u > vertices.max_size() / 2u) {
        throw std::length_error("trajectory line is too large");
    }
    vertices.reserve((frames.size() - 1u) * 2u);
    const auto append = [&vertices](const QVector3D &point) {
        vertices.push_back({point.x(), point.y(), point.z()});
    };
    float traveled = 0.0f;
    const float dashLength = std::max(1.5f, stationaryMarkerRadius * 15.0f);
    for (std::size_t index = 1u; index < frames.size(); ++index) {
        const QVector3D &start = frames[index - 1u].position;
        const QVector3D &end = frames[index].position;
        const float distance = (end - start).length();
        if (distance < 0.001f) {
            continue;
        }
        if (!dashed) {
            append(start);
            append(end);
        } else {
            float local = 0.0f;
            while (local < distance) {
                const float phase = std::fmod(traveled + local,
                                              dashLength * 2.0f);
                const bool draw = phase < dashLength;
                const float remaining = draw ? dashLength - phase
                                             : dashLength * 2.0f - phase;
                const float next = std::min(distance,
                                            local + std::max(remaining, 0.001f));
                if (draw) {
                    append(start + (end - start) * (local / distance));
                    append(start + (end - start) * (next / distance));
                }
                local = next;
            }
        }
        traveled += distance;
    }
    if (vertices.empty()) {
        const QVector3D center = frames.front().position;
        const QVector3D x(stationaryMarkerRadius, 0.0f, 0.0f);
        const QVector3D y(0.0f, stationaryMarkerRadius, 0.0f);
        const QVector3D z(0.0f, 0.0f, stationaryMarkerRadius);
        append(center - x);
        append(center + x);
        append(center - y);
        append(center + y);
        append(center - z);
        append(center + z);
    }
    if (vertices.size() > static_cast<std::size_t>(
                std::numeric_limits<qsizetype>::max() /
                sizeof(WireVertex))) {
        throw std::length_error("trajectory line geometry is too large");
    }
    result.boundsMin = QVector3D(
            vertices.front().x, vertices.front().y, vertices.front().z);
    result.boundsMax = result.boundsMin;
    for (const WireVertex &vertex : vertices) {
        ExpandBounds(
                QVector3D(vertex.x, vertex.y, vertex.z),
                result.boundsMin,
                result.boundsMax);
    }
    result.wire.resize(
            static_cast<qsizetype>(vertices.size() * sizeof(WireVertex)));
    std::memcpy(result.wire.data(),
                vertices.data(),
                static_cast<std::size_t>(result.wire.size()));
    return result;
}

QVariantMap MaterialMap(ReplacementMaterialClass materialClass) {
    const ReplacementMaterial replacement = ReplacementFor(materialClass);
    QVariantMap map;
    map.insert(QStringLiteral("materialClass"),
               MaterialClassName(materialClass));
    map.insert(QStringLiteral("debugColor"), replacement.debugColor);
    map.insert(QStringLiteral("baseTexture"), replacement.baseTexture);
    map.insert(QStringLiteral("roughness"), replacement.roughness);
    map.insert(QStringLiteral("metalness"), replacement.metalness);
    map.insert(QStringLiteral("emissiveStrength"),
               replacement.emissiveStrength);
    map.insert(QStringLiteral("unknown"),
               materialClass == ReplacementMaterialClass::Unknown);
    return map;
}

std::vector<ViewerTriangle> UnitEllipsoidTriangles() {
    constexpr unsigned Latitudes = 12u;
    constexpr unsigned Longitudes = 20u;
    constexpr float Pi = 3.14159265358979323846f;
    std::vector<ViewerTriangle> triangles;
    triangles.reserve(2u * Longitudes * (Latitudes - 1u));
    const auto point = [](float phi, float theta) {
        const float ring = std::cos(phi);
        return QVector3D(ring * std::cos(theta),
                         std::sin(phi),
                         ring * std::sin(theta));
    };
    const auto appendOutward = [&triangles](QVector3D a,
                                            QVector3D b,
                                            QVector3D c) {
        const QVector3D normal = QVector3D::crossProduct(b - a, c - a);
        if (QVector3D::dotProduct(normal, a + b + c) < 0.0f) {
            std::swap(b, c);
        }
        triangles.push_back({a, b, c});
    };
    for (unsigned latitude = 0u; latitude < Latitudes; ++latitude) {
        const float phi0 = -0.5f * Pi + Pi *
                static_cast<float>(latitude) /
                static_cast<float>(Latitudes);
        const float phi1 = -0.5f * Pi + Pi *
                static_cast<float>(latitude + 1u) /
                static_cast<float>(Latitudes);
        for (unsigned longitude = 0u; longitude < Longitudes; ++longitude) {
            const float theta0 = 2.0f * Pi *
                    static_cast<float>(longitude) /
                    static_cast<float>(Longitudes);
            const float theta1 = 2.0f * Pi *
                    static_cast<float>(longitude + 1u) /
                    static_cast<float>(Longitudes);
            const QVector3D a = point(phi0, theta0);
            const QVector3D b = point(phi0, theta1);
            const QVector3D c = point(phi1, theta1);
            const QVector3D d = point(phi1, theta0);
            if (latitude != 0u) {
                appendOutward(a, b, c);
            }
            if (latitude + 1u != Latitudes) {
                appendOutward(a, c, d);
            }
        }
    }
    return triangles;
}

RaceViewerLoadResult LoadMapData(const QString &packsDirectory,
                                 const QString &replayPath,
                                 PhysicsBackend backend,
                                 std::uint32_t simulationHorizonMs) {
    static_cast<void>(backend);
    using namespace forevervalidator;
    using namespace forevervalidator::experimental;

    RaceViewerLoadResult result;
    result.packsDirectory = packsDirectory;
    result.replayPath = replayPath;
    result.backend = kAuxiliarySimulationBackend;
    try {
        const std::string replayPathUtf8 =
                replayPath.toUtf8().toStdString();
        const ReplayIdentity identity{replayPathUtf8};
        AssetSource source = Require(
                OpenInstalledPackDirectory(
                        packsDirectory.toUtf8().toStdString()),
                "opening Packs directory failed");
        AssetBytes bytes = Require(
                ReadReplayFileUtf8(replayPathUtf8, identity),
                "reading scenario failed");
        PhysicsSandboxOptions options;
        options.backend = ToForeverValidatorBackend(
                kAuxiliarySimulationBackend);
        options.tickDurationMs = kViewerTickDurationMs;
        options.timelineMode = PhysicsSandboxTimelineMode::Canonical;
        options.simulationHorizonMs = simulationHorizonMs;
        PhysicsSandbox sandbox = Require(
                CreatePhysicsSandbox(std::move(source), options),
                "creating scenario sandbox failed");
        const PhysicsSandboxStateView initialState = Require(
                sandbox.LoadScenario({bytes.data(), bytes.size()}, identity),
                "loading scenario failed");
        PhysicsSandboxSceneView scene = Require(
                sandbox.ReadScene(), "reading scenario scene failed");
        result.cameraResources = LoadCameraResources(
                packsDirectory, initialState.vehicleModel, scene);
        result.mapKey = CollisionSceneKey(scene);
        result.mapName = QString::fromUtf8(Require(
                sandbox.ReadMapName(),
                "reading scenario map name failed"));
        PhysicsSandboxRenderSceneHandle renderScene = Require(
                sandbox.ReadRenderScene(),
                "reading visual render scene failed");

        std::vector<ViewerTriangle> triangles;
        triangles.reserve(scene.collisionTriangles.size());
        for (const PhysicsSandboxCollisionTriangle &triangle :
             scene.collisionTriangles) {
            triangles.push_back({
                    ToQt(triangle.a), ToQt(triangle.b), ToQt(triangle.c)});
        }
        result.track = BuildMeshBuffers(triangles, -1);
        result.triangleCount = static_cast<qint64>(triangles.size());

        StaticVisualBatchResult batches =
                BuildStaticVisualBatches(*renderScene);
        result.materialCount =
                static_cast<qint64>(renderScene->materials.size());
        result.diagnosticCount +=
                static_cast<qint64>(renderScene->diagnostics.size()) +
                static_cast<qint64>(batches.invalidInstanceCount) +
                static_cast<qint64>(batches.duplicateInstanceCount);
        result.sourceVisualObjectCount =
                static_cast<qint64>(batches.defaultVisibleInstanceCount);
        result.sourceVisualMeshCount =
                static_cast<qint64>(batches.sourceMeshCount);
        result.duplicateVisualObjectCount =
                static_cast<qint64>(batches.duplicateInstanceCount);
        result.visualTriangleCount =
                static_cast<qint64>(batches.defaultTriangleCount);
        result.visualBoundsMin = batches.defaultBoundsMin;
        result.visualBoundsMax = batches.defaultBoundsMax;

        struct MaterialBindingKey {
            ReplacementMaterialClass materialClass =
                    ReplacementMaterialClass::Unknown;
            bool vertexColors = false;
        };
        std::vector<MaterialBindingKey> materialBindings;
        result.visualBatches = std::move(batches.batches);
        result.visualBatchItems.reserve(result.visualBatches.size());
        for (std::size_t batchIndex = 0u;
             batchIndex < result.visualBatches.size(); ++batchIndex) {
            const StaticVisualBatch &batch = result.visualBatches[batchIndex];
            const bool applyVertexColors =
                    batch.hasVertexColors &&
                    ReplacementFor(batch.materialClass).applyVertexColors;
            std::size_t materialBindingIndex = 0u;
            for (; materialBindingIndex < materialBindings.size();
                 ++materialBindingIndex) {
                const MaterialBindingKey &binding =
                        materialBindings[materialBindingIndex];
                if (binding.materialClass == batch.materialClass &&
                    binding.vertexColors == applyVertexColors) {
                    break;
                }
            }
            if (materialBindingIndex == materialBindings.size()) {
                materialBindings.push_back(
                        {batch.materialClass, applyVertexColors});
                QVariantMap binding = MaterialMap(batch.materialClass);
                binding.insert(QStringLiteral("vertexColors"),
                               applyVertexColors);
                if (batch.materialClass == ReplacementMaterialClass::Unknown) {
                    ++result.diagnosticCount;
                }
                result.visualMaterials.push_back(std::move(binding));
            }

            QVariantMap item;
            item.insert(QStringLiteral("batchIndex"),
                        static_cast<qint64>(batchIndex));
            item.insert(QStringLiteral("materialBindingIndex"),
                        static_cast<qint64>(materialBindingIndex));
            item.insert(QStringLiteral("materialClass"),
                        MaterialClassName(batch.materialClass));
            item.insert(QStringLiteral("defaultVisible"),
                        batch.defaultVisible);
            item.insert(QStringLiteral("sourceInstanceCount"),
                        static_cast<qint64>(batch.sourceInstanceCount));
            item.insert(QStringLiteral("triangleCount"),
                        static_cast<qint64>(batch.triangleCount));
            result.visualBatchItems.push_back(std::move(item));
        }
        if (result.sourceVisualObjectCount == 0) {
            result.visualBoundsMin = result.track.boundsMin;
            result.visualBoundsMax = result.track.boundsMax;
        }
        for (const PhysicsSandboxEllipsoid &ellipsoid :
             scene.carEllipsoids) {
            QVariantMap item;
            item.insert(QStringLiteral("position"), ToQt(ellipsoid.position));
            item.insert(QStringLiteral("rotation"),
                        QQuaternion(ellipsoid.rotationW,
                                    ellipsoid.rotationX,
                                    ellipsoid.rotationY,
                                    ellipsoid.rotationZ).normalized());
            item.insert(QStringLiteral("radii"), ToQt(ellipsoid.radii));
            result.carEllipsoids.push_back(std::move(item));
        }

        const std::vector<PhysicsSandboxInputEvent> canonicalInputs =
                Require(sandbox.ReadInputs(),
                        "reading canonical inputs for manual driving failed");
        std::vector<PhysicsSandboxInputEvent> fixedInputs =
                ViewerFixedInputs(canonicalInputs);
        PhysicsSandboxState manualStart = Require(
                sandbox.CaptureState(),
                "capturing manual-driving start state failed");
        result.manualRuntime = std::make_shared<ManualDriveRuntime>(
                std::move(sandbox),
                std::move(manualStart),
                std::move(fixedInputs),
                initialState,
                simulationHorizonMs);
    } catch (const std::exception &exception) {
        result.error = QString::fromUtf8(exception.what());
    } catch (...) {
        result.error = QStringLiteral("Unexpected scenario viewer failure");
    }
    return result;
}

std::shared_ptr<ManualDriveRuntime> LoadInputPreviewRuntime(
        const QString &packsDirectory,
        const QString &replayPath,
        PhysicsBackend backend,
        std::uint32_t simulationHorizonMs) {
    static_cast<void>(backend);
    using namespace forevervalidator;
    using namespace forevervalidator::experimental;

    const std::string replayPathUtf8 = replayPath.toUtf8().toStdString();
    const ReplayIdentity identity{replayPathUtf8};
    AssetSource source = Require(
            OpenInstalledPackDirectory(
                    packsDirectory.toUtf8().toStdString()),
            "opening Packs directory for input preview failed");
    AssetBytes bytes = Require(
            ReadReplayFileUtf8(replayPathUtf8, identity),
            "reading scenario for input preview failed");
    PhysicsSandboxOptions options;
    options.backend = ToForeverValidatorBackend(
            kAuxiliarySimulationBackend);
    options.tickDurationMs = kViewerTickDurationMs;
    options.timelineMode = PhysicsSandboxTimelineMode::Canonical;
    options.simulationHorizonMs = simulationHorizonMs;
    PhysicsSandbox sandbox = Require(
            CreatePhysicsSandbox(std::move(source), options),
            "creating input preview sandbox failed");
    const PhysicsSandboxStateView initialState = Require(
            sandbox.LoadScenario({bytes.data(), bytes.size()}, identity),
            "loading scenario for input preview failed");
    std::vector<PhysicsSandboxInputEvent> fixedInputs = ViewerFixedInputs(
            Require(sandbox.ReadInputs(),
                    "reading canonical inputs for input preview failed"));
    PhysicsSandboxState initial = Require(
            sandbox.CaptureState(),
            "capturing input preview start state failed");
    return std::make_shared<ManualDriveRuntime>(
            std::move(sandbox),
            std::move(initial),
            std::move(fixedInputs),
            initialState,
            simulationHorizonMs);
}

QString FormatTime(qint64 milliseconds) {
    return QString::fromStdString(FormatFixedDurationMilliseconds(
            static_cast<std::uint64_t>(
                    std::max<qint64>(0, milliseconds))));
}

std::vector<RaceViewerFrame> ToViewerFrames(
        const std::vector<SearchTimelineFrame> &frames) {
    std::vector<RaceViewerFrame> result;
    result.reserve(frames.size());
    for (const SearchTimelineFrame &frame : frames) {
        RaceViewerFrame viewerFrame{
                frame.timeMs,
                QVector3D(frame.positionX,
                          frame.positionY,
                          frame.positionZ),
                QQuaternion(frame.rotationW,
                            frame.rotationX,
                            frame.rotationY,
                            frame.rotationZ).normalized(),
                frame.accelerate,
                frame.brake,
                frame.steering,
                frame.checkpointsCollected,
                frame.checkpointsTotal,
                frame.completedLaps,
                frame.totalLaps,
                frame.raceCompleted,
                frame.finishTimeMs,
                QVector3D(frame.linearSpeedX,
                          frame.linearSpeedY,
                          frame.linearSpeedZ),
                frame.signedSpeed,
                frame.turbo,
                frame.cameraFlightTransition,
                frame.burning,
                frame.gearChanged,
                frame.wheelContact,
                frame.wheelHasSurface,
                QVector3D(frame.cameraSupportUpX,
                          frame.cameraSupportUpY,
                          frame.cameraSupportUpZ),
                frame.stuntsScore};
        result.push_back(std::move(viewerFrame));
    }
    return result;
}

bool IsViewableTrajectory(
        const std::vector<RaceViewerFrame> &frames) {
    if (frames.empty() || frames.front().timeMs != 0) {
        return false;
    }
    std::int64_t previousTime = -1;
    for (const RaceViewerFrame &frame : frames) {
        if (frame.timeMs <= previousTime ||
            !std::isfinite(frame.position.x()) ||
            !std::isfinite(frame.position.y()) ||
            !std::isfinite(frame.position.z())) {
            return false;
        }
        previousTime = frame.timeMs;
    }
    return true;
}

RaceViewerFrame ToViewerFrame(const PhysicsSandboxStateView &state) {
    RaceViewerFrame frame{
            static_cast<std::int64_t>(state.timeMs),
            ToQt(state.car.position),
            QQuaternion(state.car.rotationW,
                        state.car.rotationX,
                        state.car.rotationY,
                        state.car.rotationZ).normalized(),
            state.accelerate,
            state.brake,
            state.steering,
            state.checkpointsCollected,
            state.checkpointsTotal,
            state.completedLaps,
            state.totalLaps,
            state.raceCompleted,
            state.finishTimeMs,
            ToQt(state.car.linearSpeed),
            state.car.signedSpeed,
            state.car.turbo,
            state.car.cameraFlightTransition,
            state.car.burning,
            state.car.gearChanged,
            state.car.wheelContact,
            state.car.wheelHasSurface,
            ToQt(state.car.cameraSupportUp),
            state.stuntsScore};
    return frame;
}

RaceViewerInputPreviewResult BuildInputPreview(
        const QString &packsDirectory,
        const QString &replayPath,
        PhysicsBackend backend,
        std::uint32_t simulationHorizonMs,
        const QString &script,
        float trajectoryRadius,
        std::shared_ptr<ManualDriveRuntime> runtime) {
    RaceViewerInputPreviewResult result;
    const auto canceled = []() {
        QThread *const thread = QThread::currentThread();
        return thread != nullptr && thread->isInterruptionRequested();
    };
    try {
        const InputScriptParseResult parsed =
                ParseInputScript(script.toStdString());
        if (!parsed || canceled()) {
            result.canceled = canceled();
            return result;
        }
        if (runtime == nullptr) {
            runtime = LoadInputPreviewRuntime(
                    packsDirectory,
                    replayPath,
                    backend,
                    simulationHorizonMs);
        }
        result.runtime = runtime;
        if (canceled()) {
            result.canceled = true;
            return result;
        }

        InputScriptBaselineResult baseline = BuildInputScriptBaseline(
                runtime->fixedInputs,
                parsed.commands,
                kViewerTickDurationMs);
        if (!baseline) {
            return result;
        }
        ConvertKeyboardSteeringToAnalog(baseline.events);
        result.inputs = baseline.events;

        std::int64_t earliestChangedTimeMs =
                std::numeric_limits<std::int64_t>::max();
        std::size_t commonInputCount = std::min(
                runtime->simulatedInputs.size(), baseline.events.size());
        std::size_t firstChangedInput = 0u;
        while (firstChangedInput < commonInputCount &&
               SameInputEvent(runtime->simulatedInputs[firstChangedInput],
                              baseline.events[firstChangedInput])) {
            ++firstChangedInput;
        }
        if (firstChangedInput < runtime->simulatedInputs.size()) {
            earliestChangedTimeMs = std::min(
                    earliestChangedTimeMs,
                    static_cast<std::int64_t>(
                            runtime->simulatedInputs[firstChangedInput]
                                    .timeMs));
        }
        if (firstChangedInput < baseline.events.size()) {
            earliestChangedTimeMs = std::min(
                    earliestChangedTimeMs,
                    static_cast<std::int64_t>(
                            baseline.events[firstChangedInput].timeMs));
        }

        auto initial = runtime->sandbox.RestoreState(runtime->initialState);
        if (!initial) {
            result.error = QStringLiteral(
                                   "Updating the input preview failed: %1")
                                   .arg(SandboxErrorText(initial.Error()));
            return result;
        }
        auto resized = runtime->sandbox.SetSimulationHorizonMs(
                simulationHorizonMs);
        if (!resized) {
            result.error = QStringLiteral(
                                   "Updating the input preview failed: %1")
                                   .arg(SandboxErrorText(resized.Error()));
            return result;
        }
        initial = std::move(resized);

        std::size_t reusableSnapshotCount = 0u;
        for (std::size_t index = 0u; index < runtime->snapshots.size();
             ++index) {
            const ManualDriveRuntime::Snapshot &snapshot =
                    runtime->snapshots[index];
            const bool changedInputAlreadyApplied =
                    snapshot.timeMs != 0 &&
                    snapshot.timeMs >= earliestChangedTimeMs;
            if (snapshot.timeMs > simulationHorizonMs ||
                changedInputAlreadyApplied) {
                break;
            }
            reusableSnapshotCount = index + 1u;
        }
        std::vector<ManualDriveRuntime::Snapshot> snapshots;
        if (reusableSnapshotCount > 0u) {
            const ManualDriveRuntime::Snapshot &snapshot =
                    runtime->snapshots[reusableSnapshotCount - 1u];
            auto restored = runtime->sandbox.RestoreState(snapshot.state);
            if (!restored) {
                result.error = QStringLiteral(
                                       "Updating the input preview failed: %1")
                                       .arg(SandboxErrorText(restored.Error()));
                return result;
            }
            initial = std::move(restored);
            result.frames.assign(
                    runtime->simulatedFrames.begin(),
                    runtime->simulatedFrames.begin() +
                            static_cast<std::ptrdiff_t>(snapshot.frameCount));
            snapshots.assign(
                    runtime->snapshots.begin(),
                    runtime->snapshots.begin() +
                            static_cast<std::ptrdiff_t>(reusableSnapshotCount));
        }
        auto replaced = runtime->sandbox.ReplaceInputs(
                std::move(baseline.events));
        if (!replaced) {
            result.error = QStringLiteral(
                                   "Updating the input preview failed: %1")
                                   .arg(SandboxErrorText(replaced.Error()));
            return result;
        }

        PhysicsSandboxStateView state = initial.Value();
        const std::uint64_t remainingDuration =
                simulationHorizonMs > state.timeMs
                ? simulationHorizonMs - state.timeMs
                : 0u;
        const std::uint64_t maximumTicks =
                remainingDuration / kViewerTickDurationMs +
                (remainingDuration % kViewerTickDurationMs == 0u ? 0u : 1u);
        if (maximumTicks >= result.frames.max_size()) {
            throw std::length_error("trajectory contains too many ticks");
        }
        result.frames.reserve(
                result.frames.size() +
                static_cast<std::size_t>(maximumTicks) + 1u);
        if (result.frames.empty()) {
            result.frames.push_back(ToViewerFrame(state));
            auto captured = runtime->sandbox.CaptureState();
            if (!captured) {
                result.error = QStringLiteral(
                                       "Updating the input preview failed: %1")
                                       .arg(SandboxErrorText(captured.Error()));
                result.frames.clear();
                return result;
            }
            snapshots.emplace_back(
                    state.timeMs, result.frames.size(),
                    std::move(captured).Value());
        }
        for (std::uint64_t tick = 0u;
             tick < maximumTicks && state.timeMs < simulationHorizonMs &&
             !state.raceCompleted;
             ++tick) {
            if (canceled()) {
                result.canceled = true;
                result.frames.clear();
                return result;
            }
            auto advanced = runtime->sandbox.AdvanceTicks(1u);
            if (!advanced) {
                result.error = QStringLiteral(
                                       "Updating the input preview failed: %1")
                                       .arg(SandboxErrorText(
                                               advanced.Error()));
                result.frames.clear();
                return result;
            }
            state = advanced.Value();
            result.frames.push_back(ToViewerFrame(state));
            if (state.timeMs % 1000u == 0u) {
                auto captured = runtime->sandbox.CaptureState();
                if (!captured) {
                    result.error = QStringLiteral(
                                           "Updating the input preview failed: %1")
                                           .arg(SandboxErrorText(
                                                   captured.Error()));
                    result.frames.clear();
                    return result;
                }
                snapshots.emplace_back(
                        state.timeMs, result.frames.size(),
                        std::move(captured).Value());
            }
        }
        if (canceled()) {
            result.canceled = true;
            result.frames.clear();
            return result;
        }
        runtime->simulatedInputs = result.inputs;
        runtime->simulatedFrames = result.frames;
        runtime->snapshots = std::move(snapshots);
        runtime->simulationHorizonMs = simulationHorizonMs;
        result.mesh = BuildTrajectoryMesh(
                result.frames, trajectoryRadius);
        if (result.mesh.filled.isEmpty()) {
            result.error = QStringLiteral(
                    "The input preview produced no viewable path.");
            result.frames.clear();
        }
    } catch (const std::exception &exception) {
        result.error = QStringLiteral("Updating the input preview failed: %1")
                               .arg(QString::fromUtf8(exception.what()));
        result.frames.clear();
    } catch (...) {
        result.error = QStringLiteral(
                "Updating the input preview failed unexpectedly.");
        result.frames.clear();
    }
    return result;
}

void AppendCheckpointTransitions(
        std::vector<RaceViewerSplit> &splits,
        std::uint32_t previousCheckpointCount,
        const RaceViewerFrame &frame) {
    for (std::uint64_t index =
                 static_cast<std::uint64_t>(
                         previousCheckpointCount) + 1u;
         index <= static_cast<std::uint64_t>(
                          frame.checkpointsCollected);
         ++index) {
        splits.push_back({
                static_cast<std::uint32_t>(index),
                frame.timeMs,
                false});
    }
    if (frame.raceCompleted &&
        std::none_of(
                splits.cbegin(),
                splits.cend(),
                [](const RaceViewerSplit &split) {
                    return split.isFinish;
                })) {
        splits.push_back({
                0u,
                frame.finishTimeMs.has_value()
                        ? static_cast<std::int64_t>(*frame.finishTimeMs)
                        : frame.timeMs,
                true});
    }
}

std::vector<RaceViewerSplit> BuildCheckpointSplits(
        const std::vector<RaceViewerFrame> &frames) {
    std::vector<RaceViewerSplit> splits;
    std::uint32_t checkpointCount = 0u;
    for (const RaceViewerFrame &frame : frames) {
        AppendCheckpointTransitions(splits, checkpointCount, frame);
        checkpointCount =
                std::max(checkpointCount, frame.checkpointsCollected);
    }
    return splits;
}

void UpdateRunPose(RaceViewerRun &run, qint64 timeMs) {
    if (run.frames.empty()) {
        run.position = {};
        run.rotation = {};
        return;
    }
    const auto upper = std::lower_bound(
            run.frames.begin(),
            run.frames.end(),
            timeMs,
            [](const RaceViewerFrame &frame, qint64 time) {
                return frame.timeMs < time;
            });
    if (upper == run.frames.begin()) {
        run.position = upper->position;
        run.rotation = upper->rotation;
        return;
    }
    if (upper == run.frames.end()) {
        run.position = run.frames.back().position;
        run.rotation = run.frames.back().rotation;
        return;
    }
    const RaceViewerFrame &after = *upper;
    const RaceViewerFrame &before = *(upper - 1);
    const qint64 interval = after.timeMs - before.timeMs;
    const float blend = interval > 0
            ? static_cast<float>(timeMs - before.timeMs) /
                    static_cast<float>(interval)
            : 0.0f;
    run.position = before.position * (1.0f - blend) +
            after.position * blend;
    run.rotation = QQuaternion::slerp(
            before.rotation, after.rotation, blend);
}

}  // namespace

RaceViewerController::RaceViewerController(QObject *parent)
    : QObject(parent), simulationDebugger_(this) {
    liveBestUpdates_ = QSettings().value(QStringLiteral("viewer/liveBestUpdates"), false).toBool();
    liveBestTimer_.setSingleShot(true);
    liveBestTimer_.setInterval(100);
    connect(&liveBestTimer_, &QTimer::timeout, this, &RaceViewerController::applyLiveBest);
    cameraPreset_ = std::clamp(
            QSettings().value(
                    QStringLiteral("viewer/cameraPreset"), 1).toInt(),
            1, 3);
    telemetryScript_ = QSettings()
            .value(QLatin1String(kTelemetryScriptKey),
                   DefaultTelemetryScript())
            .toString();
    if (telemetryScript_ == LegacyTelemetryScript())
        telemetryScript_ = DefaultTelemetryScript();
    const QJsonDocument styles = QJsonDocument::fromJson(
            QSettings().value(QLatin1String(kVisualStylesKey))
                    .toByteArray());
    if (styles.isObject()) {
        visualStyles_ = styles.toVariant().toMap();
    }
    playbackTimer_.setInterval(5);
    playbackTimer_.setTimerType(Qt::PreciseTimer);
    connect(&playbackTimer_,
            &QTimer::timeout,
            this,
            &RaceViewerController::advancePlayback);
    manualDriveTimer_.setInterval(5);
    manualDriveTimer_.setTimerType(Qt::PreciseTimer);
    connect(&manualDriveTimer_,
            &QTimer::timeout,
            this,
            &RaceViewerController::advanceManualDrive);
    connect(&simulationDebugger_,
            &SimulationDebuggerModel::frameProduced,
            this,
            &RaceViewerController::appendSimulationDebuggerFrame);
    connect(&simulationDebugger_,
            &SimulationDebuggerModel::stateChanged,
            this,
            [this]() {
                if (simulationDebugger_.active()) {
                    setPlaying(simulationDebugger_.running());
                } else if (selectedRunId_ == QStringLiteral("debug")) {
                    setPlaying(false);
                }
            });
    connect(&simulationDebugger_,
            &SimulationDebuggerModel::sessionFinished,
            this,
            [this]() {
                setPlaying(false);
                setStatusText(simulationDebugger_.statusText());
            });

    const std::vector<ViewerTriangle> ellipsoidTriangles =
            UnitEllipsoidTriangles();
    ellipsoidFilledGeometries_.reserve(kCarPaletteCount);
    for (int palette = 0; palette < kCarPaletteCount; ++palette) {
        const RaceViewerMeshBuffers ellipsoid =
                BuildMeshBuffers(ellipsoidTriangles, -2);
        auto geometry = std::make_unique<RaceGeometry>();
        geometry->setMesh(
                ellipsoid.filled,
                static_cast<int>(sizeof(FilledVertex)),
                QQuick3DGeometry::PrimitiveType::Triangles,
                true,
                ellipsoid.boundsMin,
                ellipsoid.boundsMax);
        if (palette == 0) {
            ellipsoidWireGeometry_.setMesh(
                    ellipsoid.wire,
                    static_cast<int>(sizeof(WireVertex)),
                    QQuick3DGeometry::PrimitiveType::Lines,
                    false,
                    ellipsoid.boundsMin,
                    ellipsoid.boundsMax);
        }
        ellipsoidFilledGeometries_.push_back(std::move(geometry));
    }
}

RaceViewerController::~RaceViewerController() {
    cancelRayTracingBuild();
    playbackTimer_.stop();
    manualDriveTimer_.stop();
    simulationDebugger_.stopSession();
    waitForInputPreviewWorker();
    waitForStoredRunWorker();
    waitForWorker();
}

void RaceViewerController::requestShutdown() {
    if (shuttingDown_) return;
    shuttingDown_ = true;
    endSearchPreview();
    pause();
    stopManualDrive();
    manualDriveTimer_.stop();
    simulationDebugger_.stopSession();
    queuedMapLoad_.reset();
    pendingRun_.reset();
    pendingImprovements_.clear();
    ++loadSerial_;
    cancelRayTracingBuild();
    cancelInputPreviewBuild();
    cancelStoredRunRebuilds();
    if (workerThread_ != nullptr) workerThread_->requestInterruption();
}

bool RaceViewerController::shutdownReady() const {
    for (const auto thread : {workerThread_, inputPreviewThread_, storedRunThread_})
        if (thread != nullptr && thread->isRunning()) return false;
    return simulationDebugger_.shutdownReady();
}

QQuick3DGeometry *RaceViewerController::trackFilledGeometry() {
    return &trackFilledGeometry_;
}

QQuick3DGeometry *RaceViewerController::trackWireGeometry() {
    return &trackWireGeometry_;
}

QQuick3DGeometry *RaceViewerController::ellipsoidFilledGeometry() {
    return ellipsoidFilledGeometries_.front().get();
}

QQuick3DGeometry *RaceViewerController::selectedEllipsoidFilledGeometry() {
    return ellipsoidFilledGeometryForRun(selectedRunIndex());
}

QVariantList RaceViewerController::ellipsoidFilledGeometries() const {
    QVariantList geometries;
    geometries.reserve(
            static_cast<qsizetype>(ellipsoidFilledGeometries_.size()));
    for (const auto &geometry : ellipsoidFilledGeometries_) {
        geometries.push_back(QVariant::fromValue(
                static_cast<QObject *>(geometry.get())));
    }
    return geometries;
}

QQuick3DGeometry *RaceViewerController::ellipsoidWireGeometry() {
    return &ellipsoidWireGeometry_;
}

WhiteboardModel *RaceViewerController::whiteboard() {
    return &whiteboard_;
}

SimulationDebuggerModel *RaceViewerController::simulationDebugger() {
    return &simulationDebugger_;
}

QVariantList RaceViewerController::carEllipsoids() const {
    return carEllipsoids_;
}

QVariantList RaceViewerController::visualInstances() const {
    return visualBatches_;
}

QVariantList RaceViewerController::visualBatches() const {
    return visualBatches_;
}

QVariantList RaceViewerController::visualMaterials() const {
    return visualMaterials_;
}

QVariantList RaceViewerController::trajectoryPaths() const {
    return trajectoryPaths_;
}

quint64 RaceViewerController::visualStyleRevision() const {
    return visualStyleRevision_;
}

QVariantMap RaceViewerController::visualStyle(const QString &id) const {
    return visualStyles_.value(id).toMap();
}

QVariantMap RaceViewerController::visualStylesSnapshot() const {
    return visualStyles_;
}

void RaceViewerController::restoreVisualStyles(const QVariantMap &styles) {
    if (visualStyles_ == styles) return;
    visualStyles_ = styles;
    QSettings().setValue(
            QLatin1String(kVisualStylesKey),
            QJsonDocument::fromVariant(visualStyles_).toJson(
                    QJsonDocument::Compact));
    ++visualStyleRevision_;
    emit visualStylesChanged();
    refreshStyledTrajectory(QStringLiteral("best"));
    refreshStyledTrajectory(QStringLiteral("preview"));
}

void RaceViewerController::setVisualStyle(
        const QString &id,
        const QString &property,
        const QVariant &value) {
    if (id.isEmpty() || id.size() > 160 ||
        !QList<QString>{QStringLiteral("visible"),
                        QStringLiteral("throughBlocks"),
                        QStringLiteral("color"),
                        QStringLiteral("width"),
                        QStringLiteral("opacity"),
                        QStringLiteral("linePattern")}
                 .contains(property)) {
        return;
    }
    QVariant normalized = value;
    if (property == QLatin1String("visible") ||
        property == QLatin1String("throughBlocks")) {
        normalized = value.toBool();
    } else if (property == QLatin1String("color")) {
        const QColor color(value.toString());
        if (!color.isValid()) return;
        normalized = color.name(QColor::HexRgb);
    } else if (property == QLatin1String("width") ||
               property == QLatin1String("opacity")) {
        bool okay = false;
        const double number = value.toDouble(&okay);
        if (!okay || !std::isfinite(number)) return;
        normalized = property == QLatin1String("width")
                ? std::clamp(number, 0.5, 20.0)
                : std::clamp(number, 0.0, 1.0);
    } else if (property == QLatin1String("linePattern")) {
        if (value != QLatin1String("solid") &&
            value != QLatin1String("dash")) return;
    }
    QVariantMap style = visualStyle(id);
    if (style.value(property) == normalized) return;
    style.insert(property, normalized);
    visualStyles_.insert(id, style);
    QSettings().setValue(
            QLatin1String(kVisualStylesKey),
            QJsonDocument::fromVariant(visualStyles_).toJson(
                    QJsonDocument::Compact));
    ++visualStyleRevision_;
    emit visualStylesChanged();
    if (property == QLatin1String("width") &&
        (id == QLatin1String("trajectory:best") ||
         id == QLatin1String("trajectory:preview"))) {
        refreshStyledTrajectory(id.mid(QStringLiteral("trajectory:").size()));
    }
}

qint64 RaceViewerController::trajectoryCount() const {
    return static_cast<qint64>(trajectoryPaths_.size());
}

QString RaceViewerController::previewInputScript() const {
    return previewInputScript_;
}

bool RaceViewerController::previewingHistory() const {
    return previewingHistory_;
}

void RaceViewerController::setPreviewingHistory(bool value) {
    if (previewingHistory_ == value) return;
    previewingHistory_ = value;
    emit previewingHistoryChanged();
}

qint64 RaceViewerController::simulationHorizonMs() const {
    return simulationHorizonMs_;
}

QVariantList RaceViewerController::runOptions() const {
    QVariantList options;
    options.reserve(static_cast<qsizetype>(runs_.size()));
    for (const RaceViewerRun &run : runs_) {
        QVariantMap option;
        option.insert(QStringLiteral("id"), run.id);
        option.insert(QStringLiteral("name"), run.name);
        options.push_back(std::move(option));
    }
    return options;
}

QVariantList RaceViewerController::runPoses() const {
    QVariantList poses;
    poses.reserve(static_cast<qsizetype>(runs_.size()));
    for (std::size_t index = 0u; index < runs_.size(); ++index) {
        const RaceViewerRun &run = runs_[index];
        QVariantMap pose;
        pose.insert(QStringLiteral("id"), run.id);
        pose.insert(QStringLiteral("name"), run.name);
        pose.insert(QStringLiteral("index"),
                    static_cast<qint64>(index));
        pose.insert(QStringLiteral("position"), run.position);
        pose.insert(QStringLiteral("rotation"), run.rotation);
        pose.insert(QStringLiteral("selected"),
                    run.id == selectedRunId_);
        poses.push_back(std::move(pose));
    }
    return poses;
}

qint64 RaceViewerController::runCount() const {
    return static_cast<qint64>(runs_.size());
}

QString RaceViewerController::selectedRunId() const {
    return selectedRunId_;
}

int RaceViewerController::selectedRunIndex() const {
    const auto selected = std::find_if(
            runs_.begin(), runs_.end(), [this](const RaceViewerRun &run) {
                return run.id == selectedRunId_;
            });
    return selected == runs_.end()
            ? -1
            : static_cast<int>(selected - runs_.begin());
}

QVector3D RaceViewerController::carPosition() const {
    return carPosition_;
}

QQuaternion RaceViewerController::carRotation() const {
    return carRotation_;
}

QVector3D RaceViewerController::carVelocity() const {
    const RaceViewerRun *const run = selectedRun();
    return run == nullptr ? QVector3D{}
                          : SampleTelemetryFrame(run->frames, timeMs_).linearSpeed;
}

QVector3D RaceViewerController::carHeading() const {
    return carRotation_.rotatedVector(QVector3D(0.0f, 0.0f, -1.0f));
}

QVariantList RaceViewerController::selectedRunSamples() const {
    QVariantList samples;
    const RaceViewerRun *const run = selectedRun();
    if (run == nullptr) return samples;
    samples.reserve(static_cast<qsizetype>(run->frames.size()));
    for (const RaceViewerFrame &frame : run->frames) {
        QVariantMap sample;
        sample.insert(QStringLiteral("timeMs"),
                      static_cast<qlonglong>(frame.timeMs));
        sample.insert(QStringLiteral("position"), frame.position);
        sample.insert(QStringLiteral("rotation"), frame.rotation);
        sample.insert(QStringLiteral("velocity"), frame.linearSpeed);
        sample.insert(QStringLiteral("checkpointsCollected"),
                      frame.checkpointsCollected);
        sample.insert(QStringLiteral("raceCompleted"), frame.raceCompleted);
        if (frame.finishTimeMs) {
            sample.insert(QStringLiteral("finishTimeMs"),
                          *frame.finishTimeMs);
        }
        if (frame.stuntsScore) {
            sample.insert(QStringLiteral("stuntsScore"), *frame.stuntsScore);
        }
        samples.push_back(std::move(sample));
    }
    return samples;
}

QVariantList RaceViewerController::projectSelectedRunSamples(
        const QVector3D &cameraPosition, const QQuaternion &cameraRotation,
        double fieldOfView, double width, double height, double clipNear) const {
    QVariantList samples;
    const RaceViewerRun *const run = selectedRun();
    if (run == nullptr || width <= 0 || height <= 0 ||
        fieldOfView <= 0 || fieldOfView >= 180) return samples;

    // The viewer uses an unscaled, vertical-FOV perspective camera. Build its
    // transform once, rather than asking View3D to invert it for every tick.
    QMatrix4x4 view;
    view.rotate(cameraRotation.conjugated());
    view.translate(-cameraPosition);
    QMatrix4x4 projection;
    projection.perspective(static_cast<float>(fieldOfView),
                           static_cast<float>(width / height), 0.01f, 1.0f);
    const QMatrix4x4 transform = projection * view;
    samples.reserve(static_cast<qsizetype>(run->frames.size()));
    for (const RaceViewerFrame &frame : run->frames) {
        const QVector4D point = transform * QVector4D(frame.position, 1.0f);
        QVariantMap sample{{QStringLiteral("timeMs"),
                            static_cast<qlonglong>(frame.timeMs)}};
        if (point.w() > clipNear) {
            sample.insert(QStringLiteral("point"),
                          QPointF((point.x() / point.w() + 1.0) * width * 0.5,
                                  (1.0 - point.y() / point.w()) * height * 0.5));
        }
        samples.push_back(std::move(sample));
    }
    return samples;
}

QQuick3DGeometry *RaceViewerController::timeRangeGeometry(
        const QString &id, const QString &runId,
        qint64 minimumMs, qint64 maximumMs, bool dashed) {
    if (maximumMs < minimumMs) return nullptr;
    const auto run = std::find_if(
            runs_.begin(), runs_.end(), [&runId](const RaceViewerRun &entry) {
                return entry.id == runId;
            });
    if (run == runs_.end() || run->frames.empty()) return nullptr;
    const QString key = runId + QLatin1Char(':') + id;
    const qint64 start = std::clamp<qint64>(
            minimumMs, run->frames.front().timeMs,
            run->frames.back().timeMs);
    const qint64 end = std::clamp<qint64>(
            maximumMs, start, run->frames.back().timeMs);
    const auto cached = timeRangeCacheStates_.find(key);
    const auto geometryIt = timeRangeGeometries_.find(key);
    if (cached != timeRangeCacheStates_.end() &&
        geometryIt != timeRangeGeometries_.end() && geometryIt->second &&
        cached->second.start == start && cached->second.end == end &&
        cached->second.dashed == dashed &&
        cached->second.runRevision == runGeometryRevision_) {
        return geometryIt->second.get();
    }
    std::vector<RaceViewerFrame> frames;
    frames.push_back(SampleTelemetryFrame(run->frames, start));
    for (const RaceViewerFrame &frame : run->frames) {
        if (frame.timeMs > start && frame.timeMs < end)
            frames.push_back(frame);
    }
    if (end > start)
        frames.push_back(SampleTelemetryFrame(run->frames, end));
    const RaceViewerMeshBuffers mesh = BuildTrajectoryLineMesh(
            frames, static_cast<float>(
                    std::clamp(sceneRadius_ * 0.0008, 0.03, 0.3)), dashed);
    if (mesh.wire.isEmpty()) return nullptr;
    auto &geometry = timeRangeGeometries_[key];
    if (!geometry) {
        geometry = std::make_unique<RaceGeometry>();
        QQmlEngine::setObjectOwnership(geometry.get(), QQmlEngine::CppOwnership);
    }
    geometry->setMesh(mesh.wire,
                      static_cast<int>(sizeof(WireVertex)),
                      QQuick3DGeometry::PrimitiveType::Lines,
                      false,
                      mesh.boundsMin,
                      mesh.boundsMax);
    timeRangeCacheStates_[key] = {start, end, dashed, runGeometryRevision_};
    return geometry.get();
}

QQuick3DGeometry *RaceViewerController::visualLineGeometry(
        const QString &id, const QVariantList &points, bool closed) {
    if (id.isEmpty() || points.size() < 2 || points.size() > 64)
        return nullptr;
    std::vector<WireVertex> vertices;
    vertices.reserve(static_cast<std::size_t>(points.size() * 2));
    QVector3D minimum;
    QVector3D maximum;
    for (qsizetype index = 0; index < points.size(); ++index) {
        if (!closed && index == points.size() - 1) break;
        const QVector3D start = points[index].value<QVector3D>();
        const QVector3D end = points[(index + 1) % points.size()]
                                      .value<QVector3D>();
        if (index == 0) minimum = maximum = start;
        ExpandBounds(start, minimum, maximum);
        ExpandBounds(end, minimum, maximum);
        vertices.push_back({start.x(), start.y(), start.z()});
        vertices.push_back({end.x(), end.y(), end.z()});
    }
    QByteArray bytes;
    bytes.resize(static_cast<qsizetype>(
            vertices.size() * sizeof(WireVertex)));
    std::memcpy(bytes.data(), vertices.data(),
                static_cast<std::size_t>(bytes.size()));
    auto &geometry = timeRangeGeometries_[QStringLiteral("visual:") + id];
    if (!geometry) {
        geometry = std::make_unique<RaceGeometry>();
        QQmlEngine::setObjectOwnership(geometry.get(), QQmlEngine::CppOwnership);
    }
    geometry->setMesh(std::move(bytes),
                      static_cast<int>(sizeof(WireVertex)),
                      QQuick3DGeometry::PrimitiveType::Lines,
                      false, minimum, maximum);
    return geometry.get();
}

QVariantMap RaceViewerController::conditionPreview(
        const QString &script) const {
    if (script.trimmed().isEmpty()) return {};
    const auto compiled = CompileConditionScript(script.toStdString());
    if (!compiled.program && !compiled.error) return {};
    if (!compiled.program) {
        return {{QStringLiteral("available"), false},
                {QStringLiteral("message"),
                 QString::fromStdString(compiled.error.value_or(
                         "Condition cannot be parsed"))}};
    }
    using Value = forevervalidator::experimental::
            PhysicsSandboxCudaConditionValue;
    using Opcode = forevervalidator::experimental::
            PhysicsSandboxCudaConditionOpcode;
    const auto &instructions = compiled.program->cuda.instructions;
    for (const auto &instruction : instructions) {
        if (instruction.opcode != Opcode::Scalar &&
            instruction.opcode != Opcode::Vector) {
            continue;
        }
        switch (instruction.value) {
        case Value::Position:
        case Value::PreviousPosition:
        case Value::Velocity:
        case Value::PreviousVelocity:
        case Value::Yaw:
        case Value::Pitch:
        case Value::Roll:
        case Value::PreviousYaw:
        case Value::PreviousPitch:
        case Value::PreviousRoll:
        case Value::Speed:
        case Value::PreviousSpeed:
        case Value::CheckpointCount:
        case Value::CompletedLaps:
        case Value::SimulationTimeMilliseconds:
        case Value::WheelGroundContact0:
        case Value::WheelGroundContact1:
        case Value::WheelGroundContact2:
        case Value::WheelGroundContact3:
            break;
        default:
            return {{QStringLiteral("available"), false},
                    {QStringLiteral("message"), QStringLiteral(
                         "Condition uses telemetry not stored in this run")}};
        }
    }
    const RaceViewerRun *const run = selectedRun();
    if (run == nullptr || run->frames.empty()) {
        return {{QStringLiteral("available"), false},
                {QStringLiteral("message"), QStringLiteral(
                     "No selected run to preview")}};
    }
    const RaceViewerFrame current =
            SampleTelemetryFrame(run->frames, timeMs_);
    const RaceViewerFrame previous = SampleTelemetryFrame(
            run->frames, std::max<qint64>(0, timeMs_ - kViewerTickDurationMs));
    const auto toState = [](const RaceViewerFrame &frame) {
        forevervalidator::experimental::PhysicsSandboxStateView state;
        state.timeMs = static_cast<std::uint64_t>(frame.timeMs);
        state.car.position = {frame.position.x(), frame.position.y(),
                              frame.position.z()};
        state.car.linearSpeed = {frame.linearSpeed.x(),
                                 frame.linearSpeed.y(),
                                 frame.linearSpeed.z()};
        state.car.rotationX = frame.rotation.x();
        state.car.rotationY = frame.rotation.y();
        state.car.rotationZ = frame.rotation.z();
        state.car.rotationW = frame.rotation.scalar();
        state.car.wheelContact = frame.wheelContact;
        state.checkpointsCollected = frame.checkpointsCollected;
        state.completedLaps = frame.completedLaps;
        return state;
    };
    const bool passed = compiled.program->Evaluate(
            toState(previous), toState(current),
            ConditionExecutionContext{
                    0u, 0.0, 0.0, static_cast<double>(timeMs_) / 1000.0});
    QStringList readouts;
    for (const auto &instruction : instructions) {
        if (instruction.opcode != Opcode::Scalar &&
            instruction.opcode != Opcode::Vector) {
            continue;
        }
        if (instruction.value == Value::Position ||
            instruction.value == Value::PreviousPosition) {
            const bool prior = instruction.value == Value::PreviousPosition;
            const QVector3D position = prior ? previous.position
                                             : current.position;
            const QString prefix = prior ? QStringLiteral("Prev ") : QString();
            const int component = static_cast<int>(instruction.x);
            if (component >= 1 && component <= 3) {
                const char axis = "XYZ"[component - 1];
                const float value = component == 1 ? position.x()
                        : component == 2 ? position.y() : position.z();
                readouts.push_back(QStringLiteral("%1%2 %3 m")
                                           .arg(prefix)
                                           .arg(QChar(axis))
                                           .arg(value, 0, 'f', 1));
            } else {
                readouts.push_back(QStringLiteral("%1position %2, %3, %4 m")
                                           .arg(prefix)
                                           .arg(position.x(), 0, 'f', 1)
                                           .arg(position.y(), 0, 'f', 1)
                                           .arg(position.z(), 0, 'f', 1));
            }
        } else if (instruction.value == Value::Speed ||
                   instruction.value == Value::PreviousSpeed) {
            const bool prior = instruction.value == Value::PreviousSpeed;
            const float speed = (prior ? previous.linearSpeed
                                       : current.linearSpeed).length() * 3.6f;
            readouts.push_back(QStringLiteral("%1%2 km/h")
                                       .arg(prior ? QStringLiteral("Prev ")
                                                  : QString())
                                       .arg(speed, 0, 'f', 1));
        } else if (instruction.value == Value::CheckpointCount) {
            readouts.push_back(QStringLiteral("%1 checkpoints")
                                       .arg(current.checkpointsCollected));
        } else if (instruction.value == Value::CompletedLaps) {
            readouts.push_back(QStringLiteral("%1 completed laps")
                                       .arg(current.completedLaps));
        } else if (instruction.value == Value::SimulationTimeMilliseconds) {
            readouts.push_back(QStringLiteral("%1 ms").arg(current.timeMs));
        } else if (instruction.value == Value::Velocity ||
                   instruction.value == Value::PreviousVelocity) {
            const bool prior = instruction.value == Value::PreviousVelocity;
            const QVector3D velocity = prior ? previous.linearSpeed
                                             : current.linearSpeed;
            readouts.push_back(QStringLiteral("%1velocity %2, %3, %4 m/s")
                                       .arg(prior ? QStringLiteral("Prev ")
                                                  : QString())
                                       .arg(velocity.x(), 0, 'f', 1)
                                       .arg(velocity.y(), 0, 'f', 1)
                                       .arg(velocity.z(), 0, 'f', 1));
        } else if (instruction.value >= Value::Yaw &&
                   instruction.value <= Value::PreviousRoll) {
            const int raw = static_cast<int>(instruction.value);
            const bool prior = raw >= static_cast<int>(Value::PreviousYaw);
            const int component = prior
                    ? raw - static_cast<int>(Value::PreviousYaw)
                    : raw - static_cast<int>(Value::Yaw);
            const QQuaternion rotation = prior ? previous.rotation
                                               : current.rotation;
            const double x = rotation.x();
            const double y = rotation.y();
            const double z = rotation.z();
            const double w = rotation.scalar();
            const double sinPitch = 2.0 * (w * x - y * z);
            const double radians = component == 0
                    ? std::atan2(2.0 * (w * y + x * z),
                                 1.0 - 2.0 * (x * x + y * y))
                    : component == 1
                    ? std::asin(std::clamp(sinPitch, -1.0, 1.0))
                    : std::atan2(2.0 * (w * z + x * y),
                                 1.0 - 2.0 * (x * x + z * z));
            const double angle = radians * 180.0 / 3.14159265358979323846;
            readouts.push_back(QStringLiteral("%1%2 %3°")
                                       .arg(prior ? QStringLiteral("Prev ")
                                                  : QString())
                                       .arg(QStringList{
                                               QStringLiteral("yaw"),
                                               QStringLiteral("pitch"),
                                               QStringLiteral("roll")}
                                                    .at(component))
                                       .arg(angle, 0, 'f', 1));
        } else if (instruction.value >= Value::WheelGroundContact0 &&
                   instruction.value <= Value::WheelGroundContact3) {
            const int wheel = static_cast<int>(instruction.value) -
                    static_cast<int>(Value::WheelGroundContact0);
            readouts.push_back(QStringLiteral("Wheel %1 %2")
                                       .arg(wheel + 1)
                                       .arg(current.wheelContact[wheel]
                                                    ? QStringLiteral("grounded")
                                                    : QStringLiteral("airborne")));
        }
    }
    readouts.removeDuplicates();
    QVariantMap result{
            {QStringLiteral("available"), true},
            {QStringLiteral("passed"), passed},
            {QStringLiteral("message"),
             QStringLiteral("%1 · %2")
                     .arg(passed ? QStringLiteral("Pass")
                                 : QStringLiteral("Fail"))
                     .arg(readouts.join(QStringLiteral(" · ")))}};
    if (instructions.size() == 3 &&
        instructions[0].opcode == Opcode::Scalar &&
        instructions[0].value == Value::Position &&
        instructions[1].opcode == Opcode::Constant &&
        (instructions[2].opcode == Opcode::Greater ||
         instructions[2].opcode == Opcode::GreaterOrEqual ||
         instructions[2].opcode == Opcode::Less ||
         instructions[2].opcode == Opcode::LessOrEqual)) {
        result.insert(QStringLiteral("thresholdAxis"),
                      static_cast<int>(instructions[0].x));
        result.insert(QStringLiteral("thresholdValue"), instructions[1].x);
    }
    return result;
}

int RaceViewerController::cameraPreset() const {
    return cameraPreset_;
}

bool RaceViewerController::carCameraAvailable() const {
    return carCameraAvailable_;
}

QVector3D RaceViewerController::carCameraPosition() const {
    return carCameraPosition_;
}

QQuaternion RaceViewerController::carCameraRotation() const {
    return carCameraRotation_;
}

QVector3D RaceViewerController::carCameraTarget() const {
    return carCameraTarget_;
}

double RaceViewerController::carCameraFieldOfView() const {
    return carCameraFieldOfView_;
}

bool RaceViewerController::hideSelectedCar() const {
    return carCameraAvailable_ && cameraPreset_ == 3;
}

QString RaceViewerController::telemetryScript() const {
    return telemetryScript_;
}

QString RaceViewerController::defaultTelemetryScript() const {
    return DefaultTelemetryScript();
}

void RaceViewerController::setTelemetryScript(const QString &value) {
    if (telemetryScript_ == value) {
        return;
    }
    telemetryScript_ = value;
    QSettings().setValue(QLatin1String(kTelemetryScriptKey), value);
    emit telemetryScriptChanged();
}

QString RaceViewerController::renderTelemetry(
        const QString &script,
        const QVector3D &cameraPosition) const {
    const RaceViewerRun *const run = selectedRun();
    const RaceViewerFrame frame = run == nullptr
            ? RaceViewerFrame{}
            : SampleTelemetryFrame(run->frames, timeMs_);
    const TelemetryRenderResult result = RenderTelemetryTemplate(
            script,
            cameraPosition,
            frame,
            run == nullptr ? QString{} : run->name,
            currentTick());
    return result.error.isEmpty()
            ? result.text
            : QStringLiteral("Telemetry: %1").arg(result.error);
}

QString RaceViewerController::renderTelemetryWithContext(
        const QString &script,
        const QVector3D &cameraPosition,
        const QVariantMap &context,
        bool richText) const {
    const RaceViewerRun *const run = selectedRun();
    const RaceViewerFrame frame = run == nullptr
            ? RaceViewerFrame{}
            : SampleTelemetryFrame(run->frames, timeMs_);
    const TelemetryRenderResult result = RenderTelemetryTemplate(
            script, cameraPosition, frame,
            run == nullptr ? QString{} : run->name,
            currentTick(), context, richText);
    return result.error.isEmpty()
            ? result.text
            : QStringLiteral("Telemetry: %1").arg(result.error);
}

QString RaceViewerController::telemetryScriptError(
        const QString &script) const {
    const RaceViewerRun *const run = selectedRun();
    const RaceViewerFrame frame = run == nullptr
            ? RaceViewerFrame{}
            : SampleTelemetryFrame(run->frames, timeMs_);
    return RenderTelemetryTemplate(
                   script,
                   {},
                   frame,
                   run == nullptr ? QString{} : run->name,
                   currentTick())
            .error;
}

qint64 RaceViewerController::durationMs() const {
    return durationMs_;
}

qint64 RaceViewerController::timelineSeekLimitMs() const {
    const RaceViewerRun *const run = selectedRun();
    return run == nullptr || run->frames.empty()
            ? 0
            : std::clamp<qint64>(
                      run->frames.back().timeMs, 0, durationMs_);
}

qint64 RaceViewerController::timeMs() const {
    return timeMs_;
}

qint64 RaceViewerController::currentTick() const {
    const RaceViewerRun *const run = selectedRun();
    if (run == nullptr || run->frames.empty()) {
        return 0;
    }
    return std::clamp<qint64>(
            timeMs_ / static_cast<qint64>(kViewerTickDurationMs),
            0,
            tickCount() - 1);
}

qint64 RaceViewerController::tickCount() const {
    const RaceViewerRun *const run = selectedRun();
    return run == nullptr
            ? 0
            : static_cast<qint64>(run->frames.size());
}

int RaceViewerController::tickDurationMs() const {
    return static_cast<int>(kViewerTickDurationMs);
}

QString RaceViewerController::timeText() const {
    return FormatTime(timeMs_) + QStringLiteral(" / ") +
            FormatTime(durationMs_);
}

QVariantList RaceViewerController::checkpointSplits() const {
    QVariantList result;
    const RaceViewerRun *const run = selectedRun();
    if (run == nullptr) {
        return result;
    }
    result.reserve(static_cast<qsizetype>(run->checkpointSplits.size()));
    for (const RaceViewerSplit &split : run->checkpointSplits) {
        if (split.timeMs > timeMs_) {
            break;
        }
        QVariantMap item;
        item.insert(
                QStringLiteral("label"),
                split.isFinish
                        ? tr("Finish")
                        : tr("CP %1").arg(split.index));
        item.insert(
                QStringLiteral("time"),
                QString::fromStdString(
                        FormatFixedSplitMilliseconds(
                                static_cast<std::uint64_t>(
                                        std::max<std::int64_t>(
                                                0, split.timeMs)))));
        item.insert(
                QStringLiteral("timeMs"),
                static_cast<qlonglong>(split.timeMs));
        item.insert(QStringLiteral("isFinish"), split.isFinish);
        item.insert(
                QStringLiteral("index"),
                static_cast<qulonglong>(split.index));
        result.push_back(std::move(item));
    }
    return result;
}

bool RaceViewerController::playing() const {
    return playing_;
}

bool RaceViewerController::takeOverOnInput() const {
    return takeOverOnInput_;
}

bool RaceViewerController::manualDriving() const {
    return manualDriving_;
}

bool RaceViewerController::manualSteeringTakenOver() const {
    return manualTakeover_ && steeringTakeoverTimeMs_.has_value();
}

bool RaceViewerController::manualLongitudinalTakenOver() const {
    return manualTakeover_ && longitudinalTakeoverTimeMs_.has_value();
}

bool RaceViewerController::manualLeft() const {
    return manualLeft_;
}

bool RaceViewerController::manualRight() const {
    return manualRight_;
}

bool RaceViewerController::manualAccelerate() const {
    return manualAccelerate_;
}

bool RaceViewerController::manualBrake() const {
    return manualBrake_;
}

bool RaceViewerController::canCopyCurrentInputs() const {
    const RaceViewerRun *const run = selectedRun();
    if (run == nullptr) {
        return false;
    }
    return run->id == QStringLiteral("manual")
            ? manualRuntime_ != nullptr
            : !run->inputs.empty();
}

bool RaceViewerController::loaded() const {
    return loaded_;
}

bool RaceViewerController::loading() const {
    return loading_;
}

QString RaceViewerController::statusText() const {
    return statusText_;
}

qint64 RaceViewerController::triangleCount() const {
    return triangleCount_;
}

qint64 RaceViewerController::visualTriangleCount() const {
    return visualTriangleCount_;
}

qint64 RaceViewerController::visualMeshCount() const {
    return sourceVisualMeshCount_;
}

qint64 RaceViewerController::visualBatchCount() const {
    return static_cast<qint64>(visualGeometries_.size());
}

qint64 RaceViewerController::sourceVisualObjectCount() const {
    return sourceVisualObjectCount_;
}

qint64 RaceViewerController::shadowCount() const { return 0; }

qint64 RaceViewerController::materialCount() const {
    return materialCount_;
}

qint64 RaceViewerController::diagnosticCount() const {
    return diagnosticCount_;
}

qint64 RaceViewerController::ellipsoidCount() const {
    return carEllipsoids_.size();
}

double RaceViewerController::sceneRadius() const { return sceneRadius_; }

QVector3D RaceViewerController::sceneBoundsMin() const {
    return sceneBoundsMin_;
}

QVector3D RaceViewerController::sceneBoundsMax() const {
    return sceneBoundsMax_;
}

std::shared_ptr<const RayTracingSceneData>
RaceViewerController::rayTracingScene() const {
    return rayTracingScene_;
}

void RaceViewerController::cancelRayTracingBuild() {
    ++rayTracingGeneration_;
    if (rayTracingCancelled_) rayTracingCancelled_->store(true);
    rayTracingCancelled_.reset();
}

void RaceViewerController::requestRayTracingScene() {
    if (shuttingDown_) return;
    if (!loaded_ || loading_ || rayTracingScene_ || rayTracingCancelled_ ||
        rayTracingSourceBatches_.empty()) return;
    using Result = std::pair<std::shared_ptr<const RayTracingSceneData>, QString>;
    auto *watcher = new QFutureWatcher<Result>(this);
    const auto generation = rayTracingGeneration_;
    const auto cancelled = std::make_shared<std::atomic_bool>(false);
    rayTracingCancelled_ = cancelled;
    connect(watcher, &QFutureWatcher<Result>::finished, this,
            [this, watcher, generation]() {
        const Result result = watcher->result();
        watcher->deleteLater();
        if (generation != rayTracingGeneration_) return;
        rayTracingCancelled_.reset();
        if (!result.second.isEmpty()) {
            setStatusText(tr("Ray-tracing scene failed: %1").arg(result.second));
            return;
        }
        rayTracingScene_ = result.first;
        emit rayTracingSceneChanged();
    });
    // QByteArray copies share the immutable raster buffers; no second vertex
    // allocation is made until ray tracing is explicitly requested.
    watcher->setFuture(QtConcurrent::run(
            [batches = rayTracingSourceBatches_, cancelled]() -> Result {
        try {
            return {BuildRayTracingScene(batches, cancelled.get()), {}};
        } catch (const std::exception &error) {
            return {nullptr, QString::fromUtf8(error.what())};
        }
    }));
}

QVector2D
RaceViewerController::cameraClipPlanes(const QVector3D &cameraPosition,
                                       double cameraDistance) const {
    const CameraClipPlanes planes = CalculateCameraClipPlanes(
            cameraPosition, static_cast<float>(cameraDistance), sceneBoundsMin_,
            sceneBoundsMax_);
    return {planes.nearPlane, planes.farPlane};
}

RaceViewerInputSample RaceViewerController::inputSample(qint64 tick) const
        noexcept {
    const RaceViewerRun *const run = selectedRun();
    if (run == nullptr || tick < 0 ||
        tick >= static_cast<qint64>(run->frames.size())) {
        return {};
    }
    const RaceViewerFrame &frame =
            run->frames[static_cast<std::size_t>(tick)];
    return {frame.accelerate, frame.brake, frame.steering};
}

void RaceViewerController::setLiveBestUpdates(bool value) {
    if (liveBestUpdates_ == value) return;
    liveBestUpdates_ = value;
    QSettings().setValue(QStringLiteral("viewer/liveBestUpdates"), value);
    if (!value) {
        liveBestTimer_.stop();
        pendingLiveBest_.reset();
    }
    emit liveBestUpdatesChanged();
}

void RaceViewerController::beginSearchPreview(std::uint64_t searchId) {
    if (shuttingDown_) return;
    endSearchPreview();
    liveSearchId_ = searchId;
    liveImprovementNumber_ = 0;
    autoSelectLiveBest_ = true;
}

void RaceViewerController::endSearchPreview() {
    liveBestTimer_.stop();
    pendingLiveBest_.reset();
    liveSearchId_ = 0;
}

void RaceViewerController::queueLiveBest(std::shared_ptr<const app::SearchImprovement> improvement) {
    if (!liveBestUpdates_ || !improvement || liveSearchId_ == 0 ||
        improvement->searchId != liveSearchId_ ||
        improvement->improvementNumber <= liveImprovementNumber_ ||
        improvement->timeline.empty()) return;
    liveImprovementNumber_ = improvement->improvementNumber;
    pendingLiveBest_ = std::move(improvement);
    if (!liveBestTimer_.isActive()) liveBestTimer_.start();
}

void RaceViewerController::applyLiveBest() {
    auto improvement = std::move(pendingLiveBest_);
    if (!improvement || !liveBestUpdates_ || improvement->searchId != liveSearchId_ ||
        !loaded_ || loading_ || previewingHistory_ || manualDriving_ ||
        simulationDebugger_.active() || improvement->packsDirectory != loadedPacksDirectory_ ||
        improvement->replayPath != loadedReplayPath_) return;
    auto frames = ToViewerFrames(improvement->timeline);
    if (!IsViewableTrajectory(frames)) return;
    // Changing the live pose must not pause playback, rewind, or reset the camera.
    if (autoSelectLiveBest_ && selectedRunId_ != QStringLiteral("best")) {
        selectedRunId_ = QStringLiteral("best");
        emit selectedRunChanged();
    }
    upsertRun(QStringLiteral("best"), QStringLiteral("Best"), std::move(frames),
              improvement->inputs, false);
}

void RaceViewerController::completeSearchPreview(std::shared_ptr<const app::SearchCompletion> completion) {
    if (!completion || liveSearchId_ == 0 || completion->searchId != liveSearchId_) return;
    const bool select = autoSelectLiveBest_ && !previewingHistory_ && !manualDriving_ &&
                        !simulationDebugger_.active();
    endSearchPreview();
    applySearchRun(completion->packsDirectory, completion->replayPath,
                   completion->bestTimeline, completion->bestInputs,
                   completion->simulationBackendId, select, select);
}

void RaceViewerController::addSearchRun(
        const QString &packsDirectory,
        const QString &replayPath,
        const std::vector<SearchTimelineFrame> &frames) {
    addSearchRun(packsDirectory,
                 replayPath,
                 frames,
                 std::vector<SandboxInputEvent>{},
                 QStringLiteral("optimized-cpu"));
}

void RaceViewerController::addSearchRun(
        const QString &packsDirectory,
        const QString &replayPath,
        const std::vector<SearchTimelineFrame> &frames,
        const QString &backendId) {
    addSearchRun(packsDirectory,
                 replayPath,
                 frames,
                 std::vector<SandboxInputEvent>{},
                 backendId);
}

void RaceViewerController::addSearchRun(
        const QString &packsDirectory,
        const QString &replayPath,
        const std::vector<SearchTimelineFrame> &frames,
        const std::vector<SandboxInputEvent> &inputs) {
    addSearchRun(packsDirectory,
                 replayPath,
                 frames,
                 inputs,
                 QStringLiteral("optimized-cpu"));
}

void RaceViewerController::addSearchRun(
        const QString &packsDirectory,
        const QString &replayPath,
        const std::vector<SearchTimelineFrame> &frames,
        const std::vector<SandboxInputEvent> &inputs,
        const QString &backendId) {
    applySearchRun(packsDirectory, replayPath, frames, inputs, backendId, true, true);
}

void RaceViewerController::applySearchRun(
        const QString &packsDirectory, const QString &replayPath,
        const std::vector<SearchTimelineFrame> &frames,
        const std::vector<SandboxInputEvent> &inputs, const QString &backendId,
        bool select, bool loadIfNeeded) {
    if (shuttingDown_) return;
    if (select) stopManualDrive();
    if (frames.empty()) {
        setStatusText(QStringLiteral("Best run produced no viewable frames."));
        return;
    }
    const std::optional<PhysicsBackend> backend =
            ParsePhysicsBackend(backendId.toStdString());
    if (!backend) {
        setStatusText(QStringLiteral("Select a valid physics backend."));
        return;
    }
    PendingRun pending{
            packsDirectory,
            replayPath,
            kAuxiliarySimulationBackend,
            ToViewerFrames(frames),
            inputs, select};
    if (loaded_ && loadedPacksDirectory_ == packsDirectory &&
        loadedReplayPath_ == replayPath) {
        upsertRun(QStringLiteral("best"),
                  QStringLiteral("Best"),
                  std::move(pending.frames),
                  std::move(pending.inputs),
                  select);
        scheduleStoredRunRebuilds();
        setStatusText(QStringLiteral("Best run added"));
        return;
    }
    pendingRun_ = std::move(pending);
    if (loadIfNeeded && workerThread_ == nullptr) {
        beginMapLoad(packsDirectory, replayPath,
                     kAuxiliarySimulationBackend);
    }
}

void RaceViewerController::addSearchImprovement(
        const QString &packsDirectory,
        const QString &replayPath,
        const std::vector<SearchTimelineFrame> &frames,
        const QString &backendId,
        std::uint64_t searchId,
        std::uint64_t improvementNumber,
        std::chrono::steady_clock::time_point generatedAt) {
    if (shuttingDown_ || generatedAt <= previewClearedAt_) return;
    if (frames.size() > kMaximumLiveTrajectoryBytes / sizeof(RaceViewerFrame)) {
        setStatusText(QStringLiteral("Live trajectory exceeds the 64 MiB preview budget; saved results are unchanged."));
        return;
    }
    if (searchId == 0u || improvementNumber == 0u ||
        frames.empty()) {
        setStatusText(QStringLiteral(
                "Search improvement produced no viewable trajectory."));
        return;
    }
    const std::optional<PhysicsBackend> backend =
            ParsePhysicsBackend(backendId.toStdString());
    if (!backend) {
        setStatusText(QStringLiteral("Select a valid physics backend."));
        return;
    }

    try {
        PendingImprovement pending{
                packsDirectory,
                replayPath,
                kAuxiliarySimulationBackend,
                searchId,
                improvementNumber,
                ToViewerFrames(frames)};
        if (!IsViewableTrajectory(pending.frames)) {
            setStatusText(QStringLiteral(
                    "Search improvement produced an invalid trajectory."));
            return;
        }
        const QString key =
                QStringLiteral("improvement:%1:%2")
                        .arg(searchId)
                        .arg(improvementNumber);
        if (std::find(trajectoryKeys_.begin(),
                      trajectoryKeys_.end(),
                      key) != trajectoryKeys_.end()) {
            return;
        }
        const auto queued = std::find_if(
                pendingImprovements_.begin(),
                pendingImprovements_.end(),
                [searchId, improvementNumber](
                        const PendingImprovement &entry) {
                    return entry.searchId == searchId &&
                            entry.improvementNumber ==
                                    improvementNumber;
                });
        if (queued != pendingImprovements_.end()) {
            return;
        }
        if (loaded_ &&
            loadedPacksDirectory_ == packsDirectory &&
            loadedReplayPath_ == replayPath) {
            appendImprovementTrajectory(
                    searchId,
                    improvementNumber,
                    pending.frames);
            return;
        }
        while (!pendingImprovements_.empty() &&
               (pendingImprovements_.size() >= kMaximumLiveTrajectories ||
                pendingImprovementBytes() + pending.frames.size() * sizeof(RaceViewerFrame) >
                        kMaximumLiveTrajectoryBytes)) {
            pendingImprovements_.erase(pendingImprovements_.begin());
        }
        pendingImprovements_.push_back(std::move(pending));
        if (workerThread_ == nullptr) {
            beginMapLoad(packsDirectory, replayPath,
                         kAuxiliarySimulationBackend);
        }
    } catch (const std::exception &exception) {
        setStatusText(
                QStringLiteral(
                        "Adding search improvement trajectory failed: %1")
                        .arg(QString::fromUtf8(exception.what())));
    } catch (...) {
        setStatusText(QStringLiteral(
                "Adding search improvement trajectory failed unexpectedly."));
    }
}

quint64 RaceViewerController::retainedImprovementBytes() const {
    quint64 bytes = 0;
    for (const auto &geometry : trajectoryGeometries_) bytes += geometry->vertexData().size();
    return bytes;
}

quint64 RaceViewerController::pendingImprovementBytes() const {
    quint64 bytes = 0;
    for (const auto &pending : pendingImprovements_) bytes += pending.frames.size() * sizeof(RaceViewerFrame);
    return bytes;
}

void RaceViewerController::evictOldestImprovement() {
    if (trajectoryKeys_.empty()) return;
    const auto key = trajectoryKeys_.front();
    trajectoryPaths_.erase(std::remove_if(trajectoryPaths_.begin(), trajectoryPaths_.end(),
            [&key](const QVariant &entry) {
                return entry.toMap().value(QStringLiteral("visualId")).toString() == key;
            }), trajectoryPaths_.end());
    auto geometry = std::move(trajectoryGeometries_.front());
    trajectoryGeometries_.erase(trajectoryGeometries_.begin());
    trajectoryKeys_.erase(trajectoryKeys_.begin());
    emit trajectoriesChanged();
}

bool RaceViewerController::appendImprovementTrajectory(
        std::uint64_t searchId,
        std::uint64_t improvementNumber,
        const std::vector<RaceViewerFrame> &frames) {
    const QString key =
            QStringLiteral("improvement:%1:%2")
                    .arg(searchId)
                    .arg(improvementNumber);
    if (std::find(trajectoryKeys_.begin(),
                  trajectoryKeys_.end(),
                  key) != trajectoryKeys_.end()) {
        return true;
    }

    try {
        const auto maximumVertices = std::max<std::size_t>(6, frames.empty() ? 0 : (frames.size() - 1) * 2);
        if (maximumVertices > kMaximumLiveTrajectoryBytes / sizeof(WireVertex)) return false;
        while (!trajectoryKeys_.empty() &&
               (trajectoryKeys_.size() >= kMaximumLiveTrajectories ||
                retainedImprovementBytes() + maximumVertices * sizeof(WireVertex) >
                        kMaximumLiveTrajectoryBytes)) evictOldestImprovement();
        const float radius = static_cast<float>(
                std::clamp(sceneRadius_ * 0.0004, 0.015, 0.15));
        RaceViewerMeshBuffers mesh =
                BuildTrajectoryLineMesh(frames, radius * 2.0f);
        if (mesh.wire.isEmpty()) {
            setStatusText(QStringLiteral(
                    "Search improvement produced no viewable trajectory."));
            return false;
        }
        auto geometry = std::make_unique<RaceGeometry>();
        geometry->setMesh(
                std::move(mesh.wire),
                static_cast<int>(sizeof(WireVertex)),
                QQuick3DGeometry::PrimitiveType::Lines,
                false,
                mesh.boundsMin,
                mesh.boundsMax);

        QVariantList paths = trajectoryPaths_;
        for (QVariant &entry : paths) {
            QVariantMap path = entry.toMap();
            if (path.value(QStringLiteral("kind")).toString() ==
                QStringLiteral("improvement")) {
                path.insert(QStringLiteral("opacity"), 0.3);
                entry = std::move(path);
            }
        }
        QVariantMap path;
        path.insert(QStringLiteral("kind"),
                    QStringLiteral("improvement"));
        path.insert(QStringLiteral("visualId"), key);
        path.insert(
                QStringLiteral("name"),
                QStringLiteral("Improvement %1")
                        .arg(improvementNumber));
        static constexpr std::array<const char *, 12> improvementColors{
                "#ffb84d", "#77cfa1", "#e88ac4", "#80b9f0",
                "#d5cf6b", "#b695e7", "#f18b75", "#70d1d0",
                "#c7a369", "#a9d879", "#de84a0", "#92a9ed"};
        path.insert(QStringLiteral("color"),
                    QString::fromLatin1(improvementColors[
                            (improvementNumber - 1u) %
                            improvementColors.size()]));
        path.insert(QStringLiteral("opacity"), 0.96);
        path.insert(QStringLiteral("visible"), true);
        path.insert(QStringLiteral("searchId"),
                    QVariant::fromValue<qulonglong>(searchId));
        path.insert(
                QStringLiteral("improvementNumber"),
                QVariant::fromValue<qulonglong>(improvementNumber));
        path.insert(
                QStringLiteral("geometry"),
                QVariant::fromValue(
                        static_cast<QObject *>(geometry.get())));
        paths.push_back(std::move(path));

        trajectoryGeometries_.reserve(
                trajectoryGeometries_.size() + 1u);
        trajectoryKeys_.reserve(trajectoryKeys_.size() + 1u);
        trajectoryGeometries_.push_back(std::move(geometry));
        trajectoryKeys_.push_back(key);
        trajectoryPaths_ = std::move(paths);
        emit trajectoriesChanged();
        setStatusText(
                QStringLiteral("Improvement %1 trajectory added")
                        .arg(improvementNumber));
        return true;
    } catch (const std::exception &exception) {
        setStatusText(
                QStringLiteral(
                        "Adding search improvement trajectory failed: %1")
                        .arg(QString::fromUtf8(exception.what())));
    } catch (...) {
        setStatusText(QStringLiteral(
                "Adding search improvement trajectory failed unexpectedly."));
    }
    return false;
}

void RaceViewerController::updateBestTrajectory(
        const QString &name,
        const std::vector<RaceViewerFrame> &frames) {
    try {
        const float radius = static_cast<float>(
                std::clamp(sceneRadius_ * 0.0004, 0.015, 0.15) *
                visualStyle(QStringLiteral("trajectory:best"))
                        .value(QStringLiteral("width"), 3.0).toDouble() /
                3.0);
        RaceViewerMeshBuffers mesh = BuildTrajectoryMesh(frames, radius);
        if (mesh.filled.isEmpty()) {
            return;
        }
        bestTrajectoryGeometry_.setMesh(
                std::move(mesh.filled),
                static_cast<int>(sizeof(FilledVertex)),
                QQuick3DGeometry::PrimitiveType::Triangles,
                true,
                mesh.boundsMin,
                mesh.boundsMax);

        QVariantList paths = trajectoryPaths_;
        const auto existing = std::find_if(
                paths.begin(), paths.end(), [](const QVariant &entry) {
                    return entry.toMap()
                                   .value(QStringLiteral("runId"))
                                   .toString() == QStringLiteral("best");
                });
        const bool visible = existing == paths.end() ||
                existing->toMap()
                        .value(QStringLiteral("visible"), true)
                        .toBool();
        QVariantMap path;
        path.insert(QStringLiteral("kind"), QStringLiteral("run"));
        path.insert(QStringLiteral("runId"), QStringLiteral("best"));
        path.insert(QStringLiteral("visualId"), QStringLiteral("trajectory:best"));
        path.insert(QStringLiteral("name"), name);
        path.insert(QStringLiteral("color"), QStringLiteral("#58a6ff"));
        path.insert(QStringLiteral("opacity"), 0.9);
        path.insert(QStringLiteral("visible"), visible);
        path.insert(
                QStringLiteral("geometry"),
                QVariant::fromValue(
                        static_cast<QObject *>(&bestTrajectoryGeometry_)));
        if (existing == paths.end()) {
            paths.push_back(path);
        } else {
            *existing = path;
        }
        trajectoryPaths_ = std::move(paths);
        emit trajectoriesChanged();
    } catch (const std::exception &exception) {
        setStatusText(
                QStringLiteral("Updating the Best trajectory failed: %1")
                        .arg(QString::fromUtf8(exception.what())));
    } catch (...) {
        setStatusText(QStringLiteral(
                "Updating the Best trajectory failed unexpectedly."));
    }
}

void RaceViewerController::refreshStyledTrajectory(const QString &runId) {
    const auto run = std::find_if(
            runs_.begin(), runs_.end(), [&runId](const RaceViewerRun &entry) {
                return entry.id == runId;
            });
    if (run == runs_.end() || run->frames.empty()) return;
    const QString visualId = QStringLiteral("trajectory:") + runId;
    const float radius = static_cast<float>(
            std::clamp(sceneRadius_ * 0.0004, 0.015, 0.15) *
            visualStyle(visualId)
                    .value(QStringLiteral("width"), 3.0).toDouble() / 3.0);
    const RaceViewerMeshBuffers mesh = BuildTrajectoryMesh(run->frames, radius);
    if (mesh.filled.isEmpty()) return;
    RaceGeometry &geometry = runId == QLatin1String("best")
            ? bestTrajectoryGeometry_ : inputPreviewGeometry_;
    geometry.setMesh(mesh.filled,
                     static_cast<int>(sizeof(FilledVertex)),
                     QQuick3DGeometry::PrimitiveType::Triangles,
                     true, mesh.boundsMin, mesh.boundsMax);
}

void RaceViewerController::setTimeMs(qint64 value) {
    if (manualDriving_) {
        return;
    }
    const qint64 clamped = std::clamp<qint64>(value, 0, durationMs_);
    if (timeMs_ == clamped) {
        return;
    }
    timeMs_ = clamped;
    updatePose();
    emit timeChanged();
}

void RaceViewerController::setCurrentTick(qint64 tick) {
    const RaceViewerRun *const run = selectedRun();
    if (run == nullptr || run->frames.empty()) {
        setTimeMs(0);
        return;
    }
    const qint64 clamped = std::clamp<qint64>(tick, 0, tickCount() - 1);
    setTimeMs(std::min<qint64>(
            durationMs_,
            clamped * static_cast<qint64>(kViewerTickDurationMs)));
}

void RaceViewerController::setSelectedRunId(const QString &value) {
    if (manualDriving_) {
        return;
    }
    if (simulationDebugger_.active() && value != QStringLiteral("debug")) {
        stopSimulationDebugger();
    }
    const auto selected = std::find_if(
            runs_.begin(), runs_.end(), [&value](const RaceViewerRun &run) {
                return run.id == value;
            });
    if (selected == runs_.end()) return;
    autoSelectLiveBest_ = false;
    if (selectedRunId_ == value) {
        return;
    }
    pause();
    selectedRunId_ = value;
    refreshSelectedRun();
    emit selectedRunChanged();
    emit timelineChanged();
    emit timeChanged();
}

void RaceViewerController::setPreviewInputScript(const QString &value) {
    if (previewInputScript_ == value) {
        return;
    }
    previewInputScript_ = value;
    emit previewInputScriptChanged();
    if (loaded_ && !loading_ && !manualDriving_) {
        scheduleInputPreviewRebuild();
    }
}

void RaceViewerController::refreshInputPreview() {
    scheduleInputPreviewRebuild();
}

void RaceViewerController::focusInputPreview() {
    stopManualDrive();
    stopSimulationDebugger();
    selectNextInputPreview_ = true;
    setSelectedRunId(QStringLiteral("preview"));
    jumpToStart();
}

void RaceViewerController::setSimulationHorizonMs(qint64 value) {
    constexpr qint64 maximumHorizonMs = 2147481040;
    if (value < kViewerTickDurationMs || value > maximumHorizonMs ||
        value % kViewerTickDurationMs != 0 ||
        simulationHorizonMs_ == value) {
        return;
    }
    simulationHorizonMs_ = value;
    cancelInputPreviewBuild();
    emit simulationHorizonMsChanged();
    if (!loaded_) {
        return;
    }
    stopSimulationDebugger();
    stopManualDrive();
    scheduleInputPreviewRebuild();
    scheduleStoredRunRebuilds();
}

void RaceViewerController::setTakeOverOnInput(bool value) {
    if (takeOverOnInput_ == value) {
        return;
    }
    takeOverOnInput_ = value;
    emit takeOverOnInputChanged();
}

void RaceViewerController::setCameraPreset(int value) {
    const int clamped = std::clamp(value, 1, 3);
    if (cameraPreset_ == clamped) {
        return;
    }
    cameraPreset_ = clamped;
    QSettings().setValue(
            QStringLiteral("viewer/cameraPreset"), cameraPreset_);
    if (cameraResources_) {
        cameraRuntime_ =
                std::make_unique<RaceCameraRuntime>(cameraResources_);
    } else {
        cameraRuntime_.reset();
    }
    emit cameraPresetChanged();
    updateCarCamera();
}

void RaceViewerController::play() {
    if (shuttingDown_) return;
    if (simulationDebugger_.active()) {
        RaceViewerRun *const debugRun = selectedRun();
        if (!loaded_ || manualDriving_ || playing_ || debugRun == nullptr ||
            debugRun->id != QStringLiteral("debug")) {
            return;
        }
        timeMs_ = debugRun->frames.empty() ? 0 : debugRun->frames.back().timeMs;
        updatePose();
        emit timeChanged();
        simulationDebugger_.play();
        return;
    }
    const RaceViewerRun *const run = selectedRun();
    if (!loaded_ || manualDriving_ || run == nullptr ||
        run->frames.empty() || playing_) {
        return;
    }
    if (timeMs_ >= durationMs_) {
        setCurrentTick(0);
    } else {
        setCurrentTick(currentTick());
    }
    playbackStartTick_ = currentTick();
    playbackClock_.restart();
    playbackTimer_.start();
    setPlaying(true);
}

void RaceViewerController::pause() {
    playbackTimer_.stop();
    if (simulationDebugger_.active()) {
        simulationDebugger_.pause();
    }
    setPlaying(false);
}

void RaceViewerController::togglePlayback() {
    if (playing_) {
        pause();
    } else {
        play();
    }
}

void RaceViewerController::jumpToStart() {
    pause();
    setCurrentTick(0);
}

void RaceViewerController::jumpToEnd() {
    pause();
    setTimeMs(durationMs_);
}

void RaceViewerController::startManualDrive() {
    if (shuttingDown_) return;
    if (manualDriving_) {
        return;
    }
    if (!loaded_ || loading_ || manualRuntime_ == nullptr) {
        setStatusText(QStringLiteral(
                "Load a map before starting manual drive."));
        return;
    }

    if (simulationDebugger_.active()) {
        stopSimulationDebugger();
    }
    cancelInputPreviewBuild();
    cancelStoredRunRebuilds();
    pause();
    if (!resetManualDriveSession(QStringLiteral("Manual drive"))) {
        return;
    }
    manualDriving_ = true;
    emit manualDrivingChanged();
}

bool RaceViewerController::resetManualDriveSession(
        const QString &status,
        bool preserveHeldInputs) {
    if (manualRuntime_ == nullptr) {
        setStatusText(QStringLiteral(
                "Manual drive failed: physics runtime is unavailable."));
        return false;
    }
    auto restored =
            manualRuntime_->sandbox.RestoreState(
                    manualRuntime_->initialState);
    if (!restored) {
        setStatusText(
                QStringLiteral("Manual drive failed: %1")
                        .arg(SandboxErrorText(restored.Error())));
        return false;
    }
    auto resized = manualRuntime_->sandbox.SetSimulationHorizonMs(
            static_cast<std::uint32_t>(simulationHorizonMs_));
    if (!resized) {
        setStatusText(
                QStringLiteral("Manual drive failed: %1")
                        .arg(SandboxErrorText(resized.Error())));
        return false;
    }
    manualRuntime_->state = resized.Value();
    manualRuntime_->simulationHorizonMs =
            static_cast<std::uint32_t>(simulationHorizonMs_);
    resetManualTakeoverState();
    manualRuntime_->driverInputs.clear();
    if (preserveHeldInputs) {
        appendHeldManualInputs(static_cast<std::int32_t>(
                manualRuntime_->state.timeMs));
    } else {
        resetManualInputState();
    }
    if (!replaceManualInputs()) {
        return false;
    }

    upsertRun(
            QStringLiteral("manual"),
            QStringLiteral("Manual"),
            {ToViewerFrame(manualRuntime_->state)},
            {},
            true);
    manualDriveStartTick_ =
            static_cast<qint64>(manualRuntime_->state.tick);
    setStatusText(status);
    manualDriveClock_.restart();
    manualDriveTimer_.start();
    return true;
}

void RaceViewerController::stopManualDrive() {
    finishManualDrive(QStringLiteral("Manual drive stopped"), true);
}

bool RaceViewerController::giveUpManualDrive() {
    if (!manualDriving_ || manualRuntime_ == nullptr) {
        return false;
    }
    if (manualTakeover_) {
        const QString sourceRunId = takeoverSourceRunId_;
        const auto source = std::find_if(
                runs_.begin(), runs_.end(), [&sourceRunId](
                        const RaceViewerRun &run) {
                    return run.id == sourceRunId;
                });
        if (source == runs_.end()) {
            setStatusText(QStringLiteral(
                    "Restarting the source race failed because it is no "
                    "longer available."));
            return false;
        }
        const QString sourceName = source->name;
        manualDriveTimer_.stop();
        manualDriving_ = false;
        resetManualInputState();
        resetManualTakeoverState();
        emit manualDrivingChanged();
        setSelectedRunId(sourceRunId);
        setCurrentTick(0);
        play();
        setStatusText(QStringLiteral("%1 restarted").arg(sourceName));
        return true;
    }
    manualDriveTimer_.stop();
    if (!resetManualDriveSession(
                QStringLiteral("Manual drive restarted"), true)) {
        finishManualDrive(statusText_, false);
        return false;
    }
    return true;
}

bool RaceViewerController::respawnManualDrive() {
    if (!manualDriving_ || manualRuntime_ == nullptr) {
        return false;
    }
    const std::uint64_t maximumEventTime =
            static_cast<std::uint64_t>(
                    std::numeric_limits<std::int32_t>::max());
    if (manualRuntime_->state.timeMs >
        maximumEventTime - 2u * kViewerTickDurationMs) {
        setStatusText(QStringLiteral(
                "Manual drive failed: respawn time is outside the "
                "supported race range."));
        return false;
    }
    const std::int32_t eventTime = static_cast<std::int32_t>(
            manualRuntime_->state.timeMs + kViewerTickDurationMs);
    const std::size_t previousEventCount =
            manualRuntime_->driverInputs.size();
    manualRuntime_->driverInputs.push_back(
            ManualSwitchEvent(
                    eventTime,
                    PhysicsSandboxInputAction::Respawn,
                    true));
    appendHeldManualInputs(eventTime + kViewerTickDurationMs);
    if (!replaceManualInputs()) {
        manualRuntime_->driverInputs.resize(previousEventCount);
        return false;
    }
    setStatusText(QStringLiteral("Manual drive: respawn queued"));
    return true;
}

bool RaceViewerController::startSimulationDebugger() {
    if (shuttingDown_) return false;
    if (!loaded_ || loading_ || manualRuntime_ == nullptr ||
        !simulationDebugger_.available()) {
        setStatusText(QStringLiteral(
                "Load a map before starting native source debugging."));
        return false;
    }

    pause();
    if (manualDriving_) {
        stopManualDrive();
    }
    upsertRun(
            QStringLiteral("debug"),
            QStringLiteral("Reference source"),
            {ToViewerFrame(manualRuntime_->state)},
            {},
            true);
    RaceViewerRun *const run = selectedRun();
    if (run != nullptr && run->id == QStringLiteral("debug")) {
        run->frames.clear();
        refreshSelectedRun();
        emit timelineChanged();
        emit timeChanged();
    }
    const bool started = simulationDebugger_.startSession(
            loadedPacksDirectory_,
            loadedReplayPath_,
            simulationHorizonMs_,
            previewInputScript_);
    setStatusText(simulationDebugger_.statusText());
    return started;
}

void RaceViewerController::stopSimulationDebugger() {
    if (!simulationDebugger_.active()) {
        return;
    }
    pause();
    simulationDebugger_.stopSession();
    setStatusText(QStringLiteral("Native source debugging stopped"));
}

void RaceViewerController::setManualInput(const QString &input,
                                         bool active) {
    if (manualRuntime_ == nullptr) {
        return;
    }
    if (!manualDriving_) {
        if (takeOverOnInput_ && playing_) {
            beginManualTakeover(input, active);
        }
        return;
    }
    if (!applyManualInput(input, active)) {
        finishManualDrive(statusText_, false);
        return;
    }
    emit manualInputChanged();
}

bool RaceViewerController::beginManualTakeover(const QString &input,
                                               bool active) {
    if (!takeOverOnInput_ || !playing_ || manualRuntime_ == nullptr ||
        !loaded_ || loading_) {
        return false;
    }
    const RaceViewerRun *const sourceRun = selectedRun();
    if (sourceRun == nullptr || sourceRun->frames.empty()) {
        return false;
    }
    if (input != QStringLiteral("left") &&
        input != QStringLiteral("right") &&
        input != QStringLiteral("accelerate") &&
        input != QStringLiteral("brake")) {
        return false;
    }

    const qint64 sourceTick = currentTick();
    const std::size_t prefixCount = static_cast<std::size_t>(
            std::clamp<qint64>(
                    sourceTick + 1,
                    1,
                    static_cast<qint64>(sourceRun->frames.size())));
    std::vector<RaceViewerFrame> prefix(
            sourceRun->frames.begin(),
            sourceRun->frames.begin() +
                    static_cast<std::ptrdiff_t>(prefixCount));
    std::vector<SandboxInputEvent> sourceInputs =
            inputHistoryForRun(*sourceRun);

    auto captured = manualRuntime_->sandbox.CaptureState();
    auto previousInputsResult = manualRuntime_->sandbox.ReadInputs();
    if (!captured || !previousInputsResult) {
        setStatusText(QStringLiteral(
                "Manual takeover failed while saving the current simulation."));
        return false;
    }
    PhysicsSandboxState previousState =
            std::move(captured).Value();
    std::vector<SandboxInputEvent> previousInputs =
            std::move(previousInputsResult).Value();
    const bool resumePlayback = playing_;
    pause();

    const auto restorePreviousRuntime = [this,
                                         &previousState,
                                         &previousInputs]() {
        auto restored =
                manualRuntime_->sandbox.RestoreState(previousState);
        if (!restored) {
            setStatusText(
                    QStringLiteral("Restoring playback after a failed "
                                   "takeover failed: %1")
                            .arg(SandboxErrorText(restored.Error())));
            return false;
        }
        auto replaced =
                manualRuntime_->sandbox.ReplaceInputs(previousInputs);
        if (!replaced) {
            setStatusText(
                    QStringLiteral("Restoring playback inputs after a failed "
                                   "takeover failed: %1")
                            .arg(SandboxErrorText(replaced.Error())));
            return false;
        }
        auto state = manualRuntime_->sandbox.ReadState();
        if (!state) {
            setStatusText(
                    QStringLiteral("Reading restored playback after a failed "
                                   "takeover failed: %1")
                            .arg(SandboxErrorText(state.Error())));
            return false;
        }
        manualRuntime_->state = state.Value();
        return true;
    };
    const auto fail = [this,
                       resumePlayback,
                       &restorePreviousRuntime](
                              const QString &message) {
        setStatusText(message);
        if (restorePreviousRuntime() && resumePlayback) {
            play();
        }
        return false;
    };

    auto restored = manualRuntime_->sandbox.RestoreState(
            manualRuntime_->initialState);
    if (!restored) {
        return fail(
                QStringLiteral("Manual takeover failed: %1")
                        .arg(SandboxErrorText(restored.Error())));
    }
    auto resized = manualRuntime_->sandbox.SetSimulationHorizonMs(
            static_cast<std::uint32_t>(simulationHorizonMs_));
    if (!resized) {
        return fail(
                QStringLiteral("Manual takeover failed: %1")
                        .arg(SandboxErrorText(resized.Error())));
    }
    manualRuntime_->state = resized.Value();
    manualRuntime_->simulationHorizonMs =
            static_cast<std::uint32_t>(simulationHorizonMs_);
    auto replaced = manualRuntime_->sandbox.ReplaceInputs(sourceInputs);
    if (!replaced) {
        return fail(
                QStringLiteral("Manual takeover failed: %1")
                        .arg(SandboxErrorText(replaced.Error())));
    }
    const std::uint64_t targetTick =
            static_cast<std::uint64_t>(std::max<qint64>(0, sourceTick));
    if (targetTick > manualRuntime_->state.tick) {
        const std::uint64_t remaining =
                targetTick - manualRuntime_->state.tick;
        if (remaining >
            static_cast<std::uint64_t>(
                    std::numeric_limits<std::uint32_t>::max())) {
            return fail(QStringLiteral(
                    "Manual takeover failed because the selected time is "
                    "outside the supported race range."));
        }
        auto advanced = manualRuntime_->sandbox.AdvanceTicks(
                static_cast<std::uint32_t>(remaining));
        if (!advanced) {
            return fail(
                    QStringLiteral("Manual takeover failed: %1")
                            .arg(SandboxErrorText(advanced.Error())));
        }
        manualRuntime_->state = advanced.Value();
    }

    std::vector<SandboxInputEvent> previousTakeoverSource =
            std::move(takeoverSourceInputs_);
    std::vector<SandboxInputEvent> previousDriverInputs =
            std::move(manualRuntime_->driverInputs);
    const std::optional<std::int32_t> previousSteeringTakeover =
            steeringTakeoverTimeMs_;
    const std::optional<std::int32_t> previousLongitudinalTakeover =
            longitudinalTakeoverTimeMs_;
    const bool previousManualTakeover = manualTakeover_;
    const QString previousTakeoverSourceRunId = takeoverSourceRunId_;
    const bool previousLeft = manualLeft_;
    const bool previousRight = manualRight_;
    const bool previousAccelerate = manualAccelerate_;
    const bool previousBrake = manualBrake_;

    takeoverSourceInputs_ = std::move(sourceInputs);
    takeoverSourceRunId_ = sourceRun->id;
    manualRuntime_->driverInputs.clear();
    steeringTakeoverTimeMs_.reset();
    longitudinalTakeoverTimeMs_.reset();
    manualTakeover_ = true;
    manualLeft_ = false;
    manualRight_ = false;
    manualAccelerate_ = false;
    manualBrake_ = false;
    if (!applyManualInput(input, active)) {
        takeoverSourceInputs_ = std::move(previousTakeoverSource);
        manualRuntime_->driverInputs =
                std::move(previousDriverInputs);
        steeringTakeoverTimeMs_ = previousSteeringTakeover;
        longitudinalTakeoverTimeMs_ =
                previousLongitudinalTakeover;
        manualTakeover_ = previousManualTakeover;
        takeoverSourceRunId_ = previousTakeoverSourceRunId;
        manualLeft_ = previousLeft;
        manualRight_ = previousRight;
        manualAccelerate_ = previousAccelerate;
        manualBrake_ = previousBrake;
        if (restorePreviousRuntime() && resumePlayback) {
            play();
        }
        return false;
    }

    if (simulationDebugger_.active()) {
        simulationDebugger_.stopSession();
    }
    const RaceViewerFrame takeoverFrame =
            ToViewerFrame(manualRuntime_->state);
    if (!prefix.empty() &&
        prefix.back().timeMs == takeoverFrame.timeMs) {
        prefix.back() = takeoverFrame;
    } else {
        prefix.push_back(takeoverFrame);
    }
    upsertRun(
            QStringLiteral("manual"),
            QStringLiteral("Manual"),
            std::move(prefix),
            {},
            true);
    manualDriving_ = true;
    manualDriveStartTick_ =
            static_cast<qint64>(manualRuntime_->state.tick);
    setStatusText(QStringLiteral("Manual takeover"));
    manualDriveClock_.restart();
    manualDriveTimer_.start();
    emit manualDrivingChanged();
    emit manualInputChanged();
    return true;
}

bool RaceViewerController::applyManualInput(const QString &input,
                                            bool active) {
    bool *state = nullptr;
    PhysicsSandboxInputAction action =
            PhysicsSandboxInputAction::Unmapped;
    bool steering = false;
    if (input == QStringLiteral("left")) {
        state = &manualLeft_;
        action = PhysicsSandboxInputAction::SteerLeft;
        steering = true;
    } else if (input == QStringLiteral("right")) {
        state = &manualRight_;
        action = PhysicsSandboxInputAction::SteerRight;
        steering = true;
    } else if (input == QStringLiteral("accelerate")) {
        state = &manualAccelerate_;
        action = PhysicsSandboxInputAction::Accelerate;
    } else if (input == QStringLiteral("brake")) {
        state = &manualBrake_;
        action = PhysicsSandboxInputAction::Brake;
    } else {
        return true;
    }

    std::optional<std::int32_t> &takeoverTime = steering
            ? steeringTakeoverTimeMs_
            : longitudinalTakeoverTimeMs_;
    const bool firstInterference = manualTakeover_ &&
            !takeoverTime.has_value();
    if (!firstInterference && *state == active) {
        return true;
    }

    const std::uint64_t time = manualRuntime_->state.timeMs;
    const std::uint64_t maximumEventTime =
            static_cast<std::uint64_t>(
                    std::numeric_limits<std::int32_t>::max());
    if (time > maximumEventTime - kViewerTickDurationMs) {
        setStatusText(QStringLiteral(
                "Manual drive failed: input time is outside the supported "
                "race range."));
        return false;
    }
    // The current state already includes the tick ending at `time`. The
    // first control tick that can still be changed is the following one.
    const std::int32_t eventTime = static_cast<std::int32_t>(
            time + kViewerTickDurationMs);
    const std::size_t previousEventCount =
            manualRuntime_->driverInputs.size();
    const bool previousLeft = manualLeft_;
    const bool previousRight = manualRight_;
    const bool previousAccelerate = manualAccelerate_;
    const bool previousBrake = manualBrake_;
    const std::optional<std::int32_t> previousTakeoverTime =
            takeoverTime;

    if (firstInterference) {
        takeoverTime = eventTime;
        if (steering) {
            manualLeft_ = false;
            manualRight_ = false;
            manualRuntime_->driverInputs.push_back(
                    ManualAnalogInputEvent(
                            eventTime,
                            PhysicsSandboxInputAction::Steer,
                            0));
            if (SwitchInputActiveAt(
                        takeoverSourceInputs_,
                        PhysicsSandboxInputAction::SteerLeft,
                        eventTime)) {
                manualRuntime_->driverInputs.push_back(
                        ManualSwitchEvent(
                                eventTime,
                                PhysicsSandboxInputAction::SteerLeft,
                                false));
            }
            if (SwitchInputActiveAt(
                        takeoverSourceInputs_,
                        PhysicsSandboxInputAction::SteerRight,
                        eventTime)) {
                manualRuntime_->driverInputs.push_back(
                        ManualSwitchEvent(
                                eventTime,
                                PhysicsSandboxInputAction::SteerRight,
                                false));
            }
        } else {
            manualAccelerate_ = false;
            manualBrake_ = false;
            if (AnalogInputAt(
                        takeoverSourceInputs_,
                        PhysicsSandboxInputAction::Gas,
                        eventTime) != 0) {
                manualRuntime_->driverInputs.push_back(
                        ManualAnalogInputEvent(
                                eventTime,
                                PhysicsSandboxInputAction::Gas,
                                0));
            }
            for (const PhysicsSandboxInputAction sourceAction :
                 {PhysicsSandboxInputAction::Accelerate,
                  PhysicsSandboxInputAction::Gas,
                  PhysicsSandboxInputAction::Brake}) {
                if (SwitchInputActiveAt(
                            takeoverSourceInputs_,
                            sourceAction,
                            eventTime)) {
                    manualRuntime_->driverInputs.push_back(
                            ManualSwitchEvent(
                                    eventTime,
                                    sourceAction,
                                    false));
                }
            }
        }
    }
    if (*state != active) {
        *state = active;
        manualRuntime_->driverInputs.push_back(
                ManualSwitchEvent(eventTime, action, active));
    }
    if (!replaceManualInputs()) {
        manualRuntime_->driverInputs.resize(previousEventCount);
        manualLeft_ = previousLeft;
        manualRight_ = previousRight;
        manualAccelerate_ = previousAccelerate;
        manualBrake_ = previousBrake;
        takeoverTime = previousTakeoverTime;
        return false;
    }
    return true;
}

void RaceViewerController::releaseManualInputs() {
    if (!manualDriving_) {
        resetManualInputState();
        return;
    }
    if (manualTakeover_) {
        if (manualLeft_) {
            setManualInput(QStringLiteral("left"), false);
        }
        if (manualRight_) {
            setManualInput(QStringLiteral("right"), false);
        }
        if (manualAccelerate_) {
            setManualInput(QStringLiteral("accelerate"), false);
        }
        if (manualBrake_) {
            setManualInput(QStringLiteral("brake"), false);
        }
        return;
    }
    setManualInput(QStringLiteral("left"), false);
    setManualInput(QStringLiteral("right"), false);
    setManualInput(QStringLiteral("accelerate"), false);
    setManualInput(QStringLiteral("brake"), false);
}

std::vector<SandboxInputEvent>
RaceViewerController::inputHistoryForRun(
        const RaceViewerRun &run) const {
    if (manualRuntime_ == nullptr) {
        return {};
    }

    std::vector<SandboxInputEvent> source;
    if (run.id == QStringLiteral("manual")) {
        source = effectiveManualInputs();
    } else {
        source = run.inputs;
    }
    if (source.empty()) {
        bool accelerate = false;
        bool brake = false;
        forevervalidator::AnalogInputState steering = 0;
        for (const RaceViewerFrame &frame : run.frames) {
            const std::int32_t eventTime = static_cast<std::int32_t>(
                    std::clamp<std::int64_t>(
                            frame.timeMs,
                            std::numeric_limits<std::int32_t>::min(),
                            std::numeric_limits<std::int32_t>::max()));
            const bool nextAccelerate = frame.accelerate > 0.5f;
            const bool nextBrake = frame.brake > 0.5f;
            const forevervalidator::AnalogInputState nextSteering =
                    SaturateAnalogInputState(
                            static_cast<std::int64_t>(std::llround(
                                    static_cast<double>(frame.steering) *
                                    static_cast<double>(
                                            kAnalogInputScale))));
            if (nextAccelerate != accelerate) {
                accelerate = nextAccelerate;
                source.push_back(ManualSwitchEvent(
                        eventTime,
                        PhysicsSandboxInputAction::Accelerate,
                        accelerate));
            }
            if (nextBrake != brake) {
                brake = nextBrake;
                source.push_back(ManualSwitchEvent(
                        eventTime,
                        PhysicsSandboxInputAction::Brake,
                        brake));
            }
            if (nextSteering != steering) {
                steering = nextSteering;
                source.push_back(ManualAnalogInputEvent(
                        eventTime,
                        PhysicsSandboxInputAction::Steer,
                        steering));
            }
        }
    }

    std::vector<SandboxInputEvent> result =
            manualRuntime_->fixedInputs;
    result.reserve(result.size() + source.size());
    for (const SandboxInputEvent &event : source) {
        if (std::none_of(
                    result.begin(),
                    result.end(),
                    [&event](const SandboxInputEvent &existing) {
                        return SameInputEvent(existing, event);
                    })) {
            result.push_back(event);
        }
    }
    std::stable_sort(
            result.begin(),
            result.end(),
            [](const SandboxInputEvent &left,
               const SandboxInputEvent &right) {
                return left.timeMs < right.timeMs;
            });
    return result;
}

std::vector<SandboxInputEvent>
RaceViewerController::effectiveManualInputs() const {
    if (manualRuntime_ == nullptr) {
        return {};
    }
    std::vector<SandboxInputEvent> inputs = manualTakeover_
            ? takeoverSourceInputs_
            : manualRuntime_->fixedInputs;
    if (manualTakeover_) {
        inputs.erase(
                std::remove_if(
                        inputs.begin(),
                        inputs.end(),
                        [this](const SandboxInputEvent &event) {
                            return (steeringTakeoverTimeMs_ &&
                                    IsSteeringInput(event.action) &&
                                    event.timeMs >
                                            *steeringTakeoverTimeMs_) ||
                                    (longitudinalTakeoverTimeMs_ &&
                                     IsLongitudinalInput(event.action) &&
                                     event.timeMs >
                                             *longitudinalTakeoverTimeMs_);
                        }),
                inputs.end());
    }
    inputs.insert(
            inputs.end(),
            manualRuntime_->driverInputs.begin(),
            manualRuntime_->driverInputs.end());
    std::stable_sort(
            inputs.begin(),
            inputs.end(),
            [](const SandboxInputEvent &left,
               const SandboxInputEvent &right) {
                return left.timeMs < right.timeMs;
            });
    return inputs;
}

QString RaceViewerController::currentInputScript() const {
    const RaceViewerRun *const run = selectedRun();
    if (run == nullptr) {
        return {};
    }

    std::vector<SandboxInputEvent> inputs;
    if (run->id == QStringLiteral("manual")) {
        if (manualRuntime_ == nullptr) {
            return {};
        }
        inputs = effectiveManualInputs();
    } else {
        inputs = run->inputs;
    }

    std::int64_t raceStartTimeMs = 0;
    bool foundRaceStart = false;
    for (const SandboxInputEvent &event : inputs) {
        if (event.action != PhysicsSandboxInputAction::RaceRunning) {
            continue;
        }
        if (!foundRaceStart || event.timeMs < raceStartTimeMs) {
            raceStartTimeMs = event.timeMs;
            foundRaceStart = true;
        }
    }
    const std::int64_t pendingManualTick =
            run->id == QStringLiteral("manual")
            ? kViewerTickDurationMs
            : 0;
    const std::int64_t cutoffTimeMs = std::clamp<std::int64_t>(
            raceStartTimeMs + timeMs_ + pendingManualTick,
            std::numeric_limits<std::int32_t>::min(),
            std::numeric_limits<std::int32_t>::max());
    inputs.erase(
            std::remove_if(
                    inputs.begin(),
                    inputs.end(),
                    [cutoffTimeMs](const SandboxInputEvent &event) {
                        return event.timeMs > cutoffTimeMs;
                    }),
            inputs.end());
    return QString::fromStdString(FormatInputScript(inputs));
}

QQuick3DGeometry *RaceViewerController::ellipsoidFilledGeometryForRun(
        int runIndex) {
    if (ellipsoidFilledGeometries_.empty()) return nullptr;
    const int count = static_cast<int>(ellipsoidFilledGeometries_.size());
    const int normalized = runIndex < 0 ? 0 : runIndex % count;
    return ellipsoidFilledGeometries_[static_cast<std::size_t>(normalized)]
            .get();
}

bool RaceViewerController::hasTrajectoryForRun(const QString &runId) const {
    return std::any_of(
            trajectoryPaths_.begin(),
            trajectoryPaths_.end(),
            [&runId](const QVariant &entry) {
                return entry.toMap()
                               .value(QStringLiteral("runId"))
                               .toString() == runId;
            });
}

bool RaceViewerController::trajectoryVisibleForRun(
        const QString &runId) const {
    const auto path = std::find_if(
            trajectoryPaths_.begin(),
            trajectoryPaths_.end(),
            [&runId](const QVariant &entry) {
                return entry.toMap()
                               .value(QStringLiteral("runId"))
                               .toString() == runId;
            });
    return path != trajectoryPaths_.end() &&
            path->toMap()
                    .value(QStringLiteral("visible"), true)
                    .toBool();
}

void RaceViewerController::setTrajectoryVisibleForRun(
        const QString &runId,
        bool visible) {
    QVariantList paths = trajectoryPaths_;
    const auto path = std::find_if(
            paths.begin(), paths.end(), [&runId](const QVariant &entry) {
                return entry.toMap()
                               .value(QStringLiteral("runId"))
                               .toString() == runId;
            });
    if (path == paths.end()) {
        return;
    }
    QVariantMap updated = path->toMap();
    if (updated.value(QStringLiteral("visible"), true).toBool() == visible) {
        return;
    }
    updated.insert(QStringLiteral("visible"), visible);
    *path = std::move(updated);
    trajectoryPaths_ = std::move(paths);
    emit trajectoriesChanged();
}


bool RaceViewerController::hasPreviewTrajectories() const {
    return !pendingImprovements_.empty() ||
            !trajectoryGeometries_.empty() ||
            std::any_of(
                    trajectoryPaths_.begin(),
                    trajectoryPaths_.end(),
                    [](const QVariant &entry) {
                        return entry.toMap()
                                       .value(QStringLiteral("kind"))
                                       .toString() ==
                                QStringLiteral("improvement");
                    });
}

void RaceViewerController::clearPreviewTrajectories() {
    previewClearedAt_ = std::chrono::steady_clock::now();
    if (!hasPreviewTrajectories()) {
        return;
    }

    QVariantList paths = trajectoryPaths_;
    paths.erase(
            std::remove_if(
                    paths.begin(),
                    paths.end(),
                    [](const QVariant &entry) {
                        return entry.toMap()
                                       .value(QStringLiteral("kind"))
                                       .toString() ==
                                QStringLiteral("improvement");
                    }),
            paths.end());
    const bool pathsChanged = paths.size() != trajectoryPaths_.size();
    trajectoryPaths_ = std::move(paths);

    std::vector<std::unique_ptr<RaceGeometry>>().swap(
            trajectoryGeometries_);
    std::vector<QString>().swap(trajectoryKeys_);
    std::vector<PendingImprovement>().swap(pendingImprovements_);

    if (pathsChanged) {
        emit trajectoriesChanged();
    }
    setStatusText(QStringLiteral("Preview trajectories cleared"));
}

void RaceViewerController::clearSearchResults() {
    const auto isBest = [](const RaceViewerRun &run) {
        return run.id == QStringLiteral("best");
    };
    const bool hadBest = std::any_of(runs_.begin(), runs_.end(), isBest);
    if (!hadBest && !pendingRun_ && !hasPreviewTrajectories()) return;
    const bool rebuildPending = storedRunBuildPending_ || storedRunThread_ != nullptr;
    // In-flight rebuilds must not resurrect a result from the old session.
    cancelStoredRunRebuilds();
    pendingRun_.reset();
    clearPreviewTrajectories();
    const bool selected = selectedRunId_ == QStringLiteral("best");
    if (selected) pause();
    runs_.erase(std::remove_if(runs_.begin(), runs_.end(), isBest), runs_.end());
    trajectoryPaths_.erase(
            std::remove_if(trajectoryPaths_.begin(), trajectoryPaths_.end(),
                           [](const QVariant &path) {
                               return path.toMap().value(QStringLiteral("runId")) ==
                                       QStringLiteral("best");
                           }),
            trajectoryPaths_.end());
    bestTrajectoryGeometry_.clearMesh();
    ++runGeometryRevision_;
    if (selected) {
        selectedRunId_ = runs_.empty() ? QString{} : runs_.front().id;
        emit selectedRunChanged();
    }
    if (hadBest) {
        emit runsChanged();
        emit trajectoriesChanged();
        refreshSelectedRun();
        emit timelineChanged();
        emit timeChanged();
    }
    if (rebuildPending) scheduleStoredRunRebuilds();
}

void RaceViewerController::scheduleInputPreviewRebuild() {
    if (shuttingDown_) return;
    if (!loaded_ || manualRuntime_ == nullptr || manualDriving_) {
        return;
    }
    ++inputPreviewSerial_;
    inputPreviewBuildPending_ = true;
    if (inputPreviewThread_ != nullptr) {
        inputPreviewThread_->requestInterruption();
        return;
    }
    startInputPreviewBuild();
}

void RaceViewerController::startInputPreviewBuild() {
    if (shuttingDown_) return;
    if (!inputPreviewBuildPending_ || inputPreviewThread_ != nullptr ||
        !loaded_ || manualRuntime_ == nullptr || manualDriving_) {
        return;
    }
    inputPreviewBuildPending_ = false;
    const std::uint64_t previewSerial = inputPreviewSerial_;
    const std::uint64_t loadSerial = loadSerial_;
    const QString packsDirectory = loadedPacksDirectory_;
    const QString replayPath = loadedReplayPath_;
    const PhysicsBackend backend = loadedBackend_;
    const std::uint32_t simulationHorizonMs =
            static_cast<std::uint32_t>(simulationHorizonMs_);
    const QString script = previewInputScript_;
    const float trajectoryRadius = static_cast<float>(
            std::clamp(sceneRadius_ * 0.0004, 0.015, 0.15));
    std::shared_ptr<ManualDriveRuntime> runtime = inputPreviewRuntime_;
    auto result = std::make_shared<RaceViewerInputPreviewResult>();

    QThread *const thread = QThread::create(
            [result,
             packsDirectory,
             replayPath,
             backend,
             simulationHorizonMs,
             script,
             trajectoryRadius,
             runtime = std::move(runtime)]() mutable {
                *result = BuildInputPreview(
                        packsDirectory,
                        replayPath,
                        backend,
                        simulationHorizonMs,
                        script,
                        trajectoryRadius,
                        std::move(runtime));
            });
    inputPreviewThread_ = thread;
    connect(thread, &QThread::finished, this,
            [this, thread, previewSerial, loadSerial,
             result = std::move(result)]() mutable {
        if (inputPreviewThread_ == thread) {
            inputPreviewThread_ = nullptr;
        }
        applyInputPreviewResult(
                previewSerial, loadSerial, std::move(*result));
        if (inputPreviewBuildPending_) {
            startInputPreviewBuild();
        }
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

void RaceViewerController::applyInputPreviewResult(
        std::uint64_t previewSerial,
        std::uint64_t loadSerial,
        RaceViewerInputPreviewResult result) {
    if (loadSerial == loadSerial_ && result.runtime != nullptr &&
        inputPreviewRuntime_ == nullptr) {
        inputPreviewRuntime_ = result.runtime;
    }
    if (previewSerial != inputPreviewSerial_ || loadSerial != loadSerial_ ||
        result.canceled) {
        return;
    }
    if (!result.error.isEmpty()) {
        setStatusText(result.error);
        clearInputPreview();
        emit stateChanged();
        return;
    }
    if (result.frames.empty() || result.mesh.filled.isEmpty()) {
        clearInputPreview();
        emit stateChanged();
        return;
    }

    const bool resumePlayback =
            playing_ && selectedRunId_ == QStringLiteral("preview") &&
            !simulationDebugger_.active();
    if (resumePlayback) {
        pause();
    }
    QVariantList paths = trajectoryPaths_;
    QVariantMap path;
    path.insert(QStringLiteral("kind"), QStringLiteral("preview"));
    path.insert(QStringLiteral("runId"), QStringLiteral("preview"));
    path.insert(QStringLiteral("visualId"), QStringLiteral("trajectory:preview"));
    path.insert(QStringLiteral("name"), QStringLiteral("Inputs"));
    path.insert(QStringLiteral("color"), QStringLiteral("#41c979"));
    path.insert(QStringLiteral("opacity"), 0.94);
    path.insert(
            QStringLiteral("geometry"),
            QVariant::fromValue(
                    static_cast<QObject *>(&inputPreviewGeometry_)));
    const auto existingPath = std::find_if(
            paths.begin(), paths.end(), [](const QVariant &entry) {
                return entry.toMap()
                               .value(QStringLiteral("kind"))
                               .toString() == QStringLiteral("preview");
            });
    path.insert(
            QStringLiteral("visible"),
            existingPath == paths.end() ||
                    existingPath->toMap()
                            .value(QStringLiteral("visible"), true)
                            .toBool());
    if (existingPath == paths.end()) {
        paths.prepend(path);
    } else {
        *existingPath = path;
    }
    inputPreviewGeometry_.setMesh(
            std::move(result.mesh.filled),
            static_cast<int>(sizeof(FilledVertex)),
            QQuick3DGeometry::PrimitiveType::Triangles,
            true,
            result.mesh.boundsMin,
            result.mesh.boundsMax);
    trajectoryPaths_ = std::move(paths);
    inputPreviewVisible_ = true;
    emit trajectoriesChanged();
    upsertRun(
            QStringLiteral("preview"),
            QStringLiteral("Inputs"),
            std::move(result.frames),
            std::move(result.inputs),
            false,
            result.runtime);
    if (selectNextInputPreview_) {
        selectNextInputPreview_ = false;
        setSelectedRunId(QStringLiteral("preview"));
        jumpToStart();
    }
    if (visualStyle(QStringLiteral("trajectory:preview"))
                .contains(QStringLiteral("width"))) {
        refreshStyledTrajectory(QStringLiteral("preview"));
    }
    if (resumePlayback) {
        play();
    }
    emit stateChanged();
}

void RaceViewerController::cancelInputPreviewBuild() {
    ++inputPreviewSerial_;
    inputPreviewBuildPending_ = false;
    if (inputPreviewThread_ != nullptr) {
        inputPreviewThread_->requestInterruption();
    }
}

void RaceViewerController::waitForInputPreviewWorker() {
    cancelInputPreviewBuild();
    if (inputPreviewThread_ != nullptr) {
        inputPreviewThread_->quit();
        inputPreviewThread_->wait();
        inputPreviewThread_ = nullptr;
    }
}

void RaceViewerController::scheduleStoredRunRebuilds() {
    if (shuttingDown_) return;
    if (!loaded_ || manualDriving_) return;
    ++storedRunSerial_;
    storedRunBuildPending_ = true;
    if (storedRunThread_ != nullptr) {
        storedRunThread_->requestInterruption();
        return;
    }
    startStoredRunRebuilds();
}

void RaceViewerController::startStoredRunRebuilds() {
    if (shuttingDown_) return;
    if (!storedRunBuildPending_ || storedRunThread_ != nullptr ||
        !loaded_ || manualDriving_) {
        return;
    }
    struct Job {
        QString id;
        QString name;
        QString script;
        PhysicsBackend backend;
        std::shared_ptr<ManualDriveRuntime> runtime;
    };
    std::vector<Job> jobs;
    for (const RaceViewerRun &run : runs_) {
        if (run.id == QStringLiteral("preview") ||
            run.id == QStringLiteral("debug") || run.inputs.empty()) {
            continue;
        }
        jobs.push_back({run.id,
                        run.name,
                        QString::fromStdString(FormatInputScript(run.inputs)),
                        kAuxiliarySimulationBackend,
                        run.runtime});
    }
    storedRunBuildPending_ = false;
    if (jobs.empty()) return;

    const std::uint64_t serial = storedRunSerial_;
    const std::uint64_t loadSerial = loadSerial_;
    const quint64 bestRevision = bestRunRevision_;
    const QString packsDirectory = loadedPacksDirectory_;
    const QString replayPath = loadedReplayPath_;
    const std::uint32_t horizon =
            static_cast<std::uint32_t>(simulationHorizonMs_);
    const float radius = static_cast<float>(
            std::clamp(sceneRadius_ * 0.0004, 0.015, 0.15));
    using Output = std::tuple<QString, QString, RaceViewerInputPreviewResult>;
    auto outputs = std::make_shared<std::vector<Output>>();
    QThread *const thread = QThread::create(
            [outputs,
             jobs = std::move(jobs),
             packsDirectory,
             replayPath,
             horizon,
             radius]() mutable {
                outputs->reserve(jobs.size());
                for (Job &job : jobs) {
                    RaceViewerInputPreviewResult result = BuildInputPreview(
                            packsDirectory,
                            replayPath,
                            job.backend,
                            horizon,
                            job.script,
                            radius,
                            std::move(job.runtime));
                    outputs->emplace_back(
                            std::move(job.id),
                            std::move(job.name),
                            std::move(result));
                    if (QThread::currentThread()->isInterruptionRequested()) {
                        break;
                    }
                }
            });
    storedRunThread_ = thread;
    connect(thread, &QThread::finished, this,
            [this, thread, serial, loadSerial, bestRevision, outputs]() mutable {
                if (storedRunThread_ == thread) storedRunThread_ = nullptr;
                if (serial == storedRunSerial_ &&
                    loadSerial == loadSerial_) {
                    for (auto &[id, name, result] : *outputs) {
                        // A later live improvement owns the best run, even if
                        // this older rebuild finishes in another search cycle.
                        if (id == QStringLiteral("best") && bestRevision != bestRunRevision_) {
                            continue;
                        }
                        if (result.canceled) {
                            continue;
                        }
                        if (!result.error.isEmpty() || result.frames.empty()) {
                            setStatusText(
                                    QStringLiteral("Rebuilding %1 failed: %2")
                                            .arg(name,
                                                 result.error.isEmpty()
                                                         ? QStringLiteral(
                                                                   "no frames were produced")
                                                         : result.error));
                            continue;
                        }
                        upsertRun(std::move(id),
                                  std::move(name),
                                  std::move(result.frames),
                                  std::move(result.inputs),
                                  false,
                                  std::move(result.runtime));
                    }
                }
                if (storedRunBuildPending_) startStoredRunRebuilds();
            });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

void RaceViewerController::cancelStoredRunRebuilds() {
    ++storedRunSerial_;
    storedRunBuildPending_ = false;
    if (storedRunThread_ != nullptr) {
        storedRunThread_->requestInterruption();
    }
}

void RaceViewerController::waitForStoredRunWorker() {
    cancelStoredRunRebuilds();
    if (storedRunThread_ != nullptr) {
        storedRunThread_->quit();
        storedRunThread_->wait();
        storedRunThread_ = nullptr;
    }
}

void RaceViewerController::clearInputPreview() {
    const qsizetype previousPathCount = trajectoryPaths_.size();
    trajectoryPaths_.erase(
            std::remove_if(
                    trajectoryPaths_.begin(),
                    trajectoryPaths_.end(),
                    [](const QVariant &entry) {
                        return entry.toMap()
                                           .value(QStringLiteral("kind"))
                                           .toString() ==
                                QStringLiteral("preview");
                    }),
            trajectoryPaths_.end());
    if (inputPreviewVisible_) {
        inputPreviewGeometry_.clearMesh();
        inputPreviewVisible_ = false;
    }
    if (trajectoryPaths_.size() != previousPathCount) {
        emit trajectoriesChanged();
    }

    const auto previewRun = std::find_if(
            runs_.begin(), runs_.end(), [](const RaceViewerRun &run) {
                return run.id == QStringLiteral("preview");
            });
    if (previewRun == runs_.end()) {
        return;
    }
    const bool selected = selectedRunId_ == QStringLiteral("preview");
    if (selected) {
        pause();
    }
    runs_.erase(previewRun);
    emit runsChanged();
    if (selected) {
        selectedRunId_ = runs_.empty() ? QString{} : runs_.front().id;
        emit selectedRunChanged();
    }
    refreshSelectedRun();
    emit timelineChanged();
    emit timeChanged();
}

void RaceViewerController::loadMap(const QString &packsDirectory,
                                   const QString &replayPath) {
    loadMap(packsDirectory,
            replayPath,
            QStringLiteral("optimized-cpu"));
}

void RaceViewerController::loadMap(const QString &packsDirectory,
                                   const QString &replayPath,
                                   const QString &backendId) {
    if (shuttingDown_) return;
    stopSimulationDebugger();
    stopManualDrive();
    const std::optional<PhysicsBackend> backend =
            ParsePhysicsBackend(backendId.toStdString());
    if (!backend) {
        setStatusText(QStringLiteral("Select a valid physics backend."));
        return;
    }
    pendingRun_.reset();
    pendingImprovements_.clear();
    if (workerThread_ != nullptr) {
        queuedMapLoad_ =
                MapLoadRequest{packsDirectory, replayPath,
                               kAuxiliarySimulationBackend};
        setLoading(true);
        setStatusText(QStringLiteral("Waiting to load selected map..."));
        return;
    }
    beginMapLoad(packsDirectory, replayPath,
                 kAuxiliarySimulationBackend);
}

void RaceViewerController::beginMapLoad(const QString &packsDirectory,
                                        const QString &replayPath,
                                        PhysicsBackend backend) {
    if (shuttingDown_) return;
    if (workerThread_ != nullptr) {
        queuedMapLoad_ =
                MapLoadRequest{packsDirectory, replayPath, backend};
        setLoading(true);
        setStatusText(QStringLiteral("Waiting to load selected map..."));
        return;
    }
    queuedMapLoad_.reset();
    const QFileInfo packsInfo(packsDirectory);
    const QFileInfo replayInfo(replayPath);
    if (!packsInfo.isDir() || !packsInfo.isReadable()) {
        pendingRun_.reset();
        pendingImprovements_.clear();
        setStatusText(QStringLiteral(
                "Select a readable installed Packs directory."));
        setLoading(false);
        return;
    }
    if (!replayInfo.isFile() || !replayInfo.isReadable()) {
        pendingRun_.reset();
        pendingImprovements_.clear();
        setStatusText(QStringLiteral(
                "Select a readable replay or challenge file."));
        setLoading(false);
        return;
    }

    // Keep the published 3D scene attached until the replacement is complete.
    // Publishing an empty run/ellipsoid model detaches nested Repeater3D render
    // nodes on some Qt Quick 3D backends.
    cancelRayTracingBuild();
    setLoading(true);
    cancelInputPreviewBuild();
    cancelStoredRunRebuilds();
    setStatusText(QStringLiteral(
            "Loading map geometry and materials..."));

    const std::uint64_t loadSerial = ++loadSerial_;
    const std::uint32_t simulationHorizonMs =
            static_cast<std::uint32_t>(simulationHorizonMs_);
    QThread *const thread = QThread::create(
            [this,
             packsDirectory,
             replayPath,
             backend,
             simulationHorizonMs,
             loadSerial]() {
                RaceViewerLoadResult result =
                        LoadMapData(
                                packsDirectory,
                                replayPath,
                                backend,
                                simulationHorizonMs);
                QMetaObject::invokeMethod(
                        this,
                        [this, loadSerial,
                         result = std::move(result)]() mutable {
                            applyLoadResult(loadSerial, std::move(result));
                        },
                        Qt::QueuedConnection);
            });
    workerThread_ = thread;
    connect(thread, &QThread::finished, this, [this, thread]() {
        if (workerThread_ == thread) {
            workerThread_ = nullptr;
        }
        if (queuedMapLoad_) {
            const MapLoadRequest request = *queuedMapLoad_;
            queuedMapLoad_.reset();
            beginMapLoad(request.packsDirectory,
                         request.replayPath,
                         request.backend);
            return;
        }
        if (pendingRun_ &&
            (!loaded_ ||
             loadedPacksDirectory_ != pendingRun_->packsDirectory ||
             loadedReplayPath_ != pendingRun_->replayPath)) {
            beginMapLoad(pendingRun_->packsDirectory,
                         pendingRun_->replayPath,
                         pendingRun_->backend);
            return;
        }
        if (!pendingImprovements_.empty()) {
            const PendingImprovement &pending =
                    pendingImprovements_.back();
            if (!loaded_ ||
                loadedPacksDirectory_ != pending.packsDirectory ||
                loadedReplayPath_ != pending.replayPath) {
                beginMapLoad(pending.packsDirectory,
                             pending.replayPath,
                             pending.backend);
            }
        }
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

void RaceViewerController::applyLoadResult(
        std::uint64_t loadSerial,
        RaceViewerLoadResult result) {
    if (loadSerial != loadSerial_) return;
    pause();
    if (!result.error.isEmpty()) {
        pendingRun_.reset();
        pendingImprovements_.clear();
        setStatusText(result.error);
        if (loaded_ && !queuedMapLoad_) {
            scheduleInputPreviewRebuild();
        }
        if (!queuedMapLoad_) setLoading(false);
        return;
    }
    stopManualDrive();

    cancelRayTracingBuild();
    rayTracingSourceBatches_ = result.visualBatches;
    trackFilledGeometry_.setMesh(
            std::move(result.track.filled),
            static_cast<int>(sizeof(FilledVertex)),
            QQuick3DGeometry::PrimitiveType::Triangles,
            true,
            result.track.boundsMin,
            result.track.boundsMax);
    trackWireGeometry_.setMesh(
            std::move(result.track.wire),
            static_cast<int>(sizeof(WireVertex)),
            QQuick3DGeometry::PrimitiveType::Lines,
            false,
            result.track.boundsMin,
            result.track.boundsMax);
    std::vector<std::unique_ptr<RaceGeometry>> visualGeometries;
    visualGeometries.reserve(result.visualBatches.size());
    for (StaticVisualBatch &batch : result.visualBatches) {
        auto geometry = std::make_unique<RaceGeometry>();
        const int indexCount =
                static_cast<int>(batch.indices.size() /
                                 static_cast<qsizetype>(sizeof(std::uint32_t)));
        geometry->setIndexedMesh(
                std::move(batch.vertices), std::move(batch.indices),
                StaticVisualVertexStride, true, true, true, true,
                batch.hasVertexColors, batch.boundsMin, batch.boundsMax,
                {{0, indexCount}});
        visualGeometries.push_back(std::move(geometry));
    }
    QVariantList visualBatches;
    visualBatches.reserve(result.visualBatchItems.size());
    for (QVariant &entry : result.visualBatchItems) {
        QVariantMap item = entry.toMap();
        const qint64 batchIndex =
                item.value(QStringLiteral("batchIndex")).toLongLong();
        if (batchIndex < 0 ||
            batchIndex >= static_cast<qint64>(visualGeometries.size())) {
            continue;
        }
        item.insert(
                QStringLiteral("geometry"),
                QVariant::fromValue(static_cast<QObject *>(
                        visualGeometries[static_cast<std::size_t>(batchIndex)]
                                .get())));
        visualBatches.push_back(std::move(item));
    }
    visualGeometries_ = std::move(visualGeometries);
    rayTracingScene_ = std::move(result.rayTracingScene);
    emit rayTracingSceneChanged();
    visualMaterials_ = std::move(result.visualMaterials);
    visualBatches_ = std::move(visualBatches);
    carEllipsoids_ = std::move(result.carEllipsoids);
    triangleCount_ = result.triangleCount;
    visualTriangleCount_ = result.visualTriangleCount;
    sourceVisualObjectCount_ = result.sourceVisualObjectCount;
    sourceVisualMeshCount_ = result.sourceVisualMeshCount;
    duplicateVisualObjectCount_ = result.duplicateVisualObjectCount;
    materialCount_ = result.materialCount;
    diagnosticCount_ = result.diagnosticCount;
    sceneBoundsMin_ = result.visualBoundsMin;
    sceneBoundsMax_ = result.visualBoundsMax;
    sceneRadius_ = std::max(
            1.0, 0.5 * static_cast<double>(
                    (result.visualBoundsMax -
                     result.visualBoundsMin).length()));
    timeMs_ = 0;
    loadedPacksDirectory_ = result.packsDirectory;
    loadedReplayPath_ = result.replayPath;
    loadedBackend_ = result.backend;
    whiteboard_.setMapIdentity(result.mapKey, result.mapName);
    manualRuntime_ = std::move(result.manualRuntime);
    cameraResources_ = std::move(result.cameraResources);
    cameraRuntime_ = cameraResources_
            ? std::make_unique<RaceCameraRuntime>(cameraResources_)
            : nullptr;
    inputPreviewRuntime_.reset();
    resetManualTakeoverState();
    simulationDebugger_.configure(QStringLiteral("Reference"));
    loaded_ = true;
    runs_.clear();
    selectedRunId_.clear();
    trajectoryPaths_.clear();
    inputPreviewGeometry_.clearMesh();
    bestTrajectoryGeometry_.clearMesh();
    inputPreviewVisible_ = false;
    trajectoryGeometries_.clear();
    trajectoryKeys_.clear();
    durationMs_ = 0;
    const bool addingPendingRun =
            pendingRun_ &&
            pendingRun_->packsDirectory == loadedPacksDirectory_ &&
            pendingRun_->replayPath == loadedReplayPath_;
    const bool addingPendingImprovements =
            std::any_of(
                    pendingImprovements_.begin(),
                    pendingImprovements_.end(),
                    [this](const PendingImprovement &pending) {
                        return pending.packsDirectory ==
                                        loadedPacksDirectory_ &&
                                pending.replayPath ==
                                        loadedReplayPath_;
                    });
    applyPendingRunIfReady();
    scheduleInputPreviewRebuild();
    const bool pendingImprovementsAdded =
            applyPendingImprovementsIfReady();
    if (!addingPendingImprovements ||
        pendingImprovementsAdded) {
        setStatusText(addingPendingRun
                              ? QStringLiteral("Best run added")
                              : addingPendingImprovements
                              ? QStringLiteral(
                                        "Search improvement trajectories added")
                              : QStringLiteral("Map loaded"));
    }
    updatePose();
    emit runsChanged();
    emit selectedRunChanged();
    emit trajectoriesChanged();
    emit timelineChanged();
    emit timeChanged();
    if (!queuedMapLoad_) setLoading(false);
    emit sceneChanged();
    emit stateChanged();
}

void RaceViewerController::applyPendingRunIfReady() {
    if (!pendingRun_ || !loaded_ ||
        pendingRun_->packsDirectory != loadedPacksDirectory_ ||
        pendingRun_->replayPath != loadedReplayPath_) {
        return;
    }
    std::vector<RaceViewerFrame> frames =
            std::move(pendingRun_->frames);
    std::vector<SandboxInputEvent> inputs =
            std::move(pendingRun_->inputs);
    const bool select = pendingRun_->select && !previewingHistory_;
    pendingRun_.reset();
    upsertRun(QStringLiteral("best"),
              QStringLiteral("Best"),
              std::move(frames),
              std::move(inputs),
              select);
    scheduleStoredRunRebuilds();
}

bool RaceViewerController::applyPendingImprovementsIfReady() {
    if (!loaded_ || pendingImprovements_.empty()) {
        return true;
    }
    bool succeeded = true;
    std::vector<PendingImprovement> remaining;
    remaining.reserve(pendingImprovements_.size());
    for (PendingImprovement &pending : pendingImprovements_) {
        if (pending.packsDirectory == loadedPacksDirectory_ &&
            pending.replayPath == loadedReplayPath_) {
            if (!appendImprovementTrajectory(
                        pending.searchId,
                        pending.improvementNumber,
                        pending.frames)) {
                remaining.push_back(std::move(pending));
                succeeded = false;
            }
        } else {
            remaining.push_back(std::move(pending));
        }
    }
    pendingImprovements_ = std::move(remaining);
    return succeeded;
}

void RaceViewerController::setLoading(bool value) {
    if (loading_ == value) {
        return;
    }
    loading_ = value;
    emit stateChanged();
}

void RaceViewerController::setStatusText(const QString &value) {
    if (statusText_ == value) {
        return;
    }
    statusText_ = value;
    emit stateChanged();
}

const RaceViewerRun *RaceViewerController::selectedRun() const noexcept {
    const auto selected = std::find_if(
            runs_.begin(), runs_.end(), [this](const RaceViewerRun &run) {
                return run.id == selectedRunId_;
            });
    return selected == runs_.end() ? nullptr : &*selected;
}

RaceViewerRun *RaceViewerController::selectedRun() noexcept {
    const auto selected = std::find_if(
            runs_.begin(), runs_.end(), [this](const RaceViewerRun &run) {
                return run.id == selectedRunId_;
            });
    return selected == runs_.end() ? nullptr : &*selected;
}

void RaceViewerController::upsertRun(QString id,
                                     QString name,
                                     std::vector<RaceViewerFrame> frames,
                                     std::vector<SandboxInputEvent> inputs,
                                     bool select,
                                     std::shared_ptr<ManualDriveRuntime> runtime) {
    if (frames.empty()) {
        return;
    }
    ++runGeometryRevision_;
    if (id == QStringLiteral("best")) {
        ++bestRunRevision_;
        updateBestTrajectory(name, frames);
    }
    auto existing = std::find_if(
            runs_.begin(), runs_.end(), [&id](const RaceViewerRun &run) {
                return run.id == id;
            });
    const QString runId = id;
    if (existing == runs_.end()) {
        std::vector<RaceViewerSplit> checkpointSplits =
                BuildCheckpointSplits(frames);
        runs_.push_back({std::move(id),
                         std::move(name),
                         std::move(frames),
                         std::move(inputs),
                         {},
                         {},
                         std::move(checkpointSplits),
                         std::move(runtime)});
    } else {
        existing->name = std::move(name);
        existing->frames = std::move(frames);
        existing->inputs = std::move(inputs);
        existing->checkpointSplits =
                BuildCheckpointSplits(existing->frames);
        if (runtime != nullptr) {
            existing->runtime = std::move(runtime);
        }
    }
    emit runsChanged();

    if (selectedRunId_.isEmpty()) {
        selectedRunId_ = runId;
        emit selectedRunChanged();
    } else if (select && selectedRunId_ != runId) {
        setSelectedRunId(runId);
        return;
    }
    refreshSelectedRun();
    emit timelineChanged();
    emit timeChanged();
}

void RaceViewerController::refreshSelectedRun() {
    const RaceViewerRun *const run = selectedRun();
    durationMs_ = run == nullptr || run->frames.empty()
            ? 0
            : static_cast<qint64>(run->frames.back().timeMs);
    timeMs_ = std::clamp<qint64>(timeMs_, 0, durationMs_);
    updatePose();
}

void RaceViewerController::waitForWorker() {
    if (workerThread_ != nullptr) {
        workerThread_->requestInterruption();
        workerThread_->quit();
        workerThread_->wait();
        workerThread_ = nullptr;
    }
}

void RaceViewerController::updatePose() {
    if (runs_.empty()) {
        carPosition_ = {};
        carRotation_ = {};
        emit poseChanged();
        updateCarCamera();
        return;
    }
    for (RaceViewerRun &run : runs_) {
        UpdateRunPose(run, timeMs_);
    }
    const RaceViewerRun *const run = selectedRun();
    if (run != nullptr) {
        carPosition_ = run->position;
        carRotation_ = run->rotation;
    }
    emit poseChanged();
    updateCarCamera();
}

void RaceViewerController::updateCarCamera() {
    const RaceViewerRun *const run = selectedRun();
    QVector3D position;
    QQuaternion rotation;
    QVector3D target;
    double fieldOfView = 75.0;
    bool available = false;
    if (cameraRuntime_ != nullptr && run != nullptr) {
        try {
            available = cameraRuntime_->Evaluate(
                    *run, cameraPreset_, timeMs_, &position, &rotation,
                    &target, &fieldOfView);
        } catch (...) {
            cameraRuntime_ = cameraResources_
                    ? std::make_unique<RaceCameraRuntime>(cameraResources_)
                    : nullptr;
        }
    }
    const bool changed =
            carCameraAvailable_ != available ||
            (available &&
             (carCameraPosition_ != position ||
              carCameraRotation_ != rotation ||
              carCameraTarget_ != target ||
              carCameraFieldOfView_ != fieldOfView));
    carCameraAvailable_ = available;
    if (available) {
        carCameraPosition_ = position;
        carCameraRotation_ = rotation;
        carCameraTarget_ = target;
        carCameraFieldOfView_ = fieldOfView;
    }
    if (changed) {
        emit cameraChanged();
    }
}

void RaceViewerController::advancePlayback() {
    const RaceViewerRun *const run = selectedRun();
    if (!playing_ || run == nullptr || run->frames.empty()) {
        return;
    }
    const qint64 elapsedTicks = playbackClock_.elapsed() /
            static_cast<qint64>(kViewerTickDurationMs);
    const qint64 targetTick = playbackStartTick_ + elapsedTicks;
    if (targetTick >= tickCount() - 1) {
        setTimeMs(durationMs_);
        pause();
        return;
    }
    setCurrentTick(targetTick);
}

void RaceViewerController::appendSimulationDebuggerFrame(
        const QVariantMap &frame) {
    const QVariantList position =
            frame.value(QStringLiteral("position")).toList();
    const QVariantList rotation =
            frame.value(QStringLiteral("rotation")).toList();
    const QVariantList linearSpeed =
            frame.value(QStringLiteral("linearSpeed")).toList();
    if (position.size() != 3 || rotation.size() != 4) {
        setStatusText(QStringLiteral(
                "Native debugger returned an invalid car pose."));
        simulationDebugger_.pause();
        return;
    }

    const RaceViewerFrame viewerFrame{
            frame.value(QStringLiteral("timeMs")).toLongLong(),
            QVector3D(
                    position[0].toFloat(),
                    position[1].toFloat(),
                    position[2].toFloat()),
            QQuaternion(
                    rotation[3].toFloat(),
                    rotation[0].toFloat(),
                    rotation[1].toFloat(),
                    rotation[2].toFloat())
                    .normalized(),
            frame.value(QStringLiteral("accelerate")).toFloat(),
            frame.value(QStringLiteral("brake")).toFloat(),
            frame.value(QStringLiteral("steering")).toFloat(),
            frame.value(QStringLiteral("checkpointsCollected")).toUInt(),
            frame.value(QStringLiteral("checkpointsTotal")).toUInt(),
            frame.value(QStringLiteral("completedLaps")).toUInt(),
            frame.value(QStringLiteral("totalLaps")).toUInt(),
            frame.value(QStringLiteral("raceCompleted")).toBool(),
            frame.contains(QStringLiteral("finishTimeMs"))
                    ? std::optional<std::uint32_t>(
                              frame.value(QStringLiteral("finishTimeMs"))
                                      .toUInt())
                    : std::nullopt,
            linearSpeed.size() == 3
                    ? QVector3D(linearSpeed[0].toFloat(),
                                linearSpeed[1].toFloat(),
                                linearSpeed[2].toFloat())
                    : QVector3D{},
            0.0f,
            0.0f,
            0.0f,
            false,
            false,
            {{true, true, true, true}},
            {{true, true, true, true}},
            QVector3D(0.0f, 1.0f, 0.0f),
            frame.contains(QStringLiteral("stuntsScore"))
                    ? std::optional<std::uint32_t>(
                              frame.value(QStringLiteral("stuntsScore"))
                                      .toUInt())
                    : std::nullopt};

    auto found = std::find_if(
            runs_.begin(), runs_.end(), [](const RaceViewerRun &run) {
                return run.id == QStringLiteral("debug");
            });
    if (found == runs_.end()) {
        std::vector<RaceViewerSplit> checkpointSplits =
                BuildCheckpointSplits({viewerFrame});
        runs_.push_back(
                {QStringLiteral("debug"),
                 QStringLiteral("Reference source"),
                 {viewerFrame},
                 {},
                 {},
                 {},
                 std::move(checkpointSplits),
                 {}});
        found = std::prev(runs_.end());
        emit runsChanged();
    } else if (
            !found->frames.empty() &&
            found->frames.back().timeMs == viewerFrame.timeMs) {
        found->frames.back() = viewerFrame;
        found->checkpointSplits =
                BuildCheckpointSplits(found->frames);
    } else if (
            found->frames.empty() ||
            found->frames.back().timeMs < viewerFrame.timeMs) {
        const std::uint32_t previousCheckpointCount =
                found->frames.empty()
                ? 0u
                : found->frames.back().checkpointsCollected;
        found->frames.push_back(viewerFrame);
        AppendCheckpointTransitions(
                found->checkpointSplits,
                previousCheckpointCount,
                viewerFrame);
    } else {
        setStatusText(QStringLiteral(
                "Native debugger returned a non-monotonic "
                "simulation frame."));
        simulationDebugger_.pause();
        return;
    }

    if (selectedRunId_ != QStringLiteral("debug")) {
        selectedRunId_ = QStringLiteral("debug");
        emit selectedRunChanged();
    }
    durationMs_ = std::max<qint64>(
            static_cast<qint64>(found->frames.back().timeMs),
            frame.value(QStringLiteral("horizonMs")).toLongLong());
    timeMs_ = found->frames.back().timeMs;
    updatePose();
    setStatusText(simulationDebugger_.statusText());
    emit timelineChanged();
    emit timeChanged();
}

void RaceViewerController::advanceManualDrive() {
    if (!manualDriving_ || manualRuntime_ == nullptr) {
        return;
    }
    RaceViewerRun *const run = selectedRun();
    if (run == nullptr || run->id != QStringLiteral("manual") ||
        run->frames.empty()) {
        finishManualDrive(
                QStringLiteral("Manual drive stopped: run is unavailable."),
                false);
        return;
    }

    const qint64 targetTick = manualDriveStartTick_ +
            manualDriveClock_.elapsed() /
                    static_cast<qint64>(kViewerTickDurationMs);
    const qint64 currentTick =
            static_cast<qint64>(manualRuntime_->state.tick);
    const qint64 steps = std::min<qint64>(
            std::max<qint64>(0, targetTick - currentTick), 32);
    bool changed = false;
    for (qint64 step = 0; step < steps; ++step) {
        if (manualRuntime_->state.timeMs >=
            static_cast<std::uint64_t>(simulationHorizonMs_)) {
            finishManualDrive(
                    QStringLiteral("Manual drive complete"), true);
            break;
        }
        auto advanced = manualRuntime_->sandbox.AdvanceTicks(1u);
        if (!advanced) {
            finishManualDrive(
                    QStringLiteral("Manual drive failed: %1")
                            .arg(SandboxErrorText(advanced.Error())),
                    false);
            break;
        }
        manualRuntime_->state = advanced.Value();
        const RaceViewerFrame viewerFrame =
                ToViewerFrame(manualRuntime_->state);
        const std::uint32_t previousCheckpointCount =
                run->frames.back().checkpointsCollected;
        run->frames.push_back(viewerFrame);
        AppendCheckpointTransitions(
                run->checkpointSplits,
                previousCheckpointCount,
                viewerFrame);
        durationMs_ = static_cast<qint64>(
                manualRuntime_->state.timeMs);
        timeMs_ = durationMs_;
        changed = true;
        if (manualRuntime_->state.raceCompleted) {
            finishManualDrive(
                    QStringLiteral("Manual drive complete"), true);
            break;
        }
    }
    if (changed) {
        updatePose();
        emit timelineChanged();
        emit timeChanged();
    }
}

bool RaceViewerController::replaceManualInputs() {
    if (manualRuntime_ == nullptr) {
        return false;
    }
    std::vector<PhysicsSandboxInputEvent> inputs =
            effectiveManualInputs();
    auto replaced =
            manualRuntime_->sandbox.ReplaceInputs(std::move(inputs));
    if (!replaced) {
        setStatusText(
                QStringLiteral("Manual drive failed: %1")
                        .arg(SandboxErrorText(replaced.Error())));
        return false;
    }
    return true;
}

void RaceViewerController::appendHeldManualInputs(std::int32_t timeMs) {
    if (manualRuntime_ == nullptr) {
        return;
    }
    const std::array<std::pair<bool, PhysicsSandboxInputAction>, 4> held{{
            {manualLeft_, PhysicsSandboxInputAction::SteerLeft},
            {manualRight_, PhysicsSandboxInputAction::SteerRight},
            {manualAccelerate_, PhysicsSandboxInputAction::Accelerate},
            {manualBrake_, PhysicsSandboxInputAction::Brake},
    }};
    for (const auto &[active, action] : held) {
        if (active) {
            manualRuntime_->driverInputs.push_back(
                    ManualSwitchEvent(timeMs, action, true));
        }
    }
}

void RaceViewerController::finishManualDrive(
        const QString &status,
        bool releaseInputs) {
    if (!manualDriving_) {
        return;
    }
    if (releaseInputs) {
        releaseManualInputs();
        if (!manualDriving_) {
            return;
        }
    }
    manualDriveTimer_.stop();
    manualDriving_ = false;
    auto manualRun = std::find_if(
            runs_.begin(), runs_.end(), [](const RaceViewerRun &run) {
                return run.id == QStringLiteral("manual");
            });
    if (manualRun != runs_.end()) {
        manualRun->inputs = effectiveManualInputs();
        emit runsChanged();
    }
    resetManualInputState();
    setStatusText(status);
    emit manualDrivingChanged();
    scheduleInputPreviewRebuild();
    scheduleStoredRunRebuilds();
}

void RaceViewerController::resetManualInputState() {
    const bool changed = manualLeft_ || manualRight_ ||
            manualAccelerate_ || manualBrake_;
    manualLeft_ = false;
    manualRight_ = false;
    manualAccelerate_ = false;
    manualBrake_ = false;
    if (changed) {
        emit manualInputChanged();
    }
}

void RaceViewerController::resetManualTakeoverState() {
    const bool changed = manualTakeover_ ||
            steeringTakeoverTimeMs_.has_value() ||
            longitudinalTakeoverTimeMs_.has_value();
    manualTakeover_ = false;
    takeoverSourceInputs_.clear();
    takeoverSourceRunId_.clear();
    steeringTakeoverTimeMs_.reset();
    longitudinalTakeoverTimeMs_.reset();
    manualDriveStartTick_ = 0;
    if (changed) {
        emit manualInputChanged();
    }
}

void RaceViewerController::setPlaying(bool value) {
    if (playing_ == value) {
        return;
    }
    playing_ = value;
    emit playbackChanged();
}

}  // namespace forevertas::viewer
