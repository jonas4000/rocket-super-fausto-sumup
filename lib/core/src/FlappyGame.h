#pragma once

#include <cstddef>
#include <cstdint>

namespace grinder {

// "Flappy Beans" physics, hardware- and LVGL-free so it can be unit-tested.
// Coordinates are screen pixels of the play field (above the ground), y down;
// time comes in from the caller like everywhere else.
struct FlappyConfig {
  float width = 320;        // play field
  float height = 216;       // ground top; tools/flappy_to_lvgl.py FIELD_H
  float beanX = 80;         // bean centre, fixed
  float beanRadius = 10;    // hitbox, forgiving against the 32 px sprite
  float gravity = 1100;     // px/s^2
  float flapSpeed = 300;    // upward speed after a tap, px/s (~40 px jump)
  float maxFallSpeed = 420; // px/s
  float jarWidth = 49;      // sprite width
  float gap = 88;           // opening between the upper and lower jar
  float gapMargin = 22;     // min distance of the opening from the top/ground
  float spacing = 165;      // horizontal distance between jar pairs
  float speed = 80;         // scroll speed, px/s...
  float speedPerPoint = 1;  // ...plus this per point...
  float maxSpeed = 105;     // ...up to this
};

class FlappyGame {
 public:
  enum class Phase {
    Ready,    // bean hovers, waiting for the first tap
    Playing,
    Dead,     // hit something: falls to the ground, then landed() is true
  };
  struct Jar {
    float x;       // left edge
    float gapY;    // centre of the opening
    uint8_t look;  // 0 = dark, 1 = light jars (cosmetic)
    bool scored;
  };
  static constexpr size_t kJars = 4;  // enough to cover the screen

  explicit FlappyGame(FlappyConfig config = FlappyConfig()) : cfg_(config) { reset(1); }

  // Back to Ready; seed varies the jar openings.
  void reset(uint32_t seed);
  // Tap: starts the game from Ready, then jumps. Ignored when dead.
  void flap();
  // Advances by dtMs (split into small steps, so a slow frame can't tunnel
  // the bean through a jar).
  void step(uint32_t dtMs);

  Phase phase() const { return phase_; }
  bool landed() const { return landed_; }
  uint32_t score() const { return score_; }
  float beanY() const { return beanY_; }    // centre
  float beanVy() const { return beanVy_; }  // px/s, positive = falling
  const Jar& jar(size_t i) const { return jars_[i]; }
  const FlappyConfig& config() const { return cfg_; }
  // Distance scrolled so far (moves the ground stripes).
  float scrolled() const { return scrolled_; }

 private:
  void advance(float dt);
  bool hitsJar(const Jar& j) const;
  void placeJar(Jar& j, float x);
  uint32_t nextRand();

  FlappyConfig cfg_;
  Phase phase_ = Phase::Ready;
  bool landed_ = false;
  uint32_t score_ = 0;
  float beanY_ = 0, beanVy_ = 0;
  float readyT_ = 0;  // hover animation while Ready
  float scrolled_ = 0;
  uint32_t rng_ = 1;
  Jar jars_[kJars] = {};
};

}  // namespace grinder
