#include "mhi_zjs.h"

#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include "esphome/core/preferences.h"

#include <cmath>
#include <cstring>

namespace esphome {
namespace mhi_zjs {

static const char *const TAG = "mhi_zjs.climate";

// Echo suppression window: a frame identical to our own last transmit arriving
// within this long is treated as our LED reflecting back into our receiver.
static const uint32_t MHI_ZJS_SELF_RX_MS = 2000;
// Anything arriving this soon after our own transmit is our echo regardless of
// content — the receiver's AGC can flip a bit, so an exact match is not enough.
static const uint32_t MHI_ZJS_ECHO_MS = 400;

// Index of the "Swing" entry in the vertical table.
static const uint8_t MHI_ZJS_V_SWING_INDEX = 5;

static inline bool v_position_is_swing(uint8_t b5, uint8_t b7) {
  return (b5 & 0x01) == MHI_ZJS_V_B5BIT[MHI_ZJS_V_SWING_INDEX] &&
         (b7 & 0x03) == MHI_ZJS_V_B7BITS[MHI_ZJS_V_SWING_INDEX];
}

void MhiZjsClimate::log_frame_(const char *prefix, const uint8_t *frame) {
  ESP_LOGD(TAG, "%s %02X %02X %02X %02X %02X | %02X %02X | %02X %02X | %02X %02X", prefix, frame[0], frame[1],
           frame[2], frame[3], frame[4], frame[5], frame[6], frame[7], frame[8], frame[9], frame[10]);
}

void MhiZjsClimate::dump_config() {
  ESP_LOGCONFIG(TAG, "Mitsubishi Heavy Industries SRK (ZJS 88-bit) IR Climate");
  this->dump_traits_(TAG);
}

// byte layout of the saved value: bits 0-3 horizontal, 4-5 vertical upper,
// bit 6 vertical low bit.
void MhiZjsClimate::save_vane_() {
  const uint8_t v = static_cast<uint8_t>((this->h_pos_ & 0x0F) | ((this->v_b7_ & 0x03) << 4) |
                                         ((this->v_b5_ & 0x01) << 6));
  this->vane_pref_.save(&v);
}

void MhiZjsClimate::setup() {
  climate_ir::ClimateIR::setup();
  this->vane_pref_ = global_preferences->make_preference<uint8_t>(0x4D48495A);
  uint8_t v = 0;
  if (this->vane_pref_.load(&v)) {
    this->h_pos_ = v & 0x0F;
    this->v_b7_ = (v >> 4) & 0x03;
    this->v_b5_ = (v >> 6) & 0x01;
    this->swing_mode = this->derived_swing_();
    this->last_swing_ = this->swing_mode;
  }
  this->publish_positions_();
}

climate::ClimateSwingMode MhiZjsClimate::derived_swing_() const {
  const bool v = v_position_is_swing(this->v_b5_, this->v_b7_);
  const bool h = (this->h_pos_ & 0x0F) == MHI_ZJS_H_SWING;
  if (v && h)
    return climate::CLIMATE_SWING_BOTH;
  if (v)
    return climate::CLIMATE_SWING_VERTICAL;
  if (h)
    return climate::CLIMATE_SWING_HORIZONTAL;
  return climate::CLIMATE_SWING_OFF;
}

void MhiZjsClimate::apply_swing_request_() {
  const bool want_v = this->swing_mode == climate::CLIMATE_SWING_VERTICAL ||
                      this->swing_mode == climate::CLIMATE_SWING_BOTH;
  const bool want_h = this->swing_mode == climate::CLIMATE_SWING_HORIZONTAL ||
                      this->swing_mode == climate::CLIMATE_SWING_BOTH;
  const bool have_v = v_position_is_swing(this->v_b5_, this->v_b7_);
  const bool have_h = (this->h_pos_ & 0x0F) == MHI_ZJS_H_SWING;

  if (want_v && !have_v) {
    this->v_b5_ = MHI_ZJS_V_B5BIT[MHI_ZJS_V_SWING_INDEX];
    this->v_b7_ = MHI_ZJS_V_B7BITS[MHI_ZJS_V_SWING_INDEX];
  } else if (!want_v && have_v) {
    // Leaving swing: park the vane at the centre pose.
    this->v_b5_ = MHI_ZJS_V_B5BIT[MHI_ZJS_V_CENTRE_INDEX];
    this->v_b7_ = MHI_ZJS_V_B7BITS[MHI_ZJS_V_CENTRE_INDEX];
  }
  if (want_h && !have_h) {
    this->h_pos_ = MHI_ZJS_H_SWING;
  } else if (!want_h && have_h) {
    this->h_pos_ = MHI_ZJS_H_CENTRE;
  }
}

void MhiZjsClimate::build_frame_(uint8_t out[MHI_ZJS_BYTES]) {
  const bool power = this->mode != climate::CLIMATE_MODE_OFF;
  const climate::ClimateMode m = power ? this->mode : this->last_active_mode_;

  for (uint8_t i = 0; i < 5; i++)
    out[i] = MHI_ZJS_SIG[i];

  // Byte 5: horizontal louver (bits 4-7), vertical vane low bit (bit 0),
  // allergen clear (bit 1), bits 2-3 inherited from the remote.
  out[5] = static_cast<uint8_t>((this->h_pos_ << 4) | (this->v_b5_ & 0x01) |
                                (this->allergen_ ? MHI_ZJS_ALLERGEN : 0x00) | (this->b5_mid_flags_ & 0x0C));
  out[6] = static_cast<uint8_t>(~out[5]);

  // Byte 7: fan (bits 5-7), vertical vane upper bits (bits 3-4), inherited flags.
  uint8_t fan = MHI_ZJS_FAN_AUTO;
  if (this->preset.has_value() && this->preset.value() == climate::CLIMATE_PRESET_BOOST) {
    fan = MHI_ZJS_FAN_HIPOWER;
  } else if (this->preset.has_value() && this->preset.value() == climate::CLIMATE_PRESET_ECO) {
    fan = MHI_ZJS_FAN_ECONO;
  } else if (this->fan_mode.has_value()) {
    switch (this->fan_mode.value()) {
      case climate::CLIMATE_FAN_QUIET:
        fan = 1;
        break;
      case climate::CLIMATE_FAN_LOW:
        fan = 2;
        break;
      case climate::CLIMATE_FAN_MEDIUM:
        fan = 3;
        break;
      case climate::CLIMATE_FAN_HIGH:
        fan = 4;
        break;
      default:
        fan = MHI_ZJS_FAN_AUTO;
        break;
    }
  }
  out[7] = static_cast<uint8_t>(((fan & 0x07) << 5) | ((this->v_b7_ & 0x03) << 3) | (this->b7_low_flags_ & 0x07));
  out[8] = static_cast<uint8_t>(~out[7]);

  // Byte 9: mode (bits 0-2), power (bit 3), temperature (bits 4-7).
  uint8_t mode_code;
  switch (m) {
    case climate::CLIMATE_MODE_HEAT:
      mode_code = 4;
      break;
    case climate::CLIMATE_MODE_DRY:
      mode_code = 2;
      break;
    case climate::CLIMATE_MODE_FAN_ONLY:
      mode_code = 3;
      break;
    case climate::CLIMATE_MODE_HEAT_COOL:
    case climate::CLIMATE_MODE_AUTO:
      mode_code = 0;
      break;
    case climate::CLIMATE_MODE_COOL:
    default:
      mode_code = 1;
      break;
  }
  int temp = static_cast<int>(lroundf(this->target_temperature));
  if (temp < static_cast<int>(MHI_ZJS_TEMP_MIN))
    temp = static_cast<int>(MHI_ZJS_TEMP_MIN);
  if (temp > static_cast<int>(MHI_ZJS_TEMP_MAX))
    temp = static_cast<int>(MHI_ZJS_TEMP_MAX);
  const uint8_t temp_code = static_cast<uint8_t>(temp - 17) & 0x0F;  // 18..30 -> 1..13

  out[9] = static_cast<uint8_t>((mode_code & 0x07) | (power ? MHI_ZJS_POWER_ON : 0x00) | (temp_code << 4));
  out[10] = static_cast<uint8_t>(~out[9]);
}

void MhiZjsClimate::publish_positions_() {
  // Publishing into a wired select runs its on_value automation, which calls
  // straight back into the setters below. Without this guard that is an
  // infinite loop (stack overflow). Re-entry is simply ignored.
  if (this->publishing_)
    return;
  this->publishing_ = true;

  if (this->v_select_ != nullptr) {
    for (uint8_t i = 0; i < 7; i++) {
      if (MHI_ZJS_V_B5BIT[i] == (this->v_b5_ & 0x01) && MHI_ZJS_V_B7BITS[i] == (this->v_b7_ & 0x03)) {
        this->v_select_->publish_state(MHI_ZJS_V_NAMES[i]);
        break;
      }
    }
  }
  if (this->h_select_ != nullptr) {
    for (uint8_t i = 0; i < 9; i++) {
      if (MHI_ZJS_H_CYCLE[i] == (this->h_pos_ & 0x0F)) {
        this->h_select_->publish_state(MHI_ZJS_H_NAMES[i]);
        break;
      }
    }
  }

  this->publishing_ = false;
}

void MhiZjsClimate::set_vertical_by_index(uint8_t index) {
  if (index >= 7 || this->publishing_)
    return;
  this->v_b5_ = MHI_ZJS_V_B5BIT[index];
  this->v_b7_ = MHI_ZJS_V_B7BITS[index];
  this->swing_mode = this->derived_swing_();
  this->last_swing_ = this->swing_mode;
  this->save_vane_();
  this->publish_positions_();
  this->transmit_state();
  this->publish_state();
}

void MhiZjsClimate::set_horizontal_by_index(uint8_t index) {
  if (index >= 9 || this->publishing_)
    return;
  this->h_pos_ = MHI_ZJS_H_CYCLE[index];
  this->swing_mode = this->derived_swing_();
  this->last_swing_ = this->swing_mode;
  this->save_vane_();
  this->publish_positions_();
  this->transmit_state();
  this->publish_state();
}

void MhiZjsClimate::set_allergen(bool on) {
  this->allergen_ = on;
  this->transmit_state();
}

void MhiZjsClimate::transmit_state() {
  if (this->mode != climate::CLIMATE_MODE_OFF)
    this->last_active_mode_ = this->mode;

  // A swing change that came from HA is turned into vane positions once. The
  // vane fields are otherwise driven directly by the selects and the remote.
  if (this->swing_mode != this->last_swing_) {
    this->apply_swing_request_();
    this->last_swing_ = this->swing_mode;
  }

  uint8_t frame[MHI_ZJS_BYTES];
  this->build_frame_(frame);
  std::memcpy(this->last_tx_, frame, MHI_ZJS_BYTES);
  this->have_last_tx_ = true;
  this->last_transmit_ms_ = millis();

  auto transmit = this->transmitter_->transmit();
  auto *data = transmit.get_data();
  data->set_carrier_frequency(MHI_ZJS_CARRIER);
  data->reserve(2 + MHI_ZJS_BYTES * 8 * 2 + 1);

  data->mark(MHI_ZJS_HEADER_MARK);
  data->space(MHI_ZJS_HEADER_SPACE);
  for (uint8_t i = 0; i < MHI_ZJS_BYTES; i++) {
    for (uint8_t b = 0; b < 8; b++) {  // LSB first
      data->mark(MHI_ZJS_BIT_MARK);
      data->space((frame[i] >> b) & 1 ? MHI_ZJS_ONE_SPACE : MHI_ZJS_ZERO_SPACE);
    }
  }
  data->mark(MHI_ZJS_END_MARK);

  this->log_frame_("TX", frame);
  transmit.perform();

  // A swing request from HA moves the vanes, so keep the dropdowns in step.
  this->publish_positions_();
}

bool MhiZjsClimate::on_receive(remote_base::RemoteReceiveData data) {
  if (!data.expect_item(MHI_ZJS_HEADER_MARK, MHI_ZJS_HEADER_SPACE))
    return false;

  uint8_t frame[MHI_ZJS_BYTES] = {0};
  for (uint8_t i = 0; i < MHI_ZJS_BYTES; i++) {
    for (uint8_t b = 0; b < 8; b++) {
      // Accept ANY mark length: the receiver AGC droops the mark across the
      // frame, so only the space after it carries the bit value.
      if (!data.is_valid(1) || data.peek() <= 0)
        return false;
      data.advance();  // consume the mark
      int32_t space = data.peek();
      data.advance();
      if (space >= 0)
        return false;  // expected a space
      if (static_cast<uint32_t>(-space) < MHI_ZJS_SPACE_THRESHOLD)
        frame[i] |= static_cast<uint8_t>(1u << b);  // short space = 1
    }
  }

  // Strict signature check — 40 bits, enough to reject foreign protocols.
  for (uint8_t i = 0; i < 5; i++) {
    if (frame[i] != MHI_ZJS_SIG[i])
      return false;
  }

  // Ignore our own LED echo. Within a short window assume echo regardless of
  // content; beyond that, require an exact match.
  if (this->have_last_tx_) {
    const uint32_t since = millis() - this->last_transmit_ms_;
    if (since < MHI_ZJS_ECHO_MS ||
        (since < MHI_ZJS_SELF_RX_MS && std::memcmp(frame, this->last_tx_, MHI_ZJS_BYTES) == 0)) {
      ESP_LOGV(TAG, "RX ignored: own transmit echo");
      return false;
    }
  }

  this->log_frame_("RX", frame);

  // Everything the remote reports becomes our state, including the bits we
  // have not decoded, so a later transmit preserves whatever was on.
  this->b5_mid_flags_ = frame[5] & 0x0C;
  this->b7_low_flags_ = frame[7] & 0x07;
  this->allergen_ = (frame[5] & MHI_ZJS_ALLERGEN) != 0;
  this->v_b5_ = frame[5] & 0x01;
  this->v_b7_ = (frame[7] >> 3) & 0x03;
  this->h_pos_ = (frame[5] >> 4) & 0x0F;
  this->swing_mode = this->derived_swing_();
  this->last_swing_ = this->swing_mode;
  this->save_vane_();

  const bool power = (frame[9] & MHI_ZJS_POWER_ON) != 0;

  if (!power) {
    this->mode = climate::CLIMATE_MODE_OFF;
  } else {
    switch (frame[9] & 0x07) {
      case 0:
        this->mode = climate::CLIMATE_MODE_AUTO;
        break;
      case 2:
        this->mode = climate::CLIMATE_MODE_DRY;
        break;
      case 3:
        this->mode = climate::CLIMATE_MODE_FAN_ONLY;
        break;
      case 4:
        this->mode = climate::CLIMATE_MODE_HEAT;
        break;
      case 1:
      default:
        this->mode = climate::CLIMATE_MODE_COOL;
        break;
    }
    this->last_active_mode_ = this->mode;
  }

  const uint8_t temp_code = (frame[9] >> 4) & 0x0F;
  if (temp_code >= 1 && temp_code <= 13)
    this->target_temperature = static_cast<float>(temp_code + 17);

  const uint8_t fan = (frame[7] >> 5) & 0x07;
  if (fan == MHI_ZJS_FAN_HIPOWER || fan == MHI_ZJS_FAN_ECONO) {
    this->preset = fan == MHI_ZJS_FAN_HIPOWER ? climate::CLIMATE_PRESET_BOOST : climate::CLIMATE_PRESET_ECO;
  } else {
    this->preset = climate::CLIMATE_PRESET_NONE;
    switch (fan) {
      case 1:
        this->fan_mode = climate::CLIMATE_FAN_QUIET;
        break;
      case 2:
        this->fan_mode = climate::CLIMATE_FAN_LOW;
        break;
      case 3:
        this->fan_mode = climate::CLIMATE_FAN_MEDIUM;
        break;
      case 4:
        this->fan_mode = climate::CLIMATE_FAN_HIGH;
        break;
      default:
        this->fan_mode = climate::CLIMATE_FAN_AUTO;
        break;
    }
  }

  this->publish_positions_();
  this->publish_state();
  return true;
}

}  // namespace mhi_zjs
}  // namespace esphome
