#pragma once
// Pacing clock (metal_pacing_clock, default off; runtime switch RequestPacingClock for in-run A/B).
//
// Three parts, all inert while the switch is off (the measurement part runs either way, so a run has both arms):
//  1. Measurement: every presented drawable's presentedTime and every guest-vblank tick's wake lateness go to a TSV
//     (REX_METAL_PACING_LOG=<path>) tagged with the arm, for offline evenness statistics.
//  2. Fitted guest clock: the display's real refresh period is fitted from the presentedTime history (sequential
//     refresh indices, least squares, outlier rejection; accepted only with a long enough span, small residual and
//     a period within 2.5% of the nominal one). The vblank worker scales its guest interval by the fitted/nominal
//     ratio, slewed a few microseconds per tick, and goes back to the nominal cadence when there is no good fit.
//  3. Timed presents on ProMotion (display >= 117 Hz): the presenter hands Metal a desired present time of
//     guest vblank + 12.5 ms. Metal's feedback (presentedTime against the request) feeds a watchdog that turns timed
//     presents off for the session when they are not honoured.
#include <cstdint>

namespace rex::graphics::metal::pacing {
// The runtime switch (PacingClockRequested()).
bool Enabled();
// Display refresh the system reports (nominal; the fit refines it).
void SetNominalRefreshHz(double hz);

// Vblank worker: how late the tick ran (seconds, actual - intended, >= 0 typically), at host time `tick_seconds`.
void NoteTick(double tick_seconds, double lateness_seconds);
// Vblank worker: factor to apply to the nominal guest vblank interval for the next tick (1.0 when off or no fit).
double NextIntervalScale();

// Presenter: a drawable was presented. requested_seconds = the timed-present request (0 = plain present);
// new_frame = it shows a guest frame for the first time.
void OnPresented(double presented_seconds, double requested_seconds, bool new_frame);
// Presenter: whether to use a timed present now; fills the desired time (mach uptime seconds).
bool TimedPresentTarget(double vblank_seconds, double now_seconds, double* target_seconds);
}  // namespace rex::graphics::metal::pacing
