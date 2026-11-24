// Car Cam III - AML mod for GTA III Android (libR1.so, armv7)
//
// Makes the car chase camera lower, like in San Andreas.
// CCam::Process_Cam_On_A_String (the vehicle camera) calls CCam::WorkOutCamHeight(this, &lookAt, orient, carHeight)
// right after it has put the camera behind the car. At that point
//   * the camera height is CCam+340 (Source.z)
//   * lookAt (the point the camera looks at) is a stack vector = car position + car height - 0.1 (roof level)
// and everything after it (obstruction check, Front/Up vectors) uses them. So after the original returns we
//   1. lower Source.z by HeightDrop (never below car position + MinAboveCar), and
//   2. lower lookAt.z by LookDown,
// which flattens the view angle: the car sits lower in the picture and the horizon is higher.

#include <mod/amlmod.h>
#include <mod/logger.h>
#include <mod/config.h>
#include <mod/flogger.h>
#include <stdint.h>
#include <math.h>
#include <stdlib.h>
#include <time.h>

MYMODCFG(com.carcam3.aml, Car Cam III, 1.2.0, CarCamIII)

static FLogger* fl = nullptr;
#define FLOG(...) do { if (fl) fl->Info(__VA_ARGS__); } while (0)

struct V3 { float x, y, z; };
typedef void (*WorkOut_t)(void* cam, V3* lookAt, float orient, float carHeight);
static WorkOut_t WorkOut_orig;

static bool  g_enabled   = true;
static float g_drop      = 0.8f;   // how much the camera is lowered (metres)
static float g_lookDown  = 0.6f;   // how much the look-at point is lowered (metres)
static float g_minAbove  = 0.8f;   // camera never goes below car centre + this
static float g_extraDist = 0.0f;   // metres added to the horizontal camera distance (negative = closer)
static int   g_logged    = 0;

// ---------------------------------------------------------------------------------------------
// FreeCarCam: swipe the right part of the screen while driving to orbit the camera around the car
// (horizontal = yaw, vertical = elevation). The offsets live only in this mod: the camera "source" the
// game keeps between frames is restored before CCam::Process runs, so the game's own camera never
// sees the offset (no drift / feedback). The game's own horizontal look swipe is switched off.
// ---------------------------------------------------------------------------------------------
static bool  g_free       = true;
static int   g_returnMs   = 1500;   // 0 = camera returns at once when the finger is lifted
static float g_sens       = 1.0f;
static bool  g_invX = false, g_invY = false;
static float g_zoneX      = 0.5f;   // touches that start right of this fraction of screen width
static bool  g_exclBtn    = true;
static float g_minPitch   = -5.f;   // degrees (negative = lower than the default angle)
static float g_maxPitch   = 60.f;

typedef int  (*PtrButton_t)(int, int);
typedef int  (*PtrCoords_t)(int, int*, int*, float*);
typedef int  (*ScreenSize_t)(void);
typedef int  (*TsHit_t)(void* ts, float x, float y);
typedef void (*Process_t)(void* cam);
static PtrButton_t PtrButton; static PtrCoords_t PtrCoords; static ScreenSize_t ScrW, ScrH;
static uint8_t** pTouchscreen; static float* pAdjustCam;
static TsHit_t GetButtonAtFn, GetHUDAtFn;
static Process_t Process_orig;

static float g_yaw = 0.f, g_pitch = 0.f;           // current offsets (radians)
static double g_lastTouchMs = 0, g_lastFrameMs = 0, g_lastWorkMs = 0;
static bool  g_applied = false; static void* g_cam = nullptr; static float g_saved[3];

static double NowMs() { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000.0 + t.tv_nsec / 1e6; }

static bool OnButton(int x, int y)
{
    uint8_t* ts = pTouchscreen ? *pTouchscreen : nullptr;
    if (!ts) return false;
    float W = (float)ScrW(), H = (float)ScrH();
    for (int k = 0; k < 2; k++) {
        TsHit_t fn = k == 0 ? GetButtonAtFn : GetHUDAtFn;
        if (!fn) continue;
        int i = fn(ts, (float)x, (float)y);
        if (i < 0 || i >= 32) continue;
        uint8_t* e = *(uint8_t**)(ts + 0x10 + 4 * i);
        if (!e) continue;
        float ew = *(float*)(e + 12), eh = *(float*)(e + 16);
        if (ew > W * 0.3f || eh > H * 0.5f) continue;
        return true;
    }
    return false;
}

struct Track { bool active = false, mine = false; int lx = 0, ly = 0; };
static Track tr[4];
// returns true while at least one of our fingers is down; dx,dy = pixel movement this frame
static bool Touches(float& dx, float& dy)
{
    float W = (float)ScrW();
    bool any = false; dx = dy = 0.f;
    for (int id = 0; id < 4; id++) {
        Track& t = tr[id];
        int st = PtrButton(id, 0), x = 0, y = 0;
        if (st < 2 || !PtrCoords(id, &x, &y, nullptr)) { t.active = false; t.mine = false; continue; }
        if (!t.active || st == 2) {
            t.active = true; t.lx = x; t.ly = y;
            t.mine = (float)x > W * g_zoneX && !(g_exclBtn && OnButton(x, y));
            continue;
        }
        if (t.mine) { dx += (float)(x - t.lx); dy += (float)(y - t.ly); any = true; }
        t.lx = x; t.ly = y;
    }
    return any;
}

static void RestoreCam(void* cam)
{
    if (g_applied && cam == g_cam) {
        float* s = (float*)((uint8_t*)cam + 332);
        s[0] = g_saved[0]; s[1] = g_saved[1]; s[2] = g_saved[2];
        g_applied = false;
    }
}

static void Process_hook(void* cam)
{
    RestoreCam(cam);
    if (g_free && cam) {
        uint16_t mode = *(uint16_t*)((uint8_t*)cam + 0xc);
        if ((mode == 16 || mode == 18 || mode == 22) && pAdjustCam) pAdjustCam[0] = 0.f;   // game's own look-offset swipe off
    }
    Process_orig(cam);
}

static void FreeCam(void* cam, V3* lookAt)
{
    double now = NowMs();
    float dt = g_lastFrameMs > 0 ? (float)((now - g_lastFrameMs) / 1000.0) : 0.016f;
    if (dt > 0.1f) dt = 0.1f; if (dt < 0.f) dt = 0.f;
    if (now - g_lastWorkMs > 500.0) { g_yaw = g_pitch = 0.f; for (int i = 0; i < 4; i++) tr[i].active = false; }
    g_lastFrameMs = now; g_lastWorkMs = now;

    float dx, dy;
    if (Touches(dx, dy)) {
        uint8_t* ts = pTouchscreen ? *pTouchscreen : nullptr;
        float scale = ts ? *(float*)(ts + 0x138) : 1.f;
        if (scale < 0.01f) scale = 1.f;
        float k = 0.006f / scale * g_sens;
        g_yaw   += (g_invX ? -dx : dx) * k;
        g_pitch += (g_invY ? -dy : dy) * k;
        g_lastTouchMs = now;
    } else if (g_yaw != 0.f || g_pitch != 0.f) {
        if (g_returnMs <= 0) { g_yaw = g_pitch = 0.f; }
        else if (now - g_lastTouchMs >= (double)g_returnMs) {
            float f = expf(-dt * 8.f);                      // smooth return (~0.3 s)
            g_yaw *= f; g_pitch *= f;
            if (fabsf(g_yaw) < 0.002f) g_yaw = 0.f;
            if (fabsf(g_pitch) < 0.002f) g_pitch = 0.f;
        }
    }
    const float D2R = 3.14159265f / 180.f;
    if (g_pitch > g_maxPitch * D2R) g_pitch = g_maxPitch * D2R;
    if (g_pitch < g_minPitch * D2R) g_pitch = g_minPitch * D2R;
    if (g_yaw >  3.14159f) g_yaw =  3.14159f;
    if (g_yaw < -3.14159f) g_yaw = -3.14159f;

    float* s = (float*)((uint8_t*)cam + 332);
    g_saved[0] = s[0]; g_saved[1] = s[1]; g_saved[2] = s[2]; g_cam = cam; g_applied = true;
    if (g_yaw == 0.f && g_pitch == 0.f) return;

    float ox = s[0] - lookAt->x, oy = s[1] - lookAt->y, oz = s[2] - lookAt->z;
    float d = sqrtf(ox * ox + oy * oy);
    float r = sqrtf(d * d + oz * oz);
    if (r < 0.5f) return;
    float elev = atan2f(oz, d) + g_pitch;                   // swipe down -> higher camera
    if (elev > 85.f * D2R) elev = 85.f * D2R;
    if (elev < -20.f * D2R) elev = -20.f * D2R;
    float nd = r * cosf(elev), nz = r * sinf(elev);
    float ux = d > 0.001f ? ox / d : 0.f, uy = d > 0.001f ? oy / d : -1.f;
    float c = cosf(g_yaw), sn = sinf(g_yaw);                // clockwise from above: swipe right -> view turns right
    float rx = ux * c + uy * sn, ry = -ux * sn + uy * c;
    s[0] = lookAt->x + rx * nd;
    s[1] = lookAt->y + ry * nd;
    s[2] = lookAt->z + nz;
}

static void WorkOut_hook(void* cam, V3* lookAt, float orient, float carHeight)
{
    WorkOut_orig(cam, lookAt, orient, carHeight);
    if (!g_enabled || !cam || !lookAt) return;
    float* srcX = (float*)((uint8_t*)cam + 332);
    float* srcY = (float*)((uint8_t*)cam + 336);
    float* srcZ = (float*)((uint8_t*)cam + 340);
    if (g_extraDist != 0.f) {
        float dx = *srcX - lookAt->x, dy = *srcY - lookAt->y;
        float d = sqrtf(dx * dx + dy * dy);
        if (d > 0.1f) {
            float nd = d + g_extraDist; if (nd < 2.0f) nd = 2.0f;
            *srcX = lookAt->x + dx / d * nd;
            *srcY = lookAt->y + dy / d * nd;
            if (g_logged < 3) FLOG("car cam: distance %.2f -> %.2f", d, nd);
        }
    }
    float carZ = lookAt->z - carHeight + 0.1f;           // car centre height
    float z = *srcZ - g_drop;
    float minZ = carZ + g_minAbove;
    if (z < minZ) z = (*srcZ < minZ) ? *srcZ : minZ;     // never push the camera up or below the limit
    if (g_logged++ < 3) FLOG("car cam: source.z %.2f -> %.2f, look.z %.2f -> %.2f (car z %.2f)", *srcZ, z, lookAt->z, lookAt->z - g_lookDown, carZ);
    *srcZ = z;
    lookAt->z -= g_lookDown;
    if (g_free && PtrButton) FreeCam(cam, lookAt);
}

#define CFG_BOOL(var, key) do { (var) = cfg->Bind(key, (bool)(var))->GetBool(); delete Config::pLastEntry; } while (0)
#define CFG_FLT(var, key)  do { (var) = cfg->Bind(key, (float)(var))->GetFloat(); delete Config::pLastEntry; } while (0)

ON_MOD_LOAD()
{
    logger->SetTag("CarCamIII");
    fl = new FLogger("CarCamIII.log");
    FLOG("OnModLoad start");
    void* h = aml->GetLibHandle("libR1.so");
    if (!h) { FLOG("libR1.so not found"); return; }
    uintptr_t base = aml->GetLib("libR1.so");
    void* hw = (void*)aml->GetSym(h, "_ZN4CCam16WorkOutCamHeightERK7CVectorff");
    if (!hw && base && *(uint16_t*)(base + 0x1a3c78) == 0xe92d) hw = (void*)(base + 0x1a3c79);   // push.w {r4-r7,r8,r9,lr}
    if (!hw) { FLOG("WorkOutCamHeight not found"); return; }

    CFG_BOOL(g_enabled,  "Enabled");
    CFG_FLT (g_drop,     "HeightDrop");
    CFG_FLT (g_lookDown, "LookDown");
    CFG_FLT (g_minAbove, "MinAboveCar");
    CFG_FLT (g_extraDist,"ExtraDistance");
    CFG_BOOL(g_free,     "FreeCarCam");
    g_returnMs = cfg->Bind("FreeCarCamReturnMs", g_returnMs)->GetInt(); delete Config::pLastEntry;
    CFG_FLT (g_sens,     "FreeCarCamSensitivity");
    CFG_BOOL(g_invX,     "FreeCarCamInvertX");
    CFG_BOOL(g_invY,     "FreeCarCamInvertY");
    CFG_FLT (g_zoneX,    "FreeCarCamZoneStartX");
    CFG_BOOL(g_exclBtn,  "IgnoreTouchOnButtons");
    CFG_FLT (g_minPitch, "FreeCarCamMinPitch");
    CFG_FLT (g_maxPitch, "FreeCarCamMaxPitch");
    cfg->Save();
    if (g_returnMs < 0) g_returnMs = 0;
    if (g_free) {
        PtrButton = (PtrButton_t)aml->GetSym(h, "_Z20LIB_PointerGetButtonii");
        PtrCoords = (PtrCoords_t)aml->GetSym(h, "_Z25LIB_PointerGetCoordinatesiPiS_Pf");
        ScrW = (ScreenSize_t)aml->GetSym(h, "_Z17OS_ScreenGetWidthv");
        ScrH = (ScreenSize_t)aml->GetSym(h, "_Z18OS_ScreenGetHeightv");
        pTouchscreen = (uint8_t**)aml->GetSym(h, "GTouchscreen");
        pAdjustCam = (float*)aml->GetSym(h, "Mobile_AdjustCam");
        GetButtonAtFn = (TsHit_t)aml->GetSym(h, "_ZN11Touchscreen11GetButtonAtEff");
        GetHUDAtFn    = (TsHit_t)aml->GetSym(h, "_ZN11Touchscreen15GetHUDElementAtEff");
        if (!GetButtonAtFn && base && *(uint16_t*)(base + 0x118190) == 0xb570) GetButtonAtFn = (TsHit_t)(base + 0x118191);
        if (!GetHUDAtFn    && base && *(uint16_t*)(base + 0x1192b0) == 0xee01) GetHUDAtFn    = (TsHit_t)(base + 0x1192b1);
        void* hProc = (void*)aml->GetSym(h, "_ZN4CCam7ProcessEv");
        if (!PtrButton || !PtrCoords || !ScrW || !ScrH || !pTouchscreen || !pAdjustCam || !hProc ||
            !aml->Hook(hProc, (void*)Process_hook, (void**)&Process_orig)) { FLOG("FreeCarCam unavailable (symbols/hook)"); g_free = false; }
        else FLOG("FreeCarCam on: returnMs=%d sens=%.2f", g_returnMs, g_sens);
    }
    if (!aml->Hook(hw, (void*)WorkOut_hook, (void**)&WorkOut_orig)) { FLOG("hook failed"); return; }
    FLOG("Car Cam III loaded: drop=%.2f lookDown=%.2f minAbove=%.2f", g_drop, g_lookDown, g_minAbove);
}
