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

MYMODCFG(com.carcam3.aml, Car Cam III, 1.0.0, CarCamIII)

static FLogger* fl = nullptr;
#define FLOG(...) do { if (fl) fl->Info(__VA_ARGS__); } while (0)

struct V3 { float x, y, z; };
typedef void (*WorkOut_t)(void* cam, V3* lookAt, float orient, float carHeight);
static WorkOut_t WorkOut_orig;

static bool  g_enabled   = true;
static float g_drop      = 0.8f;   // how much the camera is lowered (metres)
static float g_lookDown  = 0.6f;   // how much the look-at point is lowered (metres)
static float g_minAbove  = 0.8f;   // camera never goes below car centre + this
static int   g_logged    = 0;

static void WorkOut_hook(void* cam, V3* lookAt, float orient, float carHeight)
{
    WorkOut_orig(cam, lookAt, orient, carHeight);
    if (!g_enabled || !cam || !lookAt) return;
    float* srcZ = (float*)((uint8_t*)cam + 340);
    float carZ = lookAt->z - carHeight + 0.1f;           // car centre height
    float z = *srcZ - g_drop;
    float minZ = carZ + g_minAbove;
    if (z < minZ) z = (*srcZ < minZ) ? *srcZ : minZ;     // never push the camera up or below the limit
    if (g_logged++ < 3) FLOG("car cam: source.z %.2f -> %.2f, look.z %.2f -> %.2f (car z %.2f)", *srcZ, z, lookAt->z, lookAt->z - g_lookDown, carZ);
    *srcZ = z;
    lookAt->z -= g_lookDown;
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
    cfg->Save();
    if (!aml->Hook(hw, (void*)WorkOut_hook, (void**)&WorkOut_orig)) { FLOG("hook failed"); return; }
    FLOG("Car Cam III loaded: drop=%.2f lookDown=%.2f minAbove=%.2f", g_drop, g_lookDown, g_minAbove);
}
