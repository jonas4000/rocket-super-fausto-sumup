#include "FlappyGame.h"

#include <cmath>

namespace grinder {

void FlappyGame::reset(uint32_t seed) {
  rng_ = seed ? seed : 1;
  phase_ = Phase::Ready;
  landed_ = false;
  score_ = 0;
  beanY_ = cfg_.height / 2;
  beanVy_ = 0;
  readyT_ = 0;
  scrolled_ = 0;
  // The first jar enters from the right edge, the rest follow at `spacing`.
  for (size_t i = 0; i < kJars; ++i) placeJar(jars_[i], cfg_.width + i * cfg_.spacing);
}

void FlappyGame::flap() {
  if (phase_ == Phase::Dead) return;
  phase_ = Phase::Playing;
  beanVy_ = -cfg_.flapSpeed;
}

void FlappyGame::step(uint32_t dtMs) {
  constexpr uint32_t kStepMs = 8;  // at maxSpeed a jar moves < 1 px per step
  while (dtMs > 0) {
    uint32_t ms = dtMs < kStepMs ? dtMs : kStepMs;
    advance(ms / 1000.0f);
    dtMs -= ms;
  }
}

void FlappyGame::advance(float dt) {
  const float floorY = cfg_.height - cfg_.beanRadius;

  if (phase_ == Phase::Ready) {
    readyT_ += dt;
    beanY_ = cfg_.height / 2 + 4 * std::sin(readyT_ * 6.0f);  // gentle hover
    scrolled_ += cfg_.speed * dt;  // the ground keeps moving
    return;
  }
  if (landed_) return;

  beanVy_ += cfg_.gravity * dt;
  if (beanVy_ > cfg_.maxFallSpeed) beanVy_ = cfg_.maxFallSpeed;
  beanY_ += beanVy_ * dt;
  if (beanY_ < cfg_.beanRadius) {  // ceiling: stop, but the jars still reach it
    beanY_ = cfg_.beanRadius;
    if (beanVy_ < 0) beanVy_ = 0;
  }
  if (beanY_ >= floorY) {
    beanY_ = floorY;
    beanVy_ = 0;
    phase_ = Phase::Dead;
    landed_ = true;
    return;
  }
  if (phase_ == Phase::Dead) return;  // falling: the world stands still

  float speed = cfg_.speed + cfg_.speedPerPoint * score_;
  if (speed > cfg_.maxSpeed) speed = cfg_.maxSpeed;
  scrolled_ += speed * dt;
  float rightmost = 0;
  for (const Jar& j : jars_)
    if (j.x > rightmost) rightmost = j.x;
  for (Jar& j : jars_) {
    j.x -= speed * dt;
    if (j.x + cfg_.jarWidth < 0) {
      placeJar(j, rightmost + cfg_.spacing);
      rightmost = j.x;
    }
    if (!j.scored && j.x + cfg_.jarWidth / 2 < cfg_.beanX) {
      j.scored = true;
      ++score_;
    }
    if (hitsJar(j)) {
      phase_ = Phase::Dead;
      if (beanVy_ < 0) beanVy_ = 0;
    }
  }
}

// Circle (bean) against the two jar rectangles.
bool FlappyGame::hitsJar(const Jar& j) const {
  const float r = cfg_.beanRadius;
  const float right = j.x + cfg_.jarWidth;
  float nearestX = cfg_.beanX < j.x ? j.x : (cfg_.beanX > right ? right : cfg_.beanX);
  float dx = cfg_.beanX - nearestX;
  if (dx * dx >= r * r) return false;
  float top = j.gapY - cfg_.gap / 2;  // upper jar ends here
  float bottom = j.gapY + cfg_.gap / 2;  // lower jar starts here
  // Distance from the bean centre to the nearest point of either jar.
  float dyTop = beanY_ < top ? 0 : beanY_ - top;
  float dyBottom = beanY_ > bottom ? 0 : bottom - beanY_;
  float dy = dyTop < dyBottom ? dyTop : dyBottom;
  return dx * dx + dy * dy < r * r;
}

void FlappyGame::placeJar(Jar& j, float x) {
  const float lo = cfg_.gapMargin + cfg_.gap / 2;
  const float hi = cfg_.height - cfg_.gapMargin - cfg_.gap / 2;
  j.x = x;
  j.gapY = lo + (hi - lo) * (nextRand() % 1001) / 1000.0f;
  j.look = nextRand() & 1;
  j.scored = false;
}

uint32_t FlappyGame::nextRand() {  // xorshift32
  rng_ ^= rng_ << 13;
  rng_ ^= rng_ >> 17;
  rng_ ^= rng_ << 5;
  return rng_;
}

}  // namespace grinder
