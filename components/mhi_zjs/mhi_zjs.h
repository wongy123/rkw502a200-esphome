#pragma once

#include "esphome/components/climate_ir/climate_ir.h"
#include "esphome/components/select/select.h"

namespace esphome {
namespace mhi_zjs {

// ─── Protocol ────────────────────────────────────────────────────────────────
// Mitsubishi Heavy Industries 88-bit "ZJS" protocol, used by the RKW502A200
// remote (SRK71ZK-S). Constants measured from the remote with this device's own
// receiver; byte layout verified against IRremoteESP8266's kMitsubishiHeavyZjsSig
// and against the remote's LCD labels.
//
// Physical bit encoding: constant-length mark, bit value in the following space.
//   bit 1 -> SHORT space, bit 0 -> LONG space.  Bytes are sent LSB first.
static const uint32_t MHI_ZJS_CARRIER = 38000;
static const uint32_t MHI_ZJS_HEADER_MARK = 3156;
static const uint32_t MHI_ZJS_HEADER_SPACE = 1585;
static const uint32_t MHI_ZJS_BIT_MARK = 400;
static const uint32_t MHI_ZJS_ONE_SPACE = 430;
static const uint32_t MHI_ZJS_ZERO_SPACE = 1220;
static const uint32_t MHI_ZJS_END_MARK = 400;
static const uint32_t MHI_ZJS_SPACE_THRESHOLD = 800;

static const uint8_t MHI_ZJS_BYTES = 11;
// kMitsubishiHeavyZjsSig, as observed on the wire from the RKW502A200.
static const uint8_t MHI_ZJS_SIG[5] = {0xAD, 0x51, 0x3C, 0xD9, 0x26};

static const float MHI_ZJS_TEMP_MIN = 18.0f;
static const float MHI_ZJS_TEMP_MAX = 30.0f;

// byte 7, bits 5-7: fan stage. 5 is unused by this remote.
static const uint8_t MHI_ZJS_FAN_AUTO = 0;
static const uint8_t MHI_ZJS_FAN_HIPOWER = 6;
static const uint8_t MHI_ZJS_FAN_ECONO = 7;

// Vertical vane: 7 states, encoded across TWO bytes — byte 5 bit 0 and byte 7
// bits 3-4. Verified against the remote's LCD labels for every position.
static const uint8_t MHI_ZJS_V_B5BIT[7] = {0, 1, 1, 1, 1, 0, 0};  // byte 5, bit 0
static const uint8_t MHI_ZJS_V_B7BITS[7] = {3, 0, 1, 2, 3, 2, 0};  // byte 7, bits 3-4
static const char *const MHI_ZJS_V_NAMES[7] = {"Up", "Up Centre", "Centre", "Down Centre",
                                                "Down", "Swing",     "Auto"};

// byte 5, bit 1: ALLERGEN CLEAR. Pressing the remote's ALLERGEN CLEAR button
// sets this and keeps power on; pressing it again clears it and powers off.
static const uint8_t MHI_ZJS_ALLERGEN = 0x02;

// byte 5, bits 4-7: horizontal louver. 9 states, verified against the LCD.
static const uint8_t MHI_ZJS_H_CYCLE[9] = {3, 4, 5, 6, 7, 8, 9, 2, 0};
static const char *const MHI_ZJS_H_NAMES[9] = {"Left Left",  "Left Centre", "Centre Centre", "Centre Right",
                                               "Right Right", "Left Right",  "Right Left",    "Swing",
                                               "Auto"};
// Horizontal swing, and the fixed poses used when swing is switched off.
static const uint8_t MHI_ZJS_H_SWING = 2;
static const uint8_t MHI_ZJS_H_CENTRE = 5;  // Centre Centre
static const uint8_t MHI_ZJS_V_CENTRE_INDEX = 2;  // Position 3

// byte 9: bits 0-2 mode, bit 3 power, bits 4-7 temperature as (degC - 17).
static const uint8_t MHI_ZJS_POWER_ON = 0x08;

class MhiZjsClimate final : public climate_ir::ClimateIR {
 public:
  MhiZjsClimate()
      : climate_ir::ClimateIR(MHI_ZJS_TEMP_MIN, MHI_ZJS_TEMP_MAX, 1.0f, true, true,
                              {climate::CLIMATE_FAN_AUTO, climate::CLIMATE_FAN_QUIET,
                               climate::CLIMATE_FAN_LOW, climate::CLIMATE_FAN_MEDIUM,
                               climate::CLIMATE_FAN_HIGH},
                              {climate::CLIMATE_SWING_OFF, climate::CLIMATE_SWING_VERTICAL,
                               climate::CLIMATE_SWING_HORIZONTAL, climate::CLIMATE_SWING_BOTH},
                              {climate::CLIMATE_PRESET_NONE, climate::CLIMATE_PRESET_BOOST,
                               climate::CLIMATE_PRESET_ECO}) {}

  void dump_config() override;
  void setup() override;

  // Called from YAML lambdas (select/switch entities).
  void set_vertical_by_index(uint8_t index);
  void set_horizontal_by_index(uint8_t index);
  void set_allergen(bool on);

  void set_vertical_select(select::Select *sel) { this->v_select_ = sel; }
  void set_horizontal_select(select::Select *sel) { this->h_select_ = sel; }

 protected:
  void transmit_state() override;
  bool on_receive(remote_base::RemoteReceiveData data) override;

  void build_frame_(uint8_t out[MHI_ZJS_BYTES]);
  static void log_frame_(const char *prefix, const uint8_t *frame);
  void publish_positions_();

  // A power-off frame carries no mode of its own, so remember the last one.
  climate::ClimateMode last_active_mode_{climate::CLIMATE_MODE_COOL};

  // Vane position and flags we command. Bits we have not decoded are inherited
  // from the remote (learned on receive) so we never clobber an unknown flag.
  uint8_t v_b5_{0};              // byte 5 bit 0   — vertical vane, low bit
  uint8_t v_b7_{3};              // byte 7 bits 3-4 — vertical vane, upper bits
  uint8_t h_pos_{0};             // byte 5 bits 4-7 — horizontal louver
  bool allergen_{false};         // byte 5 bit 1
  uint8_t b5_mid_flags_{0};      // byte 5 bits 2-3, learned from the remote
  uint8_t b7_low_flags_{0};      // byte 7 bits 0-2, learned from the remote

  // Guards the publish -> on_value -> set -> publish loop when the select
  // entities are wired to this component.
  bool publishing_{false};
  // Swing mode we last acted on, so a swing request from HA is applied once
  // rather than re-applied on every transmit.
  climate::ClimateSwingMode last_swing_{climate::CLIMATE_SWING_OFF};

  void apply_swing_request_();
  climate::ClimateSwingMode derived_swing_() const;

  // Vane positions survive a reboot; the climate's own state is restored by
  // ClimateIR, so without this the dropdowns would reset to defaults.
  ESPPreferenceObject vane_pref_;
  void save_vane_();

  select::Select *v_select_{nullptr};
  select::Select *h_select_{nullptr};

  // Last transmitted frame + time, to recognise our own IR echo.
  uint8_t last_tx_[MHI_ZJS_BYTES]{};
  bool have_last_tx_{false};
  uint32_t last_transmit_ms_{0};
};

}  // namespace mhi_zjs
}  // namespace esphome
