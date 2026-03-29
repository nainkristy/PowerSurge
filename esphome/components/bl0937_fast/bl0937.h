#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
    #endif

    typedef struct {
        gpio_num_t gpio_cf; // CF: active power pulse output
        gpio_num_t gpio_cf1; // CF1: voltage/current pulse output
        gpio_num_t gpio_sel; // SEL: selects CF1 voltage/current

        // Board parameters for physical units (required for V/A/W):
        int shunt_uohm; // shunt resistor in micro-ohms
        float v_div_ratio; // Vpin_rms / Vline_rms
        float vref_v;  // BL0937 reference voltage (typ. 1.218V)

        // Timing:
        int sample_period_ms; // full period; voltage measured half, current measured half
        int sel_settle_us; // delay after SEL toggle before counting CF1 pulses

        // Calibration trim (optional):
        float voltage_calibration;
        float current_calibration;
        float power_calibration;

        // PCNT glitch filter in nanoseconds (PCNT mode only). 0 disables.
        int glitch_filter_ns;

        // Optional: if true, disables PCNT and uses GPIO ISR counting.
        bool force_isr_fallback;
    } bl0937_config_t;

    typedef struct {
        // Physical values (valid_* indicates when these are meaningful):
        float voltage_v;
        float current_a;
        float power_w;
        float energy_wh;

        // Raw measured frequencies (Hz):
        float cf_hz; // active power
        float cfu_hz; // voltage (CF1 with SEL=1)
        float cfi_hz; // current (CF1 with SEL=0)

        // Flags:
        bool valid_voltage;
        bool valid_current;
        bool valid_power;
    } bl0937_measurements_t;

    /**
     * @brief Get a config populated from Kconfig defaults.
     */
    bl0937_config_t bl0937_config_default(void);

    /**
     * @brief Initialize the driver (does not start counting yet).
     */
    esp_err_t bl0937_init(const bl0937_config_t *cfg);

    /**
     * @brief Start periodic measurements.
     */
    esp_err_t bl0937_start(void);

    /**
     * @brief Stop periodic measurements.
     */
    esp_err_t bl0937_stop(void);

    /**
     * @brief Deinitialize driver and free resources.
     */
    esp_err_t bl0937_deinit(void);

    /**
     * @brief Get the latest measurements snapshot (thread-safe copy).
     */
    bl0937_measurements_t bl0937_get(void);

    /**
     * @brief Reset accumulated energy counter to zero.
     */
    void bl0937_reset_energy(void);

    /**
     * @brief Helper to compute v_div_ratio from resistor divider (Rtop from line to pin, Rbottom from pin to GND).
     *
     * v_div_ratio = Rbottom / (Rtop + Rbottom)
     */
    static inline float bl0937_div_ratio_from_resistors(float r_top_ohm, float r_bottom_ohm)
    {
        if (r_top_ohm <= 0.0f || r_bottom_ohm <= 0.0f) return 0.0f;
        return r_bottom_ohm / (r_top_ohm + r_bottom_ohm);
    }

    #ifdef __cplusplus
}
#endif
