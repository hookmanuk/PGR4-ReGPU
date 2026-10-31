#pragma once
#include <cstdint>
#include <vector>

// The first-boot shader compile screen as 1280x720 RGBA8 (Host::bootScreen): PGR4's logo over its
// frontend world map, the cities' badges along the bottom, and a progress bar. False when the art
// can't be read from the game files (ReGPU's plain overlay is shown then). total 0: still searching.
bool BootScreenCompose(const wchar_t* title, uint32_t done, uint32_t total, std::vector<uint8_t>& rgba);
// PGR4_BOOTSCREEN_PREVIEW=1: writes the composed screens to <user dir>\bootscreen\*.png.
void BootScreenPreview();
