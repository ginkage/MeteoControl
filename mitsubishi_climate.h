#include "climate_ir.h"
#include "esphome.h"
#include <MitsubishiHeavyHeatpumpIR.h>

class ESPHomeIRSender : public IRSender {
public:
    ESPHomeIRSender(remote_base::RemoteTransmitData* transmitter)
        : IRSender(0)
    {
        transmitter_ = transmitter;
    }

    void setFrequency(int frequency) override
    {
        transmitter_->set_carrier_frequency(frequency * 1000u);
    }

    void space(int spaceLength) override { transmitter_->space(spaceLength); }

    void mark(int markLength) override { transmitter_->mark(markLength); }

protected:
    remote_base::RemoteTransmitData* transmitter_;
};

class MitsubishiClimate : public climate_ir::ClimateIR {
public:
    MitsubishiClimate()
        : ClimateIR(16, 36, 1.0f, true, true,
              { climate::CLIMATE_FAN_AUTO, climate::CLIMATE_FAN_LOW, climate::CLIMATE_FAN_MEDIUM,
                  climate::CLIMATE_FAN_HIGH },
              { climate::CLIMATE_SWING_OFF, climate::CLIMATE_SWING_BOTH,
                  climate::CLIMATE_SWING_VERTICAL, climate::CLIMATE_SWING_HORIZONTAL })
    {
    }

    void transmit_state() override
    {
        power_ = POWER_ON;

        switch (mode) {
        case climate::CLIMATE_MODE_OFF:
            power_ = POWER_OFF;
            break;
        case climate::CLIMATE_MODE_COOL:
            mode_ = MODE_COOL;
            break;
        case climate::CLIMATE_MODE_HEAT:
            mode_ = MODE_HEAT;
            break;
        case climate::CLIMATE_MODE_DRY:
            mode_ = MODE_DRY;
            break;
        case climate::CLIMATE_MODE_FAN_ONLY:
            mode_ = MODE_FAN;
            break;
        case climate::CLIMATE_MODE_AUTO:
        default:
            mode_ = MODE_AUTO;
            break;
        }

        switch (fan_mode) {
        case climate::CLIMATE_FAN_LOW:
            fan_ = FAN_1;
            break;
        case climate::CLIMATE_FAN_MEDIUM:
            fan_ = FAN_2;
            break;
        case climate::CLIMATE_FAN_HIGH:
            fan_ = FAN_3;
            break;
        case climate::CLIMATE_FAN_AUTO:
        default:
            fan_ = FAN_AUTO;
            break;
        }

        switch (swing_mode) {
        case climate::CLIMATE_SWING_OFF:
            vswing_ = VDIR_UP;
            hswing_ = HDIR_MIDDLE;
            break;
        case climate::CLIMATE_SWING_VERTICAL:
            vswing_ = VDIR_SWING;
            hswing_ = HDIR_MIDDLE;
            break;
        case climate::CLIMATE_SWING_HORIZONTAL:
            vswing_ = VDIR_UP;
            hswing_ = HDIR_SWING;
            break;
        case climate::CLIMATE_SWING_BOTH:
        default:
            vswing_ = VDIR_SWING;
            hswing_ = HDIR_SWING;
            break;
        }

        target_ = (uint8_t)roundf(
            clamp(target_temperature, minimum_temperature_, maximum_temperature_));

        auto transmit = transmitter_->transmit();
        ESPHomeIRSender sender(transmit.get_data());

        bool cleanMode = false;
        bool silentMode = true;
        bool _3DAuto = (hswing_ == HDIR_SWING) && (vswing_ == VDIR_SWING);
        heatpump_.send(
            sender, power_, mode_, fan_, target_, vswing_, hswing_, cleanMode, silentMode, _3DAuto);

        transmit.perform();
    }

    char* print_state1(char* buffer)
    {
        sprintf(buffer, " power: %d, mode: %d, fan: %d", power_,  mode_, fan_);
        return buffer;
    }

    char* print_state2(char* buffer)
    {
        sprintf(buffer, " t: %d, vs: %d, hs: %d", target_, vswing_, hswing_);
        return buffer;
    }

protected:
    uint8_t power_ = POWER_OFF, mode_ = MODE_AUTO, fan_ = FAN_AUTO, target_ = 23,
            vswing_ = VDIR_SWING, hswing_ = HDIR_SWING;
    MitsubishiHeavyZJHeatpumpIR heatpump_;
};
