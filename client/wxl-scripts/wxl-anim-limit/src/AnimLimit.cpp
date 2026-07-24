// wxl-anim-limit: allow AnimationData / M2 sequence IDs >= 506 on the 3.3.5a client.
// Port of Duskhaven ExtendedAnimationIdFixes (dusk-tswow CharacterFixes.cpp).
// Copyright (C) 2026. GPLv3 (see WarcraftXL LICENSE).
//
// Vanilla Wow.exe caps playable animation resolution at ID 505 (FlyCarried2H).
// This module detours the playable-sequence fallback path so IDs 506+ resolve
// when the live M2 actually contains that sequence (or via AnimationData.dbc
// fallback chain). Needed for monk / dragonriding-style backport anims.

#include "core/Hook.hpp"
#include "core/Logger.hpp"
#include "runtime/AnimResolveOverride.hpp"
#include "runtime/ModuleInstall.hpp"

#include <windows.h>

#include <cstdint>

namespace
{
    // Vanilla last stock AnimationData id; extended IDs start at 506.
    constexpr int kAnimCurrentOrNone = 506;

    constexpr uintptr_t kM2DataHasSequenceById           = 0x00825E00;
    constexpr uintptr_t kCM2ModelHasPlayableFallback     = 0x00826050;
    constexpr uintptr_t kCM2ModelFindPlayableFallbackId  = 0x00825F40;
    constexpr uintptr_t kCGUnitResolveModelAnimationId  = 0x007176F0;
    constexpr uintptr_t kClientDbGetRow                 = 0x0065C290;
    constexpr uintptr_t kAnimationDataDb                = 0x00AD30C8;

    constexpr size_t kOffModelShared    = 44;   // CM2Model -> shared header
    constexpr size_t kOffSharedM2Data   = 336;  // shared -> raw M2 data
    constexpr size_t kOffUnitModel      = 0xB4; // CGUnit -> CM2Model*

    struct AnimationDataRow
    {
        uint32_t    id;
        const char* name;
        uint32_t    weaponFlags;
        uint32_t    bodyFlags;
        uint32_t    flags;
        uint32_t    fallback;
        uint32_t    behaviorId;
        uint32_t    behaviorTier;
    };

    using M2DataHasSequenceByIdFn = bool(__stdcall*)(void* modelData, unsigned int animId);
    using HasPlayableFallbackFn   = bool(__thiscall*)(void* model, unsigned int animId);
    using FindPlayableFallbackFn  = int(__thiscall*)(void* model, unsigned int animId);
    using ResolveModelAnimIdFn    = int(__thiscall*)(void* unit, int animId, void* model);
    using ClientDbGetRowFn        = void*(__thiscall*)(void* db, uint32_t id);

    M2DataHasSequenceByIdFn g_hasSequenceById = reinterpret_cast<M2DataHasSequenceByIdFn>(
        kM2DataHasSequenceById);
    ClientDbGetRowFn g_dbGetRow = reinterpret_cast<ClientDbGetRowFn>(kClientDbGetRow);

    HasPlayableFallbackFn  g_origHasPlayable  = nullptr;
    FindPlayableFallbackFn g_origFindPlayable = nullptr;
    ResolveModelAnimIdFn   g_origResolveAnim  = nullptr;

    bool ModuleDisabled()
    {
        return GetFileAttributesA("WarcraftXL_anim-limit.disable") != INVALID_FILE_ATTRIBUTES
            || GetFileAttributesA("WarcraftXL_wxl-anim-limit.disable") != INVALID_FILE_ATTRIBUTES;
    }

    bool ModelHasDirectSequence(void* model, unsigned int animationId)
    {
        if (!model || !g_hasSequenceById)
            return false;

        __try
        {
            const uintptr_t modelAddr = reinterpret_cast<uintptr_t>(model);
            const uintptr_t shared = *reinterpret_cast<uintptr_t*>(modelAddr + kOffModelShared);
            if (shared < 0x10000)
                return false;

            void* modelData = *reinterpret_cast<void**>(shared + kOffSharedM2Data);
            if (reinterpret_cast<uintptr_t>(modelData) < 0x10000)
                return false;

            return g_hasSequenceById(modelData, animationId);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    int ResolveExtendedAnimationId(void* unit, int animationId, void* model)
    {
        if (animationId <= kAnimCurrentOrNone)
            return -1;

        if (!model && unit)
            model = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(unit) + kOffUnitModel);

        if (ModelHasDirectSequence(model, static_cast<unsigned int>(animationId)))
            return animationId;

        int current = animationId;
        for (int depth = 0; depth < 16; ++depth)
        {
            auto* row = reinterpret_cast<AnimationDataRow*>(
                g_dbGetRow(reinterpret_cast<void*>(kAnimationDataDb),
                           static_cast<uint32_t>(current)));
            if (!row || static_cast<int>(row->fallback) == current)
                break;

            current = static_cast<int>(row->fallback);
            if (current >= kAnimCurrentOrNone
                && ModelHasDirectSequence(model, static_cast<unsigned int>(current)))
                return current;

            if (current < kAnimCurrentOrNone)
                return current;
        }

        return -1;
    }

    bool __fastcall hkHasPlayableFallback(void* model, void* /*edx*/, unsigned int animationId)
    {
        if (animationId < static_cast<unsigned int>(kAnimCurrentOrNone))
            return g_origHasPlayable(model, animationId);

        return ModelHasDirectSequence(model, animationId);
    }

    int __fastcall hkFindPlayableFallbackId(void* model, void* /*edx*/, unsigned int animationId)
    {
        if (animationId < static_cast<unsigned int>(kAnimCurrentOrNone))
            return g_origFindPlayable(model, animationId);

        return ModelHasDirectSequence(model, animationId)
            ? static_cast<int>(animationId)
            : -1;
    }

    int __fastcall hkResolveModelAnimationId(void* unit, void* /*edx*/, int animationId, void* model)
    {
        int resolved = animationId;
        if (animationId > kAnimCurrentOrNone)
        {
            const int extended = ResolveExtendedAnimationId(unit, animationId, model);
            if (extended != -1)
                resolved = extended;
            else
                resolved = g_origResolveAnim(unit, animationId, model);
        }
        else
            resolved = g_origResolveAnim(unit, animationId, model);

        if (wxl::anim_limit::g_resolveOverride)
        {
            const int forced = wxl::anim_limit::g_resolveOverride(
                unit, animationId, model, resolved);
            if (forced >= 0)
                return forced;
        }

        return resolved;
    }

    void InstallHooks()
    {
        if (ModuleDisabled())
        {
            WLOG_INFO("anim-limit: disabled");
            return;
        }

        wxl::core::hook::Install("AnimLimit_HasPlayableFallback",
            kCM2ModelHasPlayableFallback,
            reinterpret_cast<void*>(&hkHasPlayableFallback),
            reinterpret_cast<void**>(&g_origHasPlayable));

        wxl::core::hook::Install("AnimLimit_FindPlayableFallbackId",
            kCM2ModelFindPlayableFallbackId,
            reinterpret_cast<void*>(&hkFindPlayableFallbackId),
            reinterpret_cast<void**>(&g_origFindPlayable));

        wxl::core::hook::Install("AnimLimit_ResolveModelAnimationId",
            kCGUnitResolveModelAnimationId,
            reinterpret_cast<void*>(&hkResolveModelAnimationId),
            reinterpret_cast<void**>(&g_origResolveAnim));

        WLOG_INFO("anim-limit: extended animation IDs (>= %d) enabled", kAnimCurrentOrNone);
    }

    struct Installer
    {
        Installer()
        {
            wxl::runtime::modules::Register("wxl-anim-limit", &InstallHooks);
        }
    };

    Installer g_installer;
}
