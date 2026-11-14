#ifndef FOREVERTAS_RENDER_MODULE_API_H
#define FOREVERTAS_RENDER_MODULE_API_H

/* Optional render modules for the ForeverTAS race view.
 *
 * A build may ship a shared library named forevertas-render-module next to
 * the executable. When it loads, the mode it offers joins the race view's
 * Render list: the module draws the loaded map, and the selected run's car,
 * from the race view's camera into the race view's own OpenGL framebuffer,
 * in the same frame as the race view, and the race view shows its other
 * cars and overlays over it. It can also render a run to a video file.
 * Builds without the library simply do not offer the mode.
 *
 * Only C types cross this boundary, so the library may come from another
 * compiler.
 */

#ifdef __cplusplus
extern "C" {
#endif

#define FTRM_API_VERSION 2

typedef struct FtrmCamera {
    float position[3];
    float target[3];
    float up[3];
    float verticalFovDegrees;
    float nearClip;
    float farClip;
} FtrmCamera;

/* A control of the run, as TMUF records it: switches are 1 (pressed) or 0,
 * Steer and Gas are analog from -65536 to 65536. timeMs is race time, on
 * the 10 ms tick grid. */
enum {
    FTRM_ACCELERATE = 0,
    FTRM_BRAKE = 1,
    FTRM_STEER_LEFT = 2,
    FTRM_STEER_RIGHT = 3,
    FTRM_STEER = 4,
    FTRM_GAS = 5,
    FTRM_RESPAWN = 6
};

typedef struct FtrmControl {
    int timeMs;
    int control;
    int value;
} FtrmControl;

typedef struct FtrmFrame {
    FtrmCamera camera;
    /* Race time shown, in milliseconds; fractions blend between ticks. */
    double raceTimeMs;
    /* Draw the run's car (game skin, shadow, effects). */
    int showCar;
} FtrmFrame;

/* The car of the last frame drawn, for the host's diagnostics (optional:
 * ftrm_car_state may be missing from older modules). */
typedef struct FtrmCarState {
    /* Race time the frame showed, and the module's race tick then. */
    double raceTimeMs;
    unsigned tick;
    /* Where the module's own simulation has the car at that tick. */
    float position[3];
    /* 1 when the frame was asked to draw the car (FtrmFrame::showCar). */
    int shown;
    /* Controls the module simulates; frames drawn so far. */
    int controlCount;
    unsigned framesDrawn;
} FtrmCarState;

typedef struct FtrmModule FtrmModule;

/* FTRM_API_VERSION the library was built against. */
typedef int (*FtrmApiVersionFunction)(void);
/* The mode's name in the Render list (UTF-8). */
typedef const char *(*FtrmModeLabelFunction)(void);

/* Opens the module for an installation's Packs directory; cacheDirectory
 * (may be empty) keeps work worth reusing between runs. No OpenGL; any
 * thread. Null on failure, with a message in error. */
typedef FtrmModule *(*FtrmCreateFunction)(const char *packsDirectory,
                                          const char *cacheDirectory,
                                          char *error, int errorSize);
/* Reads the map of a Challenge.Gbx or Replay.Gbx (UTF-8 path) and bakes
 * its lighting into the cache unless already there: the slow part of a
 * load. Any thread, but not while the module is used elsewhere. With
 * useCurrentContext, it bakes with the OpenGL 3.3+ context current on the
 * calling thread (a context of the host's other than the one it renders
 * with); otherwise with one of its own. Returns 1 on success. */
typedef int (*FtrmPrepareFunction)(FtrmModule *module, const char *mapPath,
                                   int useCurrentContext, char *error,
                                   int errorSize);

/* The calls below need the host's OpenGL 3.3+ context current, the same
 * one each time, on the thread that owns it. */

/* Uploads the prepared map and bakes its lighting unless cached. */
typedef int (*FtrmUploadFunction)(FtrmModule *module, char *error,
                                  int errorSize);
/* The run whose car is drawn. */
typedef void (*FtrmSetControlsFunction)(FtrmModule *module,
                                        const FtrmControl *controls,
                                        int count);
/* Draws a frame into framebuffer (of width x height pixels, with depth),
 * leaving OpenGL state modified. Returns 1 on success. */
typedef int (*FtrmRenderFunction)(FtrmModule *module, const FtrmFrame *frame,
                                  unsigned framebuffer, int width, int height,
                                  char *error, int errorSize);
/* Deletes the module's OpenGL objects. */
typedef void (*FtrmReleaseFunction)(FtrmModule *module);
/* The last frame's car (FtrmCarState); 0 before the first frame. Call it
 * where ftrm_render is called (the same thread and context). */
typedef int (*FtrmCarStateFunction)(FtrmModule *module, FtrmCarState *state);

/* No OpenGL; frees the module. */
typedef void (*FtrmDestroyFunction)(FtrmModule *module);

/* Renders a run to an MP4 file as TMUF shows a replay: countdown, race
 * camera, HUD and the game's car. Any thread; blocks until done. Draws with
 * the context current on the calling thread when useCurrentContext is set
 * (as for FtrmPrepareFunction), otherwise with one of its own. progress (may be null) gets the frames done so far;
 * a non-zero *cancel stops it. Returns 1 when the file is complete. */
typedef struct FtrmVideoSettings {
    const char *packsDirectory;
    /* As for FtrmCreateFunction; may be empty. */
    const char *cacheDirectory;
    const char *mapPath;
    const char *outputPath;
    const FtrmControl *controls;
    int controlCount;
    /* Race time to stop at, in milliseconds. */
    int durationMs;
    /* TMUF's race camera: 1, 2 or 3. */
    int camera;
    int width;
    int height;
    int framesPerSecond;
    /* Video bit rate in kilobits per second; 0 picks one for the size. */
    int bitrateKbps;
    int useCurrentContext;
} FtrmVideoSettings;

typedef void (*FtrmVideoProgress)(void *user, int framesDone, int frameCount);
typedef int (*FtrmExportVideoFunction)(const FtrmVideoSettings *settings,
                                       FtrmVideoProgress progress, void *user,
                                       const volatile int *cancel,
                                       char *error, int errorSize);

#ifdef __cplusplus
}
#endif

#endif
