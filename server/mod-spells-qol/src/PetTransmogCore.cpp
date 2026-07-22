#include "PetTransmogCore.h"

#include "Pet.h"

#include <map>

namespace
{
    std::map<uint32, float> const familyScales = {
        {1, 1.00f}, {2, 1.10f}, {3, 0.60f}, {4, 1.00f}, {5, 1.00f},
        {6, 0.60f}, {7, 0.90f}, {8, 1.40f}, {9, 1.00f}, {11, 0.80f},
        {12, 0.80f}, {15, 0.70f}, {16, 0.80f}, {17, 1.00f}, {19, 1.00f},
        {20, 1.00f}, {21, 0.72f}, {23, 0.50f}, {24, 0.63f}, {25, 0.90f},
        {26, 0.80f}, {27, 0.70f}, {29, 0.90f}, {30, 0.65f}, {31, 0.90f},
        {32, 0.60f}, {33, 0.90f}, {34, 0.55f}, {35, 0.80f}, {37, 0.65f},
        {38, 0.63f}, {39, 0.50f}, {40, 1.00f}, {41, 1.00f}, {42, 1.00f},
        {43, 0.56f}, {44, 0.60f}, {45, 0.50f}, {46, 1.10f}
    };
}

float PetTransmogCore::GetPetVisualScale(Pet* pet)
{
    if (!pet)
        return 1.0f;

    uint32 family = pet->GetCreatureTemplate()->family;
    float familyScale = 1.0f;
    auto itr = familyScales.find(family);
    if (itr != familyScales.end())
        familyScale = itr->second;

    return pet->GetNativeObjectScale() * familyScale;
}

float PetTransmogCore::GetPetScalingCompensation(Pet* pet, float targetVisualScale)
{
    if (!pet)
        return 1.0f;

    uint32 family = pet->GetCreatureTemplate()->family;
    float currentFamilyScale = 1.0f;
    auto itr = familyScales.find(family);
    if (itr != familyScales.end())
        currentFamilyScale = itr->second;

    float currentTemplateScale = pet->GetNativeObjectScale();
    return targetVisualScale / (currentTemplateScale * currentFamilyScale);
}
