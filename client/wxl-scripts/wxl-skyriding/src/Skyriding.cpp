// wxl-skyriding: AdvFly FSM + Space/X ASC/DESC gate for Horizon skyriding.
// Copyright (C) 2026. GPLv3 (see WarcraftXL LICENSE).
//
// Anim ownership: SetBoneSequence on transitions + ResolveModelAnimationId override
// (via wxl-anim-limit) so the client cannot stomp AdvFly with Fly/Run/Fall every frame.

#include "common/Log.hpp"
#include "core/Hook.hpp"
#include "core/Logger.hpp"
#include "events/EventScript.hpp"
#include "game/world/World.hpp"
#include "runtime/AnimResolveOverride.hpp"
#include "runtime/LuaBindings.hpp"
#include "runtime/ModuleInstall.hpp"

#include <Windows.h>

#include <cmath>
#include <cstdint>
#include <cstring>

namespace
{
    namespace wlua = wxl::runtime::lua;
    namespace world = wxl::game::world;
    namespace ev = wxl::events;

    constexpr uintptr_t kCGUnitSetBoneSequence = 0x00735820;
    constexpr uintptr_t kCGUnitAnimationData = 0x007385C0; // high-level anim (needed for clean one-shots)
    constexpr uintptr_t kM2DataHasSequenceById = 0x00825E00;

    constexpr size_t kUnitModelOffset = 0xB4;
    constexpr size_t kModelParentOffset = 0x48;
    constexpr size_t kUnitDescriptorsOffset = 0x8;
    constexpr size_t kMountDisplayIdDescOff = 0x214;
    constexpr size_t kUnitPositionOffset = 0x798; // x,y,z floats
    constexpr size_t kUnitFacingOffset = 0x7A8;
    constexpr size_t kUnitPitchOffset = 0x7AC;
    constexpr size_t kUnitMovementFlagsOffset = 0x7CC;
    constexpr size_t kOffModelShared = 44;
    constexpr size_t kOffSharedM2Data = 336;

    constexpr uint32_t MOVE_FLAG_FORWARD = 0x00000001;
    constexpr uint32_t MOVE_FLAG_BACKWARD = 0x00000002;
    constexpr uint32_t MOVE_FLAG_PENDING_STOP = 0x00004000;
    constexpr uint32_t MOVE_FLAG_FALLING = 0x00001000;
    constexpr uint32_t MOVE_FLAG_FALLING_FAR = 0x00002000;
    constexpr uint32_t MOVE_FLAG_DISABLE_GRAV = 0x00000400; // NOT spline (0x04000000)
    constexpr uint32_t MOVE_FLAG_ASCENDING = 0x00400000;
    constexpr uint32_t MOVE_FLAG_DESCENDING = 0x00800000;
    constexpr uint32_t MOVE_FLAG_FLYING = 0x02000000;

    // Stall sink (yards/sec). DESCENDING also forced so engine physics participates.
    constexpr float kStallSinkPerSec = 8.0f;
    constexpr float kMinCoastYardsPerSec = 2.0f; // never integrate at 0
    constexpr float kBaseFlightYards = 7.0f;     // WotLK MOVE_FLIGHT base
    // Surge: brief Z lift without writing pitch (knockback was zeroing pitch to 0).
    constexpr float kSurgeLiftPerSec = 6.0f;
    constexpr DWORD kSurgeLiftMs = 400;

    // Knockback / surge / stall still need a ray clamp (engine may skip walls).
    // Coast must NOT write UnitPos — that tunnels through WMOs. Keep FORWARD via
    // CGInputControl::SetControlBit so the client moves with native collision.
    constexpr uintptr_t kTraceLine = 0x007A3B70;
    constexpr uintptr_t kInputControlPtr = 0x00C24954; // CGInputControl*
    constexpr uintptr_t kInputTimestamp = 0x00B499A4;
    constexpr uintptr_t kSetControlBit = 0x005FA170;
    constexpr uintptr_t kUnsetControlBit = 0x005FA450;
    constexpr uint32_t kControlBitForward = 0x10; // same bit MoveForwardStart uses
    // Models | WMO | unknown (bits accepted by TraceLine's 0x40F300FF gate).
    constexpr uint32_t kTraceHitFlags = 0x00000051;
    constexpr float kCollideSkinYards = 0.45f;
    constexpr float kCollideMaxStepYards = 28.0f;
    constexpr float kCollideProbeZ = 1.2f;

    // RidingWyvern AdvFly (patch-c). Durations from the live M2.
    constexpr int ANIM_DOWN = 1530;
    constexpr int ANIM_FORWARD_GLIDE = 1532;
    constexpr int ANIM_DOWN_START = 1678;
    constexpr int ANIM_FLAP_BIG = 1680;
    constexpr int ANIM_FLAP_UP = 1702;
    constexpr int ANIM_SLOW_FALL = 1704;
    constexpr int ANIM_FORWARD_GLIDE_SLOW = 1722;
    constexpr int ANIM_SECOND_FLAP_UP = 1726;

    // Durations from RidingWyvern.m2 (exact). For one-shots, subtract blendTime so the
    // next clip starts inside the native blend window (avoids hard pose snap).
    constexpr DWORD DUR_DOWN_START_MS = 1733 - 150; // 1678 blend 150
    constexpr DWORD DUR_FLAP_BIG_MS = 2700 - 100;   // 1680 blend 100 — was 2350, cut mid-flap
    constexpr DWORD DUR_FLAP_UP_MS = 900 - 150;     // 1702 blend 150
    constexpr DWORD DUR_FLAP_UP_2_MS = 2233 - 150;  // 1726 blend 150

    // Pitch dive enter/exit (radians). Small hysteresis — exit as soon as you pull up,
    // not when you reach ~0 (that felt like a long delay).
    constexpr float PITCH_DIVE_ENTER = -0.35f;
    constexpr float PITCH_DIVE_EXIT = -0.28f;


    // Cruise speed bands (MOVE_FLIGHT rate ≈ yards/sec / 7).
    constexpr float RATE_SLOW_ENTER = 1.55f;
    constexpr float RATE_SLOW_EXIT = 1.90f;

    using SetBoneSequenceFn = void(__thiscall*)(
        void* unit, uintptr_t model, int boneSlot, int animId,
        float seqTime, int a6, float speed, int a8, int a9, int a10);
    using AnimationDataFn = void(__thiscall*)(void* unit, int animId, char flags);
    using HasSequenceFn = bool(__stdcall*)(void* modelData, unsigned int animId);

    SetBoneSequenceFn g_setBoneSeq =
        reinterpret_cast<SetBoneSequenceFn>(kCGUnitSetBoneSequence);
    AnimationDataFn g_animData =
        reinterpret_cast<AnimationDataFn>(kCGUnitAnimationData);
    HasSequenceFn g_hasSequence =
        reinterpret_cast<HasSequenceFn>(kM2DataHasSequenceById);

    bool g_mode = false;
    bool g_classicVertical = false;
    float g_turnRate = 0.75f;
    float g_flightRateNorm = 1.0f;
    bool g_braking = false;
    bool g_stalled = false;
    float g_coastYardsPerSec = 2.5f * kBaseFlightYards; // ~250% until SPD arrives

    float g_lastFacing = 0.f;
    bool g_haveLastFacing = false;
    DWORD g_lastPhysicsMs = 0;
    float g_lastCollidePos[3] = {};
    bool g_haveLastCollidePos = false;
    bool g_forcedForwardBit = false; // we Set the FORWARD control bit for coast
    DWORD g_holdGroundUntilMs = 0;   // after LAND: refuse AdvFly even if FLYING flickers back
    bool g_prevFlying = false;       // rising edge → fall-into-fly dive
    DWORD g_lastFallingMs = 0;       // FLYING often clears FALLING the same frame — remember it

    enum class AnimPhase : uint8_t
    {
        Idle = 0,
        Cruise,
        DiveStart,
        Dive,
        DiveExit,
        SurgeFlap,
        SkywardFlap1,
        SkywardFlap2,
    };

    AnimPhase g_phase = AnimPhase::Idle;
    DWORD g_phaseUntilMs = 0;
    int g_forcedAnimId = -1;   // fed to Resolve override every engine resolve
    int g_boneAnimId = -1;     // last SetBoneSequence id
    float g_boneAnimSpeed = 1.0f;
    int g_cruiseTier = ANIM_FORWARD_GLIDE; // 1532 / 1722 / 1704 with hysteresis
    bool g_inDive = false;
    DWORD g_surgeLiftUntilMs = 0;
    float g_surgePitchLock = 0.f;
    bool g_surgePitchLocked = false;
    float g_skywardPitchLock = 0.f;
    bool g_skywardPitchLocked = false;
    void* g_forceUnit = nullptr;
    void* g_forceModel = nullptr;

    bool ModuleDisabled()
    {
        return GetFileAttributesA("WarcraftXL_skyriding.disable") != INVALID_FILE_ATTRIBUTES
            || GetFileAttributesA("WarcraftXL_wxl-skyriding.disable") != INVALID_FILE_ATTRIBUTES;
    }

    void* ActivePlayer()
    {
        const unsigned long long guid = world::ActivePlayerGuid();
        if (!guid)
            return nullptr;
        return world::ResolveObject(guid, world::kTypeMaskUnit | world::kTypeMaskPlayer);
    }

    void* UnitModel(void* unit)
    {
        if (!unit)
            return nullptr;
        return *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(unit) + kUnitModelOffset);
    }

    bool ModelHasSequence(void* model, unsigned int animId)
    {
        if (!model || !g_hasSequence)
            return false;
        const uintptr_t shared = *reinterpret_cast<uintptr_t*>(
            reinterpret_cast<uintptr_t>(model) + kOffModelShared);
        if (!shared)
            return false;
        void* modelData = *reinterpret_cast<void**>(shared + kOffSharedM2Data);
        if (!modelData)
            return false;
        return g_hasSequence(modelData, animId);
    }

    bool ModelSupportsAdvFly(void* model)
    {
        return ModelHasSequence(model, static_cast<unsigned int>(ANIM_FORWARD_GLIDE));
    }

    uint32_t PlayerMountDisplayId(void* player)
    {
        if (!player)
            return 0;
        const uintptr_t descriptors = *reinterpret_cast<uintptr_t*>(
            reinterpret_cast<uintptr_t>(player) + kUnitDescriptorsOffset);
        if (!descriptors)
            return 0;
        return *reinterpret_cast<uint32_t*>(descriptors + kMountDisplayIdDescOff);
    }

    void* AnimationModel(void* player)
    {
        void* body = UnitModel(player);
        if (!body)
            return nullptr;
        void* parent = *reinterpret_cast<void**>(
            reinterpret_cast<uintptr_t>(body) + kModelParentOffset);
        if (parent && ModelSupportsAdvFly(parent))
            return parent;
        if (ModelSupportsAdvFly(body))
            return body;
        if (parent && PlayerMountDisplayId(player) != 0)
            return parent;
        return body;
    }

    bool PlayerSeatedOnAdvFlyMount(void* player)
    {
        if (!player)
            return false;
        void* body = UnitModel(player);
        if (body)
        {
            void* parent = *reinterpret_cast<void**>(
                reinterpret_cast<uintptr_t>(body) + kModelParentOffset);
            if (parent && ModelSupportsAdvFly(parent))
                return true;
            if (parent && PlayerMountDisplayId(player) != 0)
                return true;
        }
        return PlayerMountDisplayId(player) != 0;
    }

    uint32_t& MovementFlags(void* unit)
    {
        return *reinterpret_cast<uint32_t*>(reinterpret_cast<uintptr_t>(unit) + kUnitMovementFlagsOffset);
    }

    float& Facing(void* unit)
    {
        return *reinterpret_cast<float*>(reinterpret_cast<uintptr_t>(unit) + kUnitFacingOffset);
    }

    float& Pitch(void* unit)
    {
        return *reinterpret_cast<float*>(reinterpret_cast<uintptr_t>(unit) + kUnitPitchOffset);
    }

    bool UnitIsFlying(void* unit)
    {
        return unit && (MovementFlags(unit) & MOVE_FLAG_FLYING) != 0;
    }

    // Skyward knockback clears FLYING briefly but sets FALLING — must not look like a land.
    bool UnitIsFalling(void* unit)
    {
        return unit && (MovementFlags(unit)
            & (MOVE_FLAG_FALLING | MOVE_FLAG_FALLING_FAR)) != 0;
    }

    float NormalizeAngle(float a)
    {
        constexpr float kPi = 3.14159265f;
        constexpr float kTwoPi = 6.2831853f;
        while (a > kPi) a -= kTwoPi;
        while (a < -kPi) a += kTwoPi;
        return a;
    }

    void ApplyMovementGate(void* unit)
    {
        if (g_classicVertical || !g_mode || !unit)
            return;

        const bool seated = PlayerSeatedOnAdvFlyMount(unit);
        if (!seated && !UnitIsFlying(unit))
            return;

        uint32_t& flags = MovementFlags(unit);
        const uint32_t before = flags;
        const bool wasFlying = (before & MOVE_FLAG_FLYING) != 0;

        // Always block Space ascent / X descend (except stall DESC).
        uint32_t after = before & ~MOVE_FLAG_ASCENDING;

        if (g_stalled && wasFlying)
        {
            // Stall: wings can't hold — let DESCENDING drive engine sink (X is still "blocked"
            // as a player verb; here DESC is our physics, not the X key).
            after |= MOVE_FLAG_DESCENDING;
        }
        else
        {
            // Normal skyriding: no X / classic descend.
            after &= ~MOVE_FLAG_DESCENDING;
        }

        // Block Space takeoff from solid ground only.
        // Fall-into-fly: Space sets ASC+FLY while FALLING — must KEEP FLYING (only strip ASC).
        if (!wasFlying && (before & MOVE_FLAG_ASCENDING) != 0)
        {
            bool const falling = (before & (MOVE_FLAG_FALLING | MOVE_FLAG_FALLING_FAR)) != 0;
            if (!falling)
            {
                after &= ~(MOVE_FLAG_FLYING | MOVE_FLAG_FALLING | MOVE_FLAG_FALLING_FAR
                    | MOVE_FLAG_DISABLE_GRAV);
            }
        }
        if (after != before)
            flags = after;
    }

    float* UnitPos(void* unit)
    {
        return reinterpret_cast<float*>(
            reinterpret_cast<uintptr_t>(unit) + kUnitPositionOffset);
    }

    // TraceLine: (end, start, result, distFrac, flags, opt) — end before start.
    // SEH-isolated: never let a bad call take down the client.
    int CallTraceLine(float* end, float* start, float* result, float* distFrac, uint32_t flags)
    {
        using TraceLineFn = char(__cdecl*)(float*, float*, float*, float*, uint32_t, uint32_t);
        __try
        {
            return static_cast<int>(
                reinterpret_cast<TraceLineFn>(kTraceLine)(
                    end, start, result, distFrac, flags, 0));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return -1;
        }
    }

    // Clamp `to` against world geometry. Hit = distFrac shortened (return polarity
    // is unreliable across builds — do not trust AL alone).
    bool ClampMoveAgainstWorld(float const* from, float* to)
    {
        float const dx = to[0] - from[0];
        float const dy = to[1] - from[1];
        float const dz = to[2] - from[2];
        float const len = sqrtf(dx * dx + dy * dy + dz * dz);
        if (len < 1e-4f || len > kCollideMaxStepYards)
            return false;

        float start[3] = { from[0], from[1], from[2] + kCollideProbeZ };
        float end[3] = { to[0], to[1], to[2] + kCollideProbeZ };
        float hit[3] = {};
        float distFrac = 1.0f;

        CallTraceLine(end, start, hit, &distFrac, kTraceHitFlags);
        if (distFrac >= 0.999f)
            return false;

        float skinFrac = kCollideSkinYards / len;
        if (skinFrac > 0.9f)
            skinFrac = 0.9f;
        float t = distFrac - skinFrac;
        if (t < 0.f)
            t = 0.f;
        if (t > 1.f)
            t = 1.f;

        to[0] = from[0] + dx * t;
        to[1] = from[1] + dy * t;
        to[2] = from[2] + dz * t;
        return true;
    }

    void ApplyWorldCollision(void* unit)
    {
        if (!unit || !UnitIsFlying(unit))
        {
            g_haveLastCollidePos = false;
            return;
        }

        float* pos = UnitPos(unit);
        if (g_haveLastCollidePos)
        {
            float desired[3] = { pos[0], pos[1], pos[2] };
            ClampMoveAgainstWorld(g_lastCollidePos, desired);
            pos[0] = desired[0];
            pos[1] = desired[1];
            pos[2] = desired[2];
        }

        g_lastCollidePos[0] = pos[0];
        g_lastCollidePos[1] = pos[1];
        g_lastCollidePos[2] = pos[2];
        g_haveLastCollidePos = true;
    }

    // Keep / clear the same control bit MoveForwardStart/Stop uses (0x10).
    void SetForwardControlBit(bool enable)
    {
        void* input = *reinterpret_cast<void**>(kInputControlPtr);
        if (!input)
            return;
        uint32_t const time = *reinterpret_cast<uint32_t*>(kInputTimestamp);
        if (enable)
        {
            using SetFn = int(__thiscall*)(void*, uint32_t, uint32_t);
            reinterpret_cast<SetFn>(kSetControlBit)(input, kControlBitForward, time);
        }
        else
        {
            using UnsetFn = int(__thiscall*)(void*, uint32_t, uint32_t, uint32_t);
            reinterpret_cast<UnsetFn>(kUnsetControlBit)(
                input, kControlBitForward, time, 0);
        }
    }

    // Drop a coast-forced FORWARD bit. If the player is holding W, only clear our
    // claim — Unset would cancel their key until they re-press.
    void ReleaseForcedForward()
    {
        if (!g_forcedForwardBit)
            return;
        bool const wHeld = (GetAsyncKeyState('W') & 0x8000) != 0
            || (GetAsyncKeyState(VK_UP) & 0x8000) != 0;
        if (!wHeld)
            SetForwardControlBit(false);
        g_forcedForwardBit = false;
    }

    // Never hover. Coast = keep FORWARD control bit so the engine moves (with walls).
    // Do not write UnitPos — that is what tunneled through geometry.
    // Only force the bit when W is NOT held; otherwise the client owns it.
    void ApplyCoastAndBrake(void* unit, float /*dt*/)
    {
        if (!g_mode || !unit || !UnitIsFlying(unit))
        {
            g_braking = false;
            ReleaseForcedForward();
            return;
        }

        uint32_t& flags = MovementFlags(unit);
        bool const keyBack = (GetAsyncKeyState('S') & 0x8000) != 0
            || (GetAsyncKeyState(VK_DOWN) & 0x8000) != 0;
        bool const flagBack = (flags & MOVE_FLAG_BACKWARD) != 0;
        g_braking = keyBack || flagBack;

        bool const wHeld = (GetAsyncKeyState('W') & 0x8000) != 0
            || (GetAsyncKeyState(VK_UP) & 0x8000) != 0;

        flags &= ~(MOVE_FLAG_BACKWARD | MOVE_FLAG_PENDING_STOP);
        flags |= MOVE_FLAG_FORWARD;

        if (g_braking)
        {
            ReleaseForcedForward();
        }
        else if (!wHeld)
        {
            // W released mid-air: re-assert FORWARD so we coast without hovering.
            SetForwardControlBit(true);
            g_forcedForwardBit = true;
        }
        else
        {
            // Player holding W — client owns the bit; do not steal on release later.
            g_forcedForwardBit = false;
        }
    }

    // Stall = sink without changing mount pitch (DESCENDING + explicit Z).
    void ApplyStallSink(void* unit, float dt)
    {
        if (!g_mode || !unit || dt <= 0.f || !UnitIsFlying(unit) || !g_stalled)
            return;

        float* pos = UnitPos(unit);
        float const from[3] = { pos[0], pos[1], pos[2] };
        pos[2] -= kStallSinkPerSec * dt;
        ClampMoveAgainstWorld(from, pos);

        MovementFlags(unit) |= MOVE_FLAG_DESCENDING;
    }

    // Surge lift: raise a bit without ever writing pitch (holds lock if something tries).
    void ApplySurgeLift(void* unit, float dt)
    {
        if (!unit || dt <= 0.f || !UnitIsFlying(unit))
            return;

        const DWORD now = GetTickCount();
        if (g_surgePitchLocked)
            Pitch(unit) = g_surgePitchLock;

        if (g_surgeLiftUntilMs == 0 || now >= g_surgeLiftUntilMs)
        {
            if (now >= g_surgeLiftUntilMs)
            {
                g_surgeLiftUntilMs = 0;
                g_surgePitchLocked = false;
            }
            return;
        }

        float* pos = UnitPos(unit);
        float const from[3] = { pos[0], pos[1], pos[2] };
        pos[2] += kSurgeLiftPerSec * dt;
        ClampMoveAgainstWorld(from, pos);
        Pitch(unit) = g_surgePitchLock;
    }

    void ApplyTurnInertia(void* unit)
    {
        if (!unit || !UnitIsFlying(unit) || g_turnRate >= 0.999f)
        {
            if (unit)
            {
                g_lastFacing = Facing(unit);
                g_haveLastFacing = true;
            }
            return;
        }
        float& facing = Facing(unit);
        if (!g_haveLastFacing)
        {
            g_lastFacing = facing;
            g_haveLastFacing = true;
            return;
        }
        const float delta = NormalizeAngle(facing - g_lastFacing);
        facing = g_lastFacing + delta * g_turnRate;
        g_lastFacing = facing;
    }

    void SetForcedAnim(int animId)
    {
        g_forcedAnimId = animId;
    }

    // 0xFFFFFFFF as float — engine "keep/blend" seq time (fluid transitions).
    float AnySequenceTime()
    {
        float value;
        uint32_t bits = 0xFFFFFFFFu;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }

    // speed > 0 forward; speed < 0 = reverse. force=true re-issues SetBoneSequence every call.
    // Prefer AnySequenceTime() when switching clips so the engine blends (dive exit is fluid this way).
    // seqTime == 0 forces a hard restart from frame 0 (snap) — avoid on transitions.
    void PlayBoneAnim(void* unit, void* model, int animId, float speed = 1.0f,
        float seqTime = -1.0f, bool highLevel = false, bool force = false)
    {
        if (!unit || !model || !g_setBoneSeq || animId <= 0)
            return;
        // Never drive the player body — AdvFly on the character looks like standing off-mount.
        if (!ModelSupportsAdvFly(model))
            return;

        SetForcedAnim(animId);
        const bool same = (g_boneAnimId == animId)
            && (fabsf(g_boneAnimSpeed - speed) < 0.01f);
        if (same && !force)
            return;

        if (seqTime < 0.0f)
        {
            if (speed < 0.0f)
                seqTime = DUR_DOWN_START_MS / 1000.0f;
            else
                seqTime = AnySequenceTime();
        }

        // Never AnimationData on the player unit — that desyncs seated/mount visuals.
        (void)highLevel;

        g_setBoneSeq(unit, reinterpret_cast<uintptr_t>(model), -1, animId,
            seqTime, 0, speed, 0, 1, 0);
        g_boneAnimId = animId;
        g_boneAnimSpeed = speed;
        g_forceUnit = unit;
        g_forceModel = model;
    }

    void EnterPhase(AnimPhase phase, DWORD durationMs)
    {
        g_phase = phase;
        g_phaseUntilMs = durationMs ? (GetTickCount() + durationMs) : 0;
    }

    void BeginSkywardFlap1(void* unit, void* model)
    {
        // One hard start from frame 0 — never re-issue seqTime=0 every tick (looks interrupted).
        g_boneAnimId = -1;
        g_boneAnimSpeed = 0.0f;
        PlayBoneAnim(unit, model, ANIM_FLAP_UP, 1.0f, 0.0f, false, true);
        EnterPhase(AnimPhase::SkywardFlap1, DUR_FLAP_UP_MS);
    }

    bool PhaseIsLocked()
    {
        return g_phase == AnimPhase::SurgeFlap
            || g_phase == AnimPhase::SkywardFlap1
            || g_phase == AnimPhase::SkywardFlap2
            || g_phase == AnimPhase::DiveStart
            || g_phase == AnimPhase::DiveExit;
    }

    void ReleaseAnimDriver()
    {
        g_phase = AnimPhase::Idle;
        g_forcedAnimId = -1;
        g_boneAnimId = -1;
        g_boneAnimSpeed = 1.0f;
        g_cruiseTier = ANIM_FORWARD_GLIDE;
        g_inDive = false;
        g_phaseUntilMs = 0;
        g_skywardPitchLocked = false;
        g_surgePitchLocked = false;
        g_surgeLiftUntilMs = 0;
        g_forceUnit = nullptr;
        g_forceModel = nullptr;
    }

    // Server LAND (GetMapHeight touchdown) — ignore phase/spell/FLYING leftovers.
    void ForceGroundFromServer()
    {
        ReleaseForcedForward();
        ReleaseAnimDriver();
        void* unit = ActivePlayer();
        if (unit)
        {
            MovementFlags(unit) &= ~(MOVE_FLAG_FLYING | MOVE_FLAG_DISABLE_GRAV
                | MOVE_FLAG_ASCENDING | MOVE_FLAG_DESCENDING
                | MOVE_FLAG_FALLING | MOVE_FLAG_FALLING_FAR);
        }
        g_haveLastFacing = false;
        g_haveLastCollidePos = false;
        g_lastPhysicsMs = 0;
        // Hold ground briefly so a late client fly flag cannot restart AdvFly.
        g_holdGroundUntilMs = GetTickCount() + 450;
    }

    // INFO: glide default → glideslow when slow → slowfall when stalled.
    int PickCruiseAnim()
    {
        if (g_stalled)
        {
            g_cruiseTier = ANIM_SLOW_FALL;
            return ANIM_SLOW_FALL;
        }

        float rate = g_coastYardsPerSec / kBaseFlightYards;
        if (rate < 0.1f)
            rate = 0.1f;

        if (g_cruiseTier == ANIM_SLOW_FALL)
            g_cruiseTier = ANIM_FORWARD_GLIDE_SLOW;

        if (g_cruiseTier == ANIM_FORWARD_GLIDE_SLOW)
        {
            if (rate >= RATE_SLOW_EXIT)
                g_cruiseTier = ANIM_FORWARD_GLIDE;
        }
        else
        {
            if (rate < RATE_SLOW_ENTER)
                g_cruiseTier = ANIM_FORWARD_GLIDE_SLOW;
            else
                g_cruiseTier = ANIM_FORWARD_GLIDE;
        }
        return g_cruiseTier;
    }

    void BeginDive(void* unit, void* model)
    {
        if (!unit || !model || g_inDive || PhaseIsLocked())
            return;
        g_inDive = true;
        g_boneAnimId = -1;
        PlayBoneAnim(unit, model, ANIM_DOWN_START, 1.0f, 0.0f, false, true);
        EnterPhase(AnimPhase::DiveStart, DUR_DOWN_START_MS);
    }

    void TickFsm(void* unit, void* model)
    {
        if (!unit || !model)
            return;

        g_forceUnit = unit;
        g_forceModel = model;

        const float pitch = Pitch(unit);
        const DWORD now = GetTickCount();

        // --- Surge: FlapBig → Glide ---
        // Cut only at (duration - blendTime) so SetBoneSequence(Glide) lands in M2 blend window.
        if (g_phase == AnimPhase::SurgeFlap)
        {
            PlayBoneAnim(unit, model, ANIM_FLAP_BIG, 1.0f, -1.0f, false, false);
            if (now >= g_phaseUntilMs)
            {
                // AnySequenceTime = engine blend from current pose (same as dive→glide).
                PlayBoneAnim(unit, model, ANIM_FORWARD_GLIDE, 1.0f, -1.0f, false, false);
                EnterPhase(AnimPhase::Cruise, 0);
            }
            return;
        }
        if (g_phase == AnimPhase::SkywardFlap1)
        {
            if (g_skywardPitchLocked)
                Pitch(unit) = g_skywardPitchLock;
            // Keep FlapUp on bones; Resolve remaps Fall/Jump from knockback → 1702.
            // No seqTime=0 every frame (that restarts the clip).
            PlayBoneAnim(unit, model, ANIM_FLAP_UP, 1.0f, -1.0f, false, false);
            if (now >= g_phaseUntilMs)
            {
                PlayBoneAnim(unit, model, ANIM_SECOND_FLAP_UP, 1.0f, 0.0f, false, true);
                EnterPhase(AnimPhase::SkywardFlap2, DUR_FLAP_UP_2_MS);
            }
            return;
        }
        if (g_phase == AnimPhase::SkywardFlap2)
        {
            if (g_skywardPitchLocked)
                Pitch(unit) = g_skywardPitchLock;
            PlayBoneAnim(unit, model, ANIM_SECOND_FLAP_UP, 1.0f, -1.0f, false, false);
            if (now >= g_phaseUntilMs)
            {
                g_skywardPitchLocked = false;
                PlayBoneAnim(unit, model, ANIM_FORWARD_GLIDE, 1.0f, -1.0f, false, false);
                EnterPhase(AnimPhase::Cruise, 0);
            }
            return;
        }

        // --- Dive: DownStart → Down; exit as soon as pitch rises past EXIT (not ~0) ---
        // Also entered from Tick() on fall-into-fly (cliff) before pitch is steep.
        if (!g_inDive && pitch < PITCH_DIVE_ENTER && !PhaseIsLocked())
        {
            BeginDive(unit, model);
        }
        else if (g_inDive && pitch > PITCH_DIVE_EXIT
            && (g_phase == AnimPhase::Dive || g_phase == AnimPhase::DiveStart))
        {
            PlayBoneAnim(unit, model, ANIM_DOWN_START, -1.0f,
                DUR_DOWN_START_MS / 1000.0f, false, true);
            EnterPhase(AnimPhase::DiveExit, DUR_DOWN_START_MS);
        }

        if (g_phase == AnimPhase::DiveStart)
        {
            PlayBoneAnim(unit, model, ANIM_DOWN_START, 1.0f, 0.0f, false);
            if (now >= g_phaseUntilMs)
            {
                PlayBoneAnim(unit, model, ANIM_DOWN, 1.0f, 0.0f, false);
                EnterPhase(AnimPhase::Dive, 0);
            }
            return;
        }
        if (g_phase == AnimPhase::Dive)
        {
            PlayBoneAnim(unit, model, ANIM_DOWN, 1.0f, 0.0f, false);
            return;
        }
        if (g_phase == AnimPhase::DiveExit)
        {
            PlayBoneAnim(unit, model, ANIM_DOWN_START, -1.0f,
                DUR_DOWN_START_MS / 1000.0f, false);
            if (now >= g_phaseUntilMs)
            {
                g_inDive = false;
                PlayBoneAnim(unit, model, ANIM_FORWARD_GLIDE, 1.0f, 0.0f, false);
                EnterPhase(AnimPhase::Cruise, 0);
            }
            return;
        }

        // --- Cruise / stall ---
        const int cruise = PickCruiseAnim();
        PlayBoneAnim(unit, model, cruise, 1.0f, 0.0f, false);
        if (g_phase != AnimPhase::Cruise)
            EnterPhase(AnimPhase::Cruise, 0);
    }

    void Tick()
    {
        if (ModuleDisabled())
            return;

        void* unit = ActivePlayer();
        if (!unit)
        {
            ReleaseAnimDriver();
            ReleaseForcedForward();
            g_haveLastCollidePos = false;
            return;
        }

        ApplyMovementGate(unit);

        if (!g_mode || !PlayerSeatedOnAdvFlyMount(unit))
        {
            g_haveLastFacing = false;
            g_haveLastCollidePos = false;
            g_lastPhysicsMs = 0;
            ReleaseForcedForward();
            ReleaseAnimDriver();
            return;
        }

        const bool flyingNow = UnitIsFlying(unit);
        const bool fallingNow = UnitIsFalling(unit);
        const DWORD nowTick = GetTickCount();
        if (fallingNow)
            g_lastFallingMs = nowTick;
        bool const recentFall = (g_lastFallingMs != 0)
            && (nowTick - g_lastFallingMs) < 1000;
        const bool holdGround = (g_holdGroundUntilMs != 0 && nowTick < g_holdGroundUntilMs);

        // New flight (cliff / skyward): cancel land hold so AdvFly can start.
        if (flyingNow && !g_prevFlying)
            g_holdGroundUntilMs = 0;

        if (!flyingNow)
            ReleaseForcedForward();

        // Just landed (server LAND) — no AdvFly, no coast, no flap finish.
        // Never apply hold-ground while falling or freshly entering fly from a fall.
        if (holdGround && !flyingNow && !fallingNow && !recentFall)
        {
            ReleaseForcedForward();
            ReleaseAnimDriver();
            g_prevFlying = flyingNow;
            return;
        }
        if (flyingNow || fallingNow || recentFall)
            g_holdGroundUntilMs = 0;

        // Grounded (no FLYING, no FALLING): leave AdvFly immediately.
        if (!flyingNow && !fallingNow)
        {
            g_haveLastFacing = false;
            g_haveLastCollidePos = false;
            g_lastPhysicsMs = 0;
            ReleaseAnimDriver();
            g_prevFlying = flyingNow;
            return;
        }

        if (flyingNow)
        {
            float dt = 0.016f;
            if (g_lastPhysicsMs != 0 && nowTick > g_lastPhysicsMs)
                dt = (nowTick - g_lastPhysicsMs) / 1000.f;
            if (dt > 0.1f)
                dt = 0.1f;
            g_lastPhysicsMs = nowTick;

            ApplyWorldCollision(unit);
            ApplyCoastAndBrake(unit, dt);
            ApplyStallSink(unit, dt);
            ApplySurgeLift(unit, dt);
            ApplyTurnInertia(unit);

            float* pos = UnitPos(unit);
            g_lastCollidePos[0] = pos[0];
            g_lastCollidePos[1] = pos[1];
            g_lastCollidePos[2] = pos[2];
            g_haveLastCollidePos = true;
        }

        void* model = AnimationModel(unit);
        if (!model)
        {
            g_prevFlying = flyingNow;
            return;
        }

        // Fall-into-fly: FLYING often arrives with FALLING already cleared — use recentFall.
        if (flyingNow && !g_prevFlying && !PhaseIsLocked()
            && (fallingNow || recentFall || Pitch(unit) < PITCH_DIVE_ENTER))
            BeginDive(unit, model);

        if (flyingNow)
            TickFsm(unit, model);
        else if (fallingNow && PhaseIsLocked())
            TickFsm(unit, model);
        else if (fallingNow && recentFall && !PhaseIsLocked())
        {
            // Still falling, not yet FLYING — start dive pose early so Space→fly is seamless.
            BeginDive(unit, model);
            TickFsm(unit, model);
        }
        else
            ReleaseAnimDriver();

        g_prevFlying = flyingNow;
    }

    // Resolve override: ONLY on the mount model we are driving.
    // Forcing AdvFly onto the player body = standing off-mount (intermittent).
    int ResolveOverride(void* unit, int /*requested*/, void* model, int resolved)
    {
        if (!g_mode || g_forcedAnimId <= 0)
            return -1;
        if (!unit || unit != g_forceUnit)
            return -1;
        if (!model || !g_forceModel || model != g_forceModel)
            return -1;
        if (!ModelSupportsAdvFly(model))
            return -1;

        // During skyward flaps, anything that isn't the current flap must remap
        // (Fall/Jump from knockback, Glide, etc.).
        if (g_phase == AnimPhase::SkywardFlap1 || g_phase == AnimPhase::SkywardFlap2)
        {
            if (resolved != g_forcedAnimId)
                return g_forcedAnimId;
            return -1;
        }

        if (resolved > 0 && resolved < 506)
            return g_forcedAnimId;
        if (resolved != g_forcedAnimId)
            return g_forcedAnimId;
        return -1;
    }

    // --- Lua -----------------------------------------------------------------

    int __cdecl LuaSkyridingTick(void* /*state*/)
    {
        Tick();
        return 0;
    }

    int __cdecl LuaSkyridingSetMode(void* state)
    {
        const int on = (wlua::GetTop(state) >= 1 && wlua::IsNumber(state, 1))
            ? static_cast<int>(wlua::ToNumber(state, 1)) : 0;
        g_mode = on != 0;
        if (!g_mode)
        {
            ReleaseAnimDriver();
            ReleaseForcedForward();
            g_haveLastFacing = false;
            g_lastPhysicsMs = 0;
            g_braking = false;
            g_stalled = false;
            g_prevFlying = false;
            g_holdGroundUntilMs = 0;
        }
        return 0;
    }

    int __cdecl LuaSkyridingLand(void* /*state*/)
    {
        ForceGroundFromServer();
        return 0;
    }

    int __cdecl LuaSkyridingIsBraking(void* state)
    {
        wlua::PushNumber(state, g_braking ? 1.0 : 0.0);
        return 1;
    }

    int __cdecl LuaSkyridingSetCoastSpeed(void* state)
    {
        if (wlua::GetTop(state) >= 1 && wlua::IsNumber(state, 1))
        {
            float yps = static_cast<float>(wlua::ToNumber(state, 1));
            if (yps < kMinCoastYardsPerSec)
                yps = kMinCoastYardsPerSec;
            if (yps > 120.f)
                yps = 120.f;
            g_coastYardsPerSec = yps;
        }
        return 0;
    }

    int __cdecl LuaSkyridingSetStalled(void* state)
    {
        const int on = (wlua::GetTop(state) >= 1 && wlua::IsNumber(state, 1))
            ? static_cast<int>(wlua::ToNumber(state, 1)) : 0;
        g_stalled = on != 0;
        return 0;
    }

    int __cdecl LuaSkyridingSetClassicVertical(void* state)
    {
        const int allow = (wlua::GetTop(state) >= 1 && wlua::IsNumber(state, 1))
            ? static_cast<int>(wlua::ToNumber(state, 1)) : 0;
        g_classicVertical = allow != 0;
        return 0;
    }

    int __cdecl LuaSkyridingSetTurnRate(void* state)
    {
        float rate = 0.75f;
        if (wlua::GetTop(state) >= 1 && wlua::IsNumber(state, 1))
            rate = static_cast<float>(wlua::ToNumber(state, 1));
        if (rate < 0.05f) rate = 0.05f;
        if (rate > 1.0f) rate = 1.0f;
        g_turnRate = rate;
        return 0;
    }

    int __cdecl LuaSkyridingSetFlightRate(void* state)
    {
        if (wlua::GetTop(state) >= 1 && wlua::IsNumber(state, 1))
        {
            g_flightRateNorm = static_cast<float>(wlua::ToNumber(state, 1));
            g_stalled = g_flightRateNorm < 0.f;
        }
        return 0;
    }

    int __cdecl LuaSkyridingFlap(void* state)
    {
        const int kind = (wlua::GetTop(state) >= 1 && wlua::IsNumber(state, 1))
            ? static_cast<int>(wlua::ToNumber(state, 1)) : 0;
        if (PhaseIsLocked())
            return 0;

        void* unit = ActivePlayer();
        void* model = AnimationModel(unit);
        if (!unit || !model)
            return 0;

        g_boneAnimId = -1; // force SetBoneSequence
        g_boneAnimSpeed = 0.0f;
        g_inDive = false;
        if (kind == 1)
        {
            // Skyward: FlapUp → SecondFlapUp. Height = server ImpulseUp (same ground/air).
            // Hold pitch so knockback cannot flatten facing.
            g_skywardPitchLock = Pitch(unit);
            g_skywardPitchLocked = true;
            BeginSkywardFlap1(unit, model);
        }
        else
        {
            // Lock pitch for the surge window — never allow knockback/anim to flatten to 0.
            g_surgePitchLock = Pitch(unit);
            g_surgePitchLocked = true;
            g_surgeLiftUntilMs = GetTickCount() + kSurgeLiftMs;
            // Bone-only + blend seq time — same path style as dive→glide (no snap).
            PlayBoneAnim(unit, model, ANIM_FLAP_BIG, 1.0f, -1.0f, false, true);
            EnterPhase(AnimPhase::SurgeFlap, DUR_FLAP_BIG_MS);
        }
        return 0;
    }

    int __cdecl LuaPlayMountAnim(void* state)
    {
        const int animId = (wlua::GetTop(state) >= 1 && wlua::IsNumber(state, 1))
            ? static_cast<int>(wlua::ToNumber(state, 1)) : 0;
        void* unit = ActivePlayer();
        void* model = AnimationModel(unit);
        g_boneAnimId = -1;
        g_boneAnimSpeed = 0.0f;
        PlayBoneAnim(unit, model, animId, 1.0f, 0.0f, true);
        return 0;
    }

    int __cdecl LuaSkyridingDbg(void* state)
    {
        const char* hyp = (wlua::GetTop(state) >= 1 && wlua::IsString(state, 1))
            ? wlua::ToString(state, 1, nullptr) : "?";
        const char* msg = (wlua::GetTop(state) >= 2 && wlua::IsString(state, 2))
            ? wlua::ToString(state, 2, nullptr) : "";
        const char* extra = (wlua::GetTop(state) >= 3 && wlua::IsString(state, 3))
            ? wlua::ToString(state, 3, nullptr) : "";
        WLOG_INFO("skyriding dbg [%s] %s %s forced=%d phase=%u",
            hyp ? hyp : "?", msg ? msg : "", extra ? extra : "",
            g_forcedAnimId, static_cast<unsigned>(g_phase));
        return 0;
    }

    class SkyridingInput final : public ev::EventScript
    {
    public:
        SkyridingInput()
        {
            on<&SkyridingInput::OnInput>(ev::Event::OnInput);
            on<&SkyridingInput::OnUpdate>(ev::Event::OnUpdate);
        }

        void OnInput(const ev::InputArgs& args)
        {
            if (!g_mode || g_classicVertical || !args.handled)
                return;
            if (args.message != WM_KEYDOWN && args.message != WM_SYSKEYDOWN)
                return;
            const uintptr_t vk = args.wparam;
            if (vk != VK_SPACE && vk != 'X' && vk != 'x')
                return;
            void* unit = ActivePlayer();
            if (!unit)
                return;
            if (!PlayerSeatedOnAdvFlyMount(unit) && !UnitIsFlying(unit))
                return;
            *args.handled = true;
            ApplyMovementGate(unit);
        }

        void OnUpdate(const ev::UpdateArgs& /*args*/)
        {
            // Engine-frame driver: gate + FSM (addon tick is backup only).
            Tick();
        }
    };

    SkyridingInput g_skyridingInput;

    void Install()
    {
        if (ModuleDisabled())
        {
            WLOG_INFO("wxl-skyriding: disabled via flag file");
            return;
        }

        wxl::anim_limit::SetResolveOverride(&ResolveOverride);

        wlua::RegisterFunction("WXL_SkyridingTick", &LuaSkyridingTick);
        wlua::RegisterFunction("WXL_SkyridingSetMode", &LuaSkyridingSetMode);
        wlua::RegisterFunction("WXL_SkyridingLand", &LuaSkyridingLand);
        wlua::RegisterFunction("WXL_SkyridingIsBraking", &LuaSkyridingIsBraking);
        wlua::RegisterFunction("WXL_SkyridingSetCoastSpeed", &LuaSkyridingSetCoastSpeed);
        wlua::RegisterFunction("WXL_SkyridingSetStalled", &LuaSkyridingSetStalled);
        wlua::RegisterFunction("WXL_SkyridingSetClassicVertical", &LuaSkyridingSetClassicVertical);
        wlua::RegisterFunction("WXL_SkyridingSetTurnRate", &LuaSkyridingSetTurnRate);
        wlua::RegisterFunction("WXL_SkyridingSetFlightRate", &LuaSkyridingSetFlightRate);
        wlua::RegisterFunction("WXL_SkyridingFlap", &LuaSkyridingFlap);
        wlua::RegisterFunction("WXL_PlayMountAnim", &LuaPlayMountAnim);
        wlua::RegisterFunction("WXL_SkyridingDbg", &LuaSkyridingDbg);

        WLOG_INFO("wxl-skyriding: resolve-override + FSM (glide/dive/flap)");
    }

    struct Installer
    {
        Installer()
        {
            wxl::runtime::modules::Register("wxl-skyriding", &Install);
        }
    };
    Installer g_installer;
}
