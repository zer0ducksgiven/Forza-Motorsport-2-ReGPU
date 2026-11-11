#pragma once
#include "gpu/dlss.h"

// AMD FSR 3 upscaling (fsr3.cpp), with the inputs and states DLSS takes (dlss.h). Records into
// `list` (the real command list). False if FidelityFX is unavailable.
bool Fsr3Evaluate(ID3D12Device* device, ID3D12GraphicsCommandList* list, const DlssSetup& setup, const DlssFrame& frame, float sharpness);
// The same with the FidelityFX ML upscaler (FSR 4), when FM2_FSR4=1 and this GPU lists it; otherwise FSR 3.
bool Fsr4Evaluate(ID3D12Device* device, ID3D12GraphicsCommandList* list, const DlssSetup& setup, const DlssFrame& frame, float sharpness);
