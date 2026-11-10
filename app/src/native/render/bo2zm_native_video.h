// Derived from GPL-3.0 code; modified for bo2-zm-nx in 2026.
//
// Raw test video of the game image (bo2zm_video_rgba): <name>.rgba frames plus <name>.txt "w h fps count".
#pragma once

#include <functional>

namespace rex::ui {
class Presenter;
}

namespace bo2zm::video {

// Starts recording when bo2zm_video_rgba (or REX_BO2ZM_VIDEO_RGBA / BO2ZM_VIDEO_RGBA) names a file.
void Boot(std::function<rex::ui::Presenter*()> get_presenter);
// Closes the file and writes the final .txt.
void Stop();

}  // namespace bo2zm::video
