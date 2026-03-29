#pragma once
#include "esphome.h"
#include "bl0937.h"

namespace esphome {
    namespace bl0937_fast {

        class BL0937Fast : public Component {
        public:
            uint8_t pin_;
            float multiplier_;
            volatile uint32_t pulse_interval_us = 0;
            volatile uint32_t last_cf_time = 0;

            BL0937Fast(uint8_t pin, float multiplier) : pin_(pin), multiplier_(multiplier) {}

            void setup() override; // We define this below the class

            float get_instantaneous_power() {
                uint32_t now = micros();
                // Reset to 0 if no pulse for 1.2 seconds
                if (now - last_cf_time > 1200000) return 0.0f;
                if (pulse_interval_us == 0) return 0.0f;
                return (1000000.0f / (float)pulse_interval_us) * multiplier_;
            }
        };

        // Static pointer to the instance
        static BL0937Fast *global_bl0937_instance = nullptr;

        // The ISR function now knows exactly what BL0937Fast contains
        static void IRAM_ATTR cf_isrv() {
            if (global_bl0937_instance != nullptr) {
                uint32_t now = micros();
                uint32_t diff = now - global_bl0937_instance->last_cf_time;
                if (diff > 100) {
                    global_bl0937_instance->pulse_interval_us = diff;
                    global_bl0937_instance->last_cf_time = now;
                }
            }
        }

        // Now we define the setup method that was declared inside the class
        inline void BL0937Fast::setup() {
            global_bl0937_instance = this;
            pinMode(pin_, INPUT_PULLUP);
            attachInterrupt(digitalPinToInterrupt(pin_), cf_isrv, FALLING);
        }

    } // namespace bl0937_fast
} // namespace esphome
