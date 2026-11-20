// Standalone regression: compile with the SDK and simde include directories.
#include <rex/ppc/intrinsics.h>
#include <cassert>
#include <cfloat>
#include <cstdio>
#include <limits>

static simde__m128 guest(float x, float y, float z, float w) {
  return simde_mm_set_ps(x, y, z, w);
}
static void check(simde__m128 v, float expected) {
  alignas(16) float lanes[4];
  simde_mm_store_ps(lanes, v);
  for (float lane : lanes) assert(std::isnan(expected) ? std::isnan(lane) : lane == expected);
}
int main() {
  const auto sentinel = guest(-FLT_MAX, -FLT_MAX, -FLT_MAX, -FLT_MAX);
  const auto old_dot = simde_mm_dp_ps(sentinel, sentinel, 0xEF);
  check(old_dot, std::numeric_limits<float>::infinity());
  check(rex::ppc::VmxDot<3>(sentinel, sentinel), std::numeric_limits<float>::quiet_NaN());
  // Infinity makes the first GJK improvement test true; Xenon's NaN keeps it false.
  const auto previous = simde_mm_set1_ps(FLT_MAX);
  const auto tolerance = simde_mm_set1_ps(0.01f);
  auto stop = [&](simde__m128 dot) {
    return simde_mm_movemask_ps(simde_mm_cmpge_ps(simde_mm_mul_ps(previous, tolerance),
                                               simde_mm_sub_ps(previous, dot)));
  };
  assert(stop(old_dot) == 15);
  assert(stop(rex::ppc::VmxDot<3>(sentinel, sentinel)) == 0);
  check(rex::ppc::VmxDot<3>(guest(1,2,3,100), guest(4,5,6,100)), 32);
  check(rex::ppc::VmxDot<4>(guest(1,2,3,100), guest(4,5,6,100)), 10032);
  // Products may exceed float range and cancel before the one final narrowing.
  check(rex::ppc::VmxDot<3>(guest(FLT_MAX,2,FLT_MAX,0), guest(2,3,-2,0)), 6);
  const float inf = std::numeric_limits<float>::infinity();
  check(rex::ppc::VmxDot<3>(guest(inf,1,1,0), guest(1,1,1,0)), inf);
  check(rex::ppc::VmxDot<3>(guest(inf,1,-inf,0), guest(1,1,1,0)),
        std::numeric_limits<float>::quiet_NaN());
  check(rex::ppc::VmxDot<3>(guest(1e20f,3,-1e20f,0), guest(1,1,1,0)), 3);
  check(rex::ppc::VmxDot<3>(guest(FLT_MIN,0,0,0), guest(.5f,0,0,0)), 0);
  puts("PASS: dot overflow, collision stop condition, lane order, cancellation, infinities, denormals");
}
