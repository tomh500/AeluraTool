#pragma once
#include "config.h"
bool InitPreview(HINSTANCE instance, std::function<void()> drag, std::function<void()> edit = {}, std::function<void()> locate = {}, std::function<void()> dismiss = {});
void ConfigurePreview(const Config& config);
void ConfigurePreview(unsigned corner, unsigned size, bool captureAnimation, bool dragAnimation = true);
void ShowPreview(Gdiplus::Bitmap* image, const std::filesystem::path& file, POINT point, unsigned seconds = 0, RECT origin = {});
RECT PreviewImageScreenRect();
void PreviewDragResult(bool accepted);
void PreviewCopyResult(bool copied);
void HidePreview();
void DismissPreview();
void TogglePreviewPin();
bool PreviewPinned();
bool PreviewVisible();
void DestroyPreview();
