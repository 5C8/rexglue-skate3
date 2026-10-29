/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstring>
#include <string>
#include <string_view>

#include <rex/cvar.h>
#include <rex/dbg.h>
#include <rex/input/flags.h>
#include <rex/input/input_driver.h>
#include <rex/input/input_system.h>
#include <rex/input/mnk/mnk_input_driver.h>
#include <rex/input/nop/nop_input_driver.h>
#include <rex/input/sdl/sdl_input_driver.h>
#include <rex/input/xinput/xinput_input_driver.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/window_listener.h>

#if REX_PLATFORM_WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

constexpr const char* kDefaultInputBackend =
#if REX_PLATFORM_WIN32
    "auto";
#else
    "sdl";
#endif

}  // namespace

REXCVAR_DEFINE_STRING(input_backend, kDefaultInputBackend, "Input",
                      "Input backend: auto (Windows: XInput for Xbox pads + SDL for "
                      "PlayStation / Switch / generic pads, e.g. DualSense), xinput, sdl")
    .allowed({"auto", "sdl", "xinput"});

REXCVAR_DEFINE_STRING(input_bound_pad, "", "Input",
                      "Splitscreen: the one controller this instance reads, as guest player 1 "
                      "('xinput:N' or 'sdl:<device path>'), focused or not. Empty = every "
                      "controller, in the focused window only.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(guide_button, false, "Input", "Enable guide button pass-through");
// Back+Start is deliberately not the default: Steam Input uses View+Menu
// (Back+Start) as its Guide-button chord on pads without a Guide button, so
// opening our settings would also pop the Steam overlay.
REXCVAR_DEFINE_STRING(menu_chord, "rb+start", "Input",
                      "Controller chord that toggles the settings menu. Buttons joined by '+': "
                      "a, b, x, y, lb, rb, l3, r3, back, start, dpad_up, dpad_down, dpad_left, "
                      "dpad_right. Empty disables the chord.");
REXCVAR_DEFINE_BOOL(hid_rumble_enabled, true, "Input", "Enable controller vibration");
REXCVAR_DEFINE_UINT32(hid_rumble_min_motor_speed, 0x1000, "Input",
                      "Minimum XInput motor speed forwarded to host controllers")
    .range(0, 0xFFFF);
namespace rex::input {
namespace {

uint16_t ChordButtonFromToken(std::string_view token) {
  if (token == "a") return X_INPUT_GAMEPAD_A;
  if (token == "b") return X_INPUT_GAMEPAD_B;
  if (token == "x") return X_INPUT_GAMEPAD_X;
  if (token == "y") return X_INPUT_GAMEPAD_Y;
  if (token == "lb") return X_INPUT_GAMEPAD_LEFT_SHOULDER;
  if (token == "rb") return X_INPUT_GAMEPAD_RIGHT_SHOULDER;
  if (token == "l3") return X_INPUT_GAMEPAD_LEFT_THUMB;
  if (token == "r3") return X_INPUT_GAMEPAD_RIGHT_THUMB;
  if (token == "back" || token == "select" || token == "view") return X_INPUT_GAMEPAD_BACK;
  if (token == "start" || token == "menu") return X_INPUT_GAMEPAD_START;
  if (token == "dpad_up") return X_INPUT_GAMEPAD_DPAD_UP;
  if (token == "dpad_down") return X_INPUT_GAMEPAD_DPAD_DOWN;
  if (token == "dpad_left") return X_INPUT_GAMEPAD_DPAD_LEFT;
  if (token == "dpad_right") return X_INPUT_GAMEPAD_DPAD_RIGHT;
  return 0;
}

// "lb+rb+start" -> button mask; 0 when empty or no token parses (chord off).
uint16_t ChordMaskFromSpec(std::string_view spec) {
  uint16_t mask = 0;
  std::string token;
  for (size_t i = 0; i <= spec.size(); ++i) {
    char c = i < spec.size() ? spec[i] : '+';
    if (c == '+' || c == ',' || c == ' ') {
      if (!token.empty()) {
        mask |= ChordButtonFromToken(token);
        token.clear();
      }
      continue;
    }
    token.push_back(char(std::tolower(static_cast<unsigned char>(c))));
  }
  return mask;
}

std::atomic<bool> g_ignore_focus{false};  // InputSystem::SetIgnoreFocus

// Game input follows the focused window: controllers are read system-wide,
// so with several instances open (two local online clients, say) one pad
// would otherwise drive every window at once.
bool ProcessHasForegroundWindow() {
  if (g_ignore_focus.load(std::memory_order_relaxed)) {
    return true;
  }
#if REX_PLATFORM_WIN32
  HWND foreground = GetForegroundWindow();
  if (!foreground) {
    return false;
  }
  DWORD pid = 0;
  GetWindowThreadProcessId(foreground, &pid);
  return pid == GetCurrentProcessId();
#else
  return true;
#endif
}

bool PadBound() {
  return !REXCVAR_GET(input_bound_pad).empty();
}

constexpr uint32_t kAnyUser = 0xFF;

}  // namespace

InputSystem::InputSystem(rex::ui::Window* window) : window_(window) {}

std::optional<uint32_t> InputSystem::RouteUser(InputDriver& driver, uint32_t user_index) const {
  const std::string bound = REXCVAR_GET(input_bound_pad);
  if (bound.empty()) {
    return user_index;
  }
  if (user_index != 0 && user_index != kAnyUser) {
    return std::nullopt;  // the bound pad is the only player
  }
  if (auto pad_user = driver.ResolvePad(bound)) {
    return pad_user;
  }
  // Keyboard/mouse is shared by every window: it follows focus as before.
  if (dynamic_cast<mnk::MnkInputDriver*>(&driver) && ProcessHasForegroundWindow()) {
    return user_index;
  }
  return std::nullopt;
}

InputSystem::~InputSystem() = default;

void InputSystem::SetIgnoreFocus(bool ignore) {
  g_ignore_focus.store(ignore, std::memory_order_relaxed);
}

X_STATUS InputSystem::Setup() {
  return X_STATUS_SUCCESS;
}

void InputSystem::Shutdown() {
  if (window_) {
    rex::ui::UIEvent closing_event(window_);
    for (auto& driver : drivers_) {
      if (auto* window_listener = dynamic_cast<rex::ui::WindowListener*>(driver.get())) {
        window_listener->OnClosing(closing_event);
      }
    }
    window_ = nullptr;
  }
  drivers_.clear();
}

void InputSystem::AddDriver(std::unique_ptr<InputDriver> driver) {
  drivers_.push_back(std::move(driver));
}

void InputSystem::PrependDriver(std::unique_ptr<InputDriver> driver) {
  drivers_.insert(drivers_.begin(), std::move(driver));
}

void InputSystem::AttachWindow(rex::ui::Window* window) {
  window_ = window;
  for (auto& driver : drivers_) {
    driver->OnWindowAvailable(window);
  }
}

void InputSystem::SetActiveCallback(std::function<bool()> callback) {
  active_callback_ = callback;
  for (auto& driver : drivers_) {
    driver->set_is_active_callback(callback);
  }
}

void InputSystem::SetMenuChordCallback(std::function<void()> callback) {
  menu_chord_callback_ = std::move(callback);
}

X_RESULT InputSystem::GetCapabilities(uint32_t user_index, uint32_t flags,
                                      X_INPUT_CAPABILITIES* out_caps) {
  SCOPE_profile_cpu_f("hid");

  bool any_connected = false;
  for (auto& driver : drivers_) {
    const auto routed = RouteUser(*driver, user_index);
    if (!routed) {
      continue;
    }
    X_RESULT result = driver->GetCapabilities(*routed, flags, out_caps);
    if (result != X_ERROR_DEVICE_NOT_CONNECTED) {
      any_connected = true;
    }
    if (result == X_ERROR_SUCCESS) {
      return result;
    }
  }
  return any_connected ? X_ERROR_EMPTY : X_ERROR_DEVICE_NOT_CONNECTED;
}

X_RESULT InputSystem::GetState(uint32_t user_index, X_INPUT_STATE* out_state) {
  SCOPE_profile_cpu_f("hid");

  bool any_connected = false;
  bool first_result = true;
  X_INPUT_STATE merged = {};

  for (auto& driver : drivers_) {
    const auto routed = RouteUser(*driver, user_index);
    if (!routed) {
      continue;
    }
    X_INPUT_STATE state = {};
    X_RESULT result = driver->GetState(*routed, &state);
    if (result != X_ERROR_DEVICE_NOT_CONNECTED) {
      any_connected = true;
    }
    if (result == X_ERROR_SUCCESS) {
      if (first_result) {
        merged = state;
        first_result = false;
      } else {
        // Merge: OR buttons, max triggers, max-magnitude sticks
        merged.gamepad.buttons = static_cast<uint16_t>(merged.gamepad.buttons) |
                                 static_cast<uint16_t>(state.gamepad.buttons);
        merged.gamepad.left_trigger =
            std::max(merged.gamepad.left_trigger, state.gamepad.left_trigger);
        merged.gamepad.right_trigger =
            std::max(merged.gamepad.right_trigger, state.gamepad.right_trigger);

        auto merge_axis = [](int16_t a, int16_t b) -> int16_t {
          return (std::abs(static_cast<int>(a)) >= std::abs(static_cast<int>(b))) ? a : b;
        };
        merged.gamepad.thumb_lx = merge_axis(merged.gamepad.thumb_lx, state.gamepad.thumb_lx);
        merged.gamepad.thumb_ly = merge_axis(merged.gamepad.thumb_ly, state.gamepad.thumb_ly);
        merged.gamepad.thumb_rx = merge_axis(merged.gamepad.thumb_rx, state.gamepad.thumb_rx);
        merged.gamepad.thumb_ry = merge_axis(merged.gamepad.thumb_ry, state.gamepad.thumb_ry);

        if (static_cast<uint32_t>(state.packet_number) >
            static_cast<uint32_t>(merged.packet_number)) {
          merged.packet_number = state.packet_number;
        }
      }
    }
  }

  if (first_result) {
    return any_connected ? X_ERROR_EMPTY : X_ERROR_DEVICE_NOT_CONNECTED;
  }

  // An unfocused window sees a connected pad with nothing pressed (unless
  // this instance owns its pad: RouteUser already dropped the others).
  if (!PadBound() && !ProcessHasForegroundWindow()) {
    std::memset(&merged.gamepad, 0, sizeof(merged.gamepad));
  }

  const uint16_t menu_chord_buttons = ChordMaskFromSpec(REXCVAR_GET(menu_chord));
  const bool menu_chord_down =
      menu_chord_buttons != 0 &&
      (static_cast<uint16_t>(merged.gamepad.buttons) & menu_chord_buttons) == menu_chord_buttons;
  if (menu_chord_down && !menu_chord_down_ && menu_chord_callback_) {
    menu_chord_callback_();
  }
  menu_chord_down_ = menu_chord_down;

  if (active_callback_ && !active_callback_()) {
    std::memset(&merged.gamepad, 0, sizeof(merged.gamepad));
  }

  if (out_state) {
    *out_state = merged;
  }
  return X_ERROR_SUCCESS;
}

bool InputSystem::GetUiGamepadState(X_INPUT_GAMEPAD* out_gamepad) {
  X_INPUT_GAMEPAD merged = {};
  bool any = false;

  auto merge_axis = [](int16_t a, int16_t b) -> int16_t {
    return (std::abs(static_cast<int>(a)) >= std::abs(static_cast<int>(b))) ? a : b;
  };

  const std::string bound = REXCVAR_GET(input_bound_pad);
  for (auto& driver : drivers_) {
    // Overlays in a splitscreen window follow its own pad only.
    const std::optional<uint32_t> pad_user =
        bound.empty() ? std::nullopt : driver->ResolvePad(bound);
    for (uint32_t user_index = 0; user_index < 4; ++user_index) {
      if (!bound.empty() && pad_user != user_index) {
        continue;
      }
      X_INPUT_STATE state = {};
      if (driver->GetStateUi(user_index, &state) != X_ERROR_SUCCESS) {
        continue;
      }
      any = true;
      merged.buttons = static_cast<uint16_t>(merged.buttons) |
                       static_cast<uint16_t>(state.gamepad.buttons);
      merged.left_trigger = std::max(merged.left_trigger, state.gamepad.left_trigger);
      merged.right_trigger = std::max(merged.right_trigger, state.gamepad.right_trigger);
      merged.thumb_lx = merge_axis(merged.thumb_lx, state.gamepad.thumb_lx);
      merged.thumb_ly = merge_axis(merged.thumb_ly, state.gamepad.thumb_ly);
      merged.thumb_rx = merge_axis(merged.thumb_rx, state.gamepad.thumb_rx);
      merged.thumb_ry = merge_axis(merged.thumb_ry, state.gamepad.thumb_ry);
    }
  }

  if (out_gamepad) {
    *out_gamepad = merged;
  }
  return any;
}

X_RESULT InputSystem::SetState(uint32_t user_index, X_INPUT_VIBRATION* vibration) {
  SCOPE_profile_cpu_f("hid");

  if (!vibration) {
    return X_ERROR_BAD_ARGUMENTS;
  }

  // Unfocused windows leave the (shared) pad alone; writing zero here would
  // cancel the focused instance's rumble. A bound pad is this window's own.
  if (!PadBound() && !ProcessHasForegroundWindow()) {
    return X_ERROR_SUCCESS;
  }

  X_INPUT_VIBRATION filtered_vibration = {};
  if (REXCVAR_GET(hid_rumble_enabled)) {
    const uint32_t min_motor_speed = REXCVAR_GET(hid_rumble_min_motor_speed);
    const auto filter_motor = [min_motor_speed](uint16_t speed) -> uint16_t {
      return speed >= min_motor_speed ? speed : 0;
    };
    filtered_vibration.left_motor_speed =
        filter_motor(static_cast<uint16_t>(vibration->left_motor_speed));
    filtered_vibration.right_motor_speed =
        filter_motor(static_cast<uint16_t>(vibration->right_motor_speed));
  }

  bool any_connected = false;
  for (auto& driver : drivers_) {
    const auto routed = RouteUser(*driver, user_index);
    if (!routed) {
      continue;
    }
    X_RESULT result = driver->SetState(*routed, &filtered_vibration);
    if (result != X_ERROR_DEVICE_NOT_CONNECTED) {
      any_connected = true;
    }
    if (result == X_ERROR_SUCCESS) {
      return result;
    }
  }
  return any_connected ? X_ERROR_EMPTY : X_ERROR_DEVICE_NOT_CONNECTED;
}

X_RESULT InputSystem::GetKeystroke(uint32_t user_index, uint32_t flags,
                                   X_INPUT_KEYSTROKE* out_keystroke) {
  SCOPE_profile_cpu_f("hid");

  const bool bound = PadBound();
  if (!bound && !ProcessHasForegroundWindow()) {
    return X_ERROR_EMPTY;
  }

  bool any_connected = false;
  for (auto& driver : drivers_) {
    const auto routed = RouteUser(*driver, user_index);
    if (!routed) {
      continue;
    }
    X_RESULT result = driver->GetKeystroke(*routed, flags, out_keystroke);
    if (result != X_ERROR_DEVICE_NOT_CONNECTED) {
      any_connected = true;
    }
    if (result == X_ERROR_SUCCESS && bound && out_keystroke) {
      out_keystroke->user_index = 0;  // the bound pad is guest player 1
    }
    if (result == X_ERROR_SUCCESS || result == X_ERROR_EMPTY) {
      return result;
    }
  }
  return any_connected ? X_ERROR_EMPTY : X_ERROR_DEVICE_NOT_CONNECTED;
}

std::unique_ptr<InputSystem> CreateDefaultInputSystem(bool tool_mode) {
  auto input = std::make_unique<InputSystem>(nullptr);

  if (!tool_mode) {
    const std::string backend = REXCVAR_GET(input_backend);
#if REX_PLATFORM_WIN32
    const bool use_xinput = backend == "xinput" || backend == "auto";
#else
    const bool use_xinput = false;
#endif
    const bool use_sdl = backend == "sdl" || backend == "auto" || !use_xinput;
#if REX_PLATFORM_WIN32
    if (use_xinput) {
      auto xinput_driver = std::make_unique<xinput::XinputInputDriver>(nullptr, 0);
      if (xinput_driver->Setup() == X_STATUS_SUCCESS) {
        input->AddDriver(std::move(xinput_driver));
      }
    }
#endif

    if (use_sdl) {
      auto sdl_driver = std::make_unique<sdl::SDLInputDriver>(nullptr, 0);
      // "auto": XInput already serves the Xbox pads; SDL takes the rest
      // (DualSense / DualShock, Switch, generic HID pads).
      sdl_driver->set_skip_xinput_devices(use_xinput);
      if (sdl_driver->Setup() == X_STATUS_SUCCESS) {
        input->AddDriver(std::move(sdl_driver));
      }
    }

    // MnK driver (keyboard/mouse -> controller emulation)
    auto mnk_driver = std::make_unique<mnk::MnkInputDriver>(nullptr, 0);
    if (mnk_driver->Setup() == X_STATUS_SUCCESS) {
      input->AddDriver(std::move(mnk_driver));
    }
  }

  // NOP driver (primary in tool mode, fallback otherwise)
  uint8_t nop_index = tool_mode ? 0 : 1;
  input->AddDriver(std::make_unique<nop::NopInputDriver>(nullptr, nop_index));
  return input;
}

}  // namespace rex::input
