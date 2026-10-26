#pragma once

#include <lvgl.h>

#include "Controller.h"
#include "FlappyGame.h"

namespace grinder {

// LVGL screens for the 320x240 landscape display. Shared by the firmware and
// the desktop simulator. The UI only reads controller state and forwards
// button presses / settings changes; it never touches payment or motor.
//
// Besides the controller state, the UI has its own modes (screensaver, PIN
// entry, settings) that are only reachable while the controller is Idle.
// Event callbacks only change mode_ or call the controller; screens are rebuilt
// in update(), never inside a callback (that would delete the clicked object).
class Ui {
 public:
  using Clock = uint32_t (*)();

  Ui(Controller& controller, Clock clock, const char* settingsPin)
      : ctl_(controller), clock_(clock), pin_(settingsPin) {}

  // Optional; seed from real entropy (esp_random() on the device) so each boot
  // shows a different sequence of grind messages.
  void seedRandom(uint32_t seed) { rng_ ^= seed; }
  // Flappy Beans tuning; ui-snap uses wide openings to script a scoring round.
  void setGameConfig(const FlappyConfig& c) { game_ = FlappyGame(c); }

  // root: a 320x240 object (the active screen on the device).
  void begin(lv_obj_t* root);
  // Call every loop iteration, outside LVGL event callbacks.
  void update();

 private:
  enum class Mode {
    Main, Portafilter, Screensaver, Pin, Settings, ResetConfirm,
    Info, HowTo, Stats,  // info pages, no PIN
    Game, GameName, Leaderboard,  // Flappy Beans, from the screensaver
  };
  static Mode parentOf(Mode m);  // where the close (X) / back (<) button leads
  static bool isInfoPage(Mode m);  // reading pages: longer screensaver timeout
  static bool isGameMode(Mode m);  // screensaver and Flappy Beans: off with the game
  static constexpr size_t kMaxSettingsRows = 4;
  static constexpr size_t kPinLength = 4;
  static constexpr size_t kConfettiCount = 36;

  struct Confetti {
    lv_obj_t* obj;
    float x, y, vx, vy;  // screen pixels, px/s
  };

  void rebuild();
  void buildIdle();
  void buildAwaitingPayment();
  void buildGrinding();
  void buildDone();
  void buildProblem(const char* title, const char* subtitle, bool withButtons);
  void buildPortafilter();
  void buildScreensaver();
  lv_obj_t* arcadeButton(lv_obj_t* parent, int32_t w, lv_color_t faceColor,
                         lv_color_t baseColor, Mode next);
  void animateStart(lv_obj_t* face, int32_t y, uint32_t ms, bool wake);
  void startConfetti();
  void moveConfetti();
  uint32_t nextRand();  // 0..65535
  const char* nextGrindMessage();  // random, never the same twice in a row
  lv_obj_t* buildHeaderClose(const char* title);  // title + X on the right; returns the row
  lv_obj_t* buildHeaderBack(const char* title);  // < on the left + title; returns the row
  void buildInfo();
  void buildHowTo();
  void buildStats();
  void buildPin();
  void buildSettings();
  void buildResetConfirm();
  void refreshPin();
  void refreshSettingsRow(size_t i);

  // Flappy Beans (UiFlappy.cpp).
  void buildGame();
  void buildGameName();
  void buildLeaderboard();
  void startGame();       // back to "Get ready"
  void updateGame();      // one frame: physics, sprites, overlays
  void showGameOver();
  void refreshName();

  static void onDose(lv_event_t* e);
  static void onPortafilterOk(lv_event_t* e);
  static void onCancel(lv_event_t* e);
  static void onRetry(lv_event_t* e);
  static void onRootClicked(lv_event_t* e);
  static void onOpenSettings(lv_event_t* e);
  static void onOpenInfo(lv_event_t* e);
  static void onInfoItem(lv_event_t* e);
  static void onClose(lv_event_t* e);
  static void onPinKey(lv_event_t* e);
  static void onAdjust(lv_event_t* e);
  static void onGameSwitch(lv_event_t* e);
  static void onResetAsk(lv_event_t* e);
  static void onResetConfirmed(lv_event_t* e);
  static void onManualGrind(lv_event_t* e);
  static void onStartButton(lv_event_t* e);
  static void onGameTap(lv_event_t* e);
  static void onGameButton(lv_event_t* e);
  static void onNameKey(lv_event_t* e);

  Controller& ctl_;
  Clock clock_;
  const char* pin_;
  lv_obj_t* root_ = nullptr;

  Mode mode_ = Mode::Screensaver;  // boots into the screensaver (Main with the game off)
  Mode shownMode_ = Mode::Main;
  State shownState_ = State::Idle;
  bool shownOnline_ = true;
  bool shownFree_ = false;  // Main shows a Flappy Beans free shot
  State lastState_ = State::Idle;
  size_t pendingDose_ = 0;  // tapped dose, waiting for the portafilter question
  size_t grindMessage_ = 0;  // index into kGrindMessages, last one shown
  uint32_t rng_ = 0;         // LCG state (grind messages, confetti)

  // Milestone confetti over the grinding screen.
  Confetti confetti_[kConfettiCount] = {};
  bool confettiOn_ = false;
  uint32_t confettiStartMs_ = 0;
  uint32_t confettiLastMs_ = 0;


  Mode startNext_ = Mode::Main;  // where the pressed screensaver button leads

  // Flappy Beans. The game screen is built once per round trip; phases
  // (ready, playing, game over) only show/hide its overlays.
  FlappyGame game_;
  bool gamePaused_ = false;
  bool gameOverShown_ = false;
  uint32_t gameLastMs_ = 0;
  uint32_t pendingScore_ = 0;  // top 5 score waiting for a name
  uint32_t nameDueMs_ = 0;     // game over panel shows the rank, then name entry
  int highlightRank_ = -1;     // leaderboard row to highlight (just saved)
  char playerName_[ScoreEntry::kNameMax + 1] = {};  // typed on the name page, empty each time
  size_t playerNameLen_ = 0;
  struct GameView {
    lv_obj_t* ground;
    lv_obj_t* bean;
    lv_obj_t* jarTop[FlappyGame::kJars];
    lv_obj_t* jarBottom[FlappyGame::kJars];
    lv_obj_t* score;  // digit row, top centre
    lv_obj_t* best;   // "BEST" + digit row, top left
    lv_obj_t* ready;  // title, "Get ready", tap hint, menu buttons
    lv_obj_t* pause;
    lv_obj_t* pausedMenu;
    lv_obj_t* over;   // "Game over" panel
    lv_obj_t* overScore;
    lv_obj_t* overBest;
    lv_obj_t* medal;
    lv_obj_t* newTag;
    lv_obj_t* rankNote;
    lv_obj_t* overButtons;  // hidden for a top 5 score (name entry follows)
    lv_obj_t* nameLabel;  // GameName page
  };
  GameView gv_ = {};

  // Live widgets refreshed in update(); nullptr when not on screen.
  lv_obj_t* payTimeLabel_ = nullptr;  // checkout timeout countdown
  lv_obj_t* freeShotLabel_ = nullptr;  // free shot countdown on Main
  lv_obj_t* grindArc_ = nullptr;
  lv_obj_t* grindTime_ = nullptr;   // seconds left, inside the ring
  lv_obj_t* grindTitle_ = nullptr;  // span group: message + 3 animated dots
  lv_span_t* grindDots_[3] = {};
  int grindDotsShown_ = -1;
  lv_obj_t* doneBar_ = nullptr;
  lv_obj_t* doneLabel_ = nullptr;

  char pinInput_[kPinLength + 1] = {};
  size_t pinLen_ = 0;
  bool pinWrong_ = false;
  lv_obj_t* pinDot_[kPinLength] = {};
  lv_obj_t* pinSub_ = nullptr;  // "Wrong PIN" after a wrong entry, else empty

  lv_obj_t* priceLabel_[kMaxSettingsRows] = {};
  lv_obj_t* timeLabel_[kMaxSettingsRows] = {};
  lv_obj_t* manualBtn_ = nullptr;  // Settings: hold to run the motor
  // Settings: "minimum €1.00" note after - at the lowest price, hidden again
  // in update() at priceNoteUntilMs_.
  lv_obj_t* priceNote_ = nullptr;
  uint32_t priceNoteUntilMs_ = 0;
};

}  // namespace grinder
