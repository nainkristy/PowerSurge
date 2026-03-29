/**
 *  Copyright 2026 Achim Pieters | StudioPieters®
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to deal
 *  in the Software without restriction, including without limitation the rights
 *  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *  copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NON INFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 *  WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 *  CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 *  for more information visit https://www.studiopieters.nl
 **/

#include "bl0937.h"
#include "sdkconfig.h"
#ifndef CONFIG_BL0937_ISR_FALLBACK
#define CONFIG_BL0937_ISR_FALLBACK 0
#endif

#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_check.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "driver/gpio.h"

#if __has_include("soc/soc_caps.h")
#include "soc/soc_caps.h"
#endif

#if defined(SOC_PCNT_SUPPORTED) && SOC_PCNT_SUPPORTED
#include "driver/pulse_cnt.h"
#endif

// Datasheet constants (BL0937 V1.02):
// FCF  = 1721506 * (V(V) * V(I)) / Vref^2
// FCFU = 15397   * V(V) / Vref
// FCFI = 94638   * V(I) / Vref
#define BL0937_K_CF   (1721506.0f)
#define BL0937_K_CFU  (15397.0f)
#define BL0937_K_CFI  (94638.0f)

static const char *TAG = "bl0937";

typedef enum {
    PHASE_VOLTAGE = 0,
    PHASE_CURRENT = 1,
} bl0937_phase_t;

typedef struct {
    bl0937_config_t cfg;
    bool initialized;
    bool running;

    esp_timer_handle_t timer;

    // phase state machine
    bl0937_phase_t phase;
    int64_t phase_start_us;
    int64_t last_full_update_us;

    // counters for current half-phase
    uint32_t cf_count_accum;
    uint32_t cf_count_prev;
    uint32_t cf1_count_prev;

    // last half-phase captured counts
    uint32_t last_cf_count; // counts during half-phase
    uint32_t last_cfu_count; // counts during voltage half-phase
    uint32_t last_cfi_count; // counts during current half-phase

    // computed measurement snapshot
    bl0937_measurements_t meas;

    portMUX_TYPE mux;

    #if defined(SOC_PCNT_SUPPORTED) && SOC_PCNT_SUPPORTED
    // PCNT resources
    pcnt_unit_handle_t unit_cf;
    pcnt_channel_handle_t chan_cf;
    pcnt_unit_handle_t unit_cf1;
    pcnt_channel_handle_t chan_cf1;
    bool pcnt_active;
    #endif

    // ISR fallback counters
    volatile uint32_t isr_cf;
    volatile uint32_t isr_cf1;
    bool isr_active;
} bl0937_ctx_t;

static bl0937_ctx_t s_ctx = {
    .mux = portMUX_INITIALIZER_UNLOCKED,
};

static float fparse(const char *s, float dflt)
{
    if (!s) return dflt;
    char *end = NULL;
    float v = strtof(s, &end);
    if (end == s) return dflt;
    return v;
}

bl0937_config_t bl0937_config_default(void)
{
    bl0937_config_t c = {0};
    #if CONFIG_BL0937_ENABLE
    c.gpio_cf  = (gpio_num_t)CONFIG_ESP_BL0937_CF_GPIO;
    c.gpio_cf1 = (gpio_num_t)CONFIG_ESP_BL0937_CF1_GPIO;
    c.gpio_sel = (gpio_num_t)CONFIG_ESP_BL0937_SEL_GPIO;

    c.shunt_uohm  = CONFIG_BL0937_SHUNT_UOHM;
    c.v_div_ratio = fparse(CONFIG_BL0937_V_DIV_RATIO, 0.00100f);
    c.vref_v      = fparse(CONFIG_BL0937_VREF_V, 1.218f);

    c.sample_period_ms = CONFIG_BL0937_SAMPLE_PERIOD_MS;
    c.sel_settle_us    = CONFIG_BL0937_SEL_SETTLE_US;

    c.voltage_calibration = fparse(CONFIG_BL0937_VOLTAGE_CAL, 1.0f);
    c.current_calibration = fparse(CONFIG_BL0937_CURRENT_CAL, 1.0f);
    c.power_calibration   = fparse(CONFIG_BL0937_POWER_CAL,   1.0f);

    c.glitch_filter_ns     = CONFIG_BL0937_GLITCH_FILTER_NS;
    c.force_isr_fallback   = CONFIG_BL0937_ISR_FALLBACK;
    #else
    c.gpio_cf  = GPIO_NUM_4;
    c.gpio_cf1 = GPIO_NUM_5;
    c.gpio_sel = GPIO_NUM_18;
    c.shunt_uohm = 1000;
    c.v_div_ratio = 0.00100f;
    c.vref_v = 1.218f;
    c.sample_period_ms = 1000;
    c.sel_settle_us = 200;
    c.voltage_calibration = 1.0f;
    c.current_calibration = 1.0f;
    c.power_calibration = 1.0f;
    c.glitch_filter_ns = 1000;
    c.force_isr_fallback = false;
    #endif
    return c;
}

static inline float counts_to_hz(uint32_t counts, float window_s)
{
    if (window_s <= 0.0f) return 0.0f;
    return (float)counts / window_s;
}

static void compute_physical(bl0937_measurements_t *m, const bl0937_config_t *cfg)
{
    // Defaults: invalid until we have parameters and non-zero windows.
    m->valid_voltage = false;
    m->valid_current = false;
    m->valid_power   = false;

    const float vref = cfg->vref_v;
    const float vdiv = cfg->v_div_ratio;
    const float rsh  = (cfg->shunt_uohm > 0) ? ((float)cfg->shunt_uohm * 1e-6f) : 0.0f;

    // Voltage:
    if (vref > 0.0f && vdiv > 0.0f && m->cfu_hz > 0.0f) {
        const float v_pin = (vref / BL0937_K_CFU) * m->cfu_hz; // V(V)
        const float v_line = v_pin / vdiv;
        m->voltage_v = v_line * cfg->voltage_calibration;
        m->valid_voltage = true;
    }

    // Current:
    if (vref > 0.0f && rsh > 0.0f && m->cfi_hz > 0.0f) {
        const float v_i = (vref / BL0937_K_CFI) * m->cfi_hz; // V(I)
        const float i_line = v_i / rsh;
        m->current_a = i_line * cfg->current_calibration;
        m->valid_current = true;
    }

    // Active power:
    if (vref > 0.0f && vdiv > 0.0f && rsh > 0.0f && m->cf_hz > 0.0f) {
        // FCF = K * V(V)*V(I) / Vref^2
        // => V(V)*V(I) = FCF * Vref^2 / K
        // P = Vline * Iline = (V(V)/vdiv) * (V(I)/rsh) = (V(V)*V(I)) / (vdiv * rsh)
        const float vv_vi = (m->cf_hz * vref * vref) / BL0937_K_CF;
        const float p = vv_vi / (vdiv * rsh);
        m->power_w = p * cfg->power_calibration;
        m->valid_power = true;
    }
}


// GPIO ISR fallback handlers (also used when PCNT is available but fallback is forced)
static void IRAM_ATTR isr_cf(void *arg) {
    (void)arg;
    s_ctx.isr_cf++;
}

static void IRAM_ATTR isr_cf1(void *arg) {
    (void)arg;
    s_ctx.isr_cf1++;
}

#if !(defined(SOC_PCNT_SUPPORTED) && SOC_PCNT_SUPPORTED)
#if 0
static void IRAM_ATTR isr_cf(void *arg)
{
(void)arg;
s_ctx.isr_cf++;
}

static void IRAM_ATTR isr_cf1(void *arg)
{
(void)arg;
s_ctx.isr_cf1++;
}
#endif
#endif

static esp_err_t gpio_setup_sel(gpio_num_t sel)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << sel,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "gpio_config(sel) failed");
    return gpio_set_level(sel, 1); // start in voltage mode (SEL=1)
}

#if defined(SOC_PCNT_SUPPORTED) && SOC_PCNT_SUPPORTED
static esp_err_t pcnt_setup_unit(pcnt_unit_handle_t *unit, pcnt_channel_handle_t *chan, gpio_num_t gpio_pulse, int glitch_filter_ns)
{
    pcnt_unit_config_t ucfg = {
        .high_limit = 32767,
        .low_limit  = -32768,
    };
    ESP_RETURN_ON_ERROR(pcnt_new_unit(&ucfg, unit), TAG, "pcnt_new_unit failed");

    pcnt_chan_config_t ccfg = {
        .edge_gpio_num = gpio_pulse,
        .level_gpio_num = -1,
    };
    ESP_RETURN_ON_ERROR(pcnt_new_channel(*unit, &ccfg, chan), TAG, "pcnt_new_channel failed");

    ESP_RETURN_ON_ERROR(pcnt_channel_set_edge_action(*chan, PCNT_CHANNEL_EDGE_ACTION_INCREASE, PCNT_CHANNEL_EDGE_ACTION_HOLD),
                        TAG, "pcnt_channel_set_edge_action failed");
    ESP_RETURN_ON_ERROR(pcnt_channel_set_level_action(*chan, PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_KEEP),
                        TAG, "pcnt_channel_set_level_action failed");

    if (glitch_filter_ns > 0) {
        pcnt_glitch_filter_config_t fcfg = {
            .max_glitch_ns = glitch_filter_ns,
        };
        // Not all targets support filters; if it fails, warn and continue.
        esp_err_t err = pcnt_unit_set_glitch_filter(*unit, &fcfg);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "PCNT glitch filter unsupported/failed: %s", esp_err_to_name(err));
        }
    }

    ESP_RETURN_ON_ERROR(pcnt_unit_enable(*unit), TAG, "pcnt_unit_enable failed");
    ESP_RETURN_ON_ERROR(pcnt_unit_clear_count(*unit), TAG, "pcnt_unit_clear_count failed");
    ESP_RETURN_ON_ERROR(pcnt_unit_start(*unit), TAG, "pcnt_unit_start failed");
    return ESP_OK;
}

static __attribute__((unused)) esp_err_t pcnt_read_count(pcnt_unit_handle_t unit, int *out)
{
    int val = 0;
    ESP_RETURN_ON_ERROR(pcnt_unit_get_count(unit, &val), TAG, "pcnt_unit_get_count failed");
    *out = val;
    return ESP_OK;
}

static inline void pcnt_clear_count_safe(pcnt_unit_handle_t unit)
{
    // Best-effort: ignore errors (e.g. unit not started) since we can recover on next sample.
    (void)pcnt_unit_clear_count(unit);
}
#endif

static void timer_cb(void *arg)
{
    (void)arg;

    // Half-period cadence: measure voltage then current
    const int half_ms = s_ctx.cfg.sample_period_ms / 2;
    const float half_s = (float)half_ms / 1000.0f;

    int64_t now_us = esp_timer_get_time();

    // Read counts for the *completed* half-window.
    // NOTE: We clear counters *after* switching SEL and allowing it to settle,
    // so that any unstable pulses during SEL switching do not contaminate the
    // next measurement window.
    uint32_t cf_counts = 0;
    uint32_t cf1_counts = 0;

    #if defined(SOC_PCNT_SUPPORTED) && SOC_PCNT_SUPPORTED
    if (s_ctx.pcnt_active) {
        int cf = 0, cf1 = 0;
        // Read without clearing; clear happens later after SEL settle.
        if (pcnt_unit_get_count(s_ctx.unit_cf, &cf) == ESP_OK) {
            if (cf < 0) cf = 0;
            cf_counts = (uint32_t)cf;
        }
        if (pcnt_unit_get_count(s_ctx.unit_cf1, &cf1) == ESP_OK) {
            if (cf1 < 0) cf1 = 0;
            cf1_counts = (uint32_t)cf1;
        }
    } else
        #endif
        if (s_ctx.isr_active) {
            // Atomically snapshot ISR counters (clear happens later after SEL settle)
            portENTER_CRITICAL(&s_ctx.mux);
            cf_counts = s_ctx.isr_cf;
            cf1_counts = s_ctx.isr_cf1;
            portEXIT_CRITICAL(&s_ctx.mux);
        }

        // We have just completed the window for the *current* s_ctx.phase.
        const bl0937_phase_t prev_phase = s_ctx.phase;
        const bl0937_phase_t next_phase = (prev_phase == PHASE_VOLTAGE) ? PHASE_CURRENT : PHASE_VOLTAGE;

        // Switch SEL to next phase.
        gpio_set_level(s_ctx.cfg.gpio_sel, (next_phase == PHASE_VOLTAGE) ? 1 : 0);

        // Allow SEL to settle (esp_timer callback is NOT a hard ISR).
        if (s_ctx.cfg.sel_settle_us > 0) {
            esp_rom_delay_us((uint32_t)s_ctx.cfg.sel_settle_us);
        }

        // Clear counters AFTER settle so the next window starts clean.
        #if defined(SOC_PCNT_SUPPORTED) && SOC_PCNT_SUPPORTED
        if (s_ctx.pcnt_active) {
            (void)pcnt_unit_clear_count(s_ctx.unit_cf);
            (void)pcnt_unit_clear_count(s_ctx.unit_cf1);
        }
        #endif
        if (s_ctx.isr_active) {
            portENTER_CRITICAL(&s_ctx.mux);
            s_ctx.isr_cf = 0;
            s_ctx.isr_cf1 = 0;
            portEXIT_CRITICAL(&s_ctx.mux);
        }

        // Store captured counts for the completed phase and advance phase state.
        portENTER_CRITICAL(&s_ctx.mux);
        s_ctx.last_cf_count = cf_counts;
        if (prev_phase == PHASE_VOLTAGE) {
            s_ctx.last_cfu_count = cf1_counts;
        } else {
            s_ctx.last_cfi_count = cf1_counts;
        }
        s_ctx.phase = next_phase;
        portEXIT_CRITICAL(&s_ctx.mux);

        // Only publish a full update once per full period (i.e., after we just finished CURRENT phase).
        const bool do_full = (prev_phase == PHASE_CURRENT);
        if (!do_full) return;

        // Build measurement snapshot
        bl0937_measurements_t m = {0};

    uint32_t cf_last, cfu_last, cfi_last;
    portENTER_CRITICAL(&s_ctx.mux);
    cf_last  = s_ctx.last_cf_count;// last half-window CF
    cfu_last = s_ctx.last_cfu_count;
    cfi_last = s_ctx.last_cfi_count;
    portEXIT_CRITICAL(&s_ctx.mux);

    // CF is measured per half window; approximate full-period CF Hz by averaging the two halves.
    // We only stored the last half. For better stability, we treat CF Hz as last_half/half_s.
    // Users can increase sample_period_ms if they need smoothing.
    m.cf_hz  = counts_to_hz(cf_last,  half_s);
    m.cfu_hz = counts_to_hz(cfu_last, half_s);
    m.cfi_hz = counts_to_hz(cfi_last, half_s);

    compute_physical(&m, &s_ctx.cfg);

    // Energy accumulation (Wh) using dt = full period
    const float dt_h = ((float)s_ctx.cfg.sample_period_ms / 1000.0f) / 3600.0f;
    if (m.valid_power) {
        m.energy_wh = 0.0f; // will be overwritten from accumulated value below
        portENTER_CRITICAL(&s_ctx.mux);
        s_ctx.meas.energy_wh += m.power_w * dt_h;
        m.energy_wh = s_ctx.meas.energy_wh;
        portEXIT_CRITICAL(&s_ctx.mux);
    } else {
        portENTER_CRITICAL(&s_ctx.mux);
        m.energy_wh = s_ctx.meas.energy_wh;
        portEXIT_CRITICAL(&s_ctx.mux);
    }

    // Publish snapshot
    portENTER_CRITICAL(&s_ctx.mux);
    // preserve accumulated energy in s_ctx.meas; overwrite the rest
    float e = s_ctx.meas.energy_wh;
    s_ctx.meas = m;
    s_ctx.meas.energy_wh = e;
    portEXIT_CRITICAL(&s_ctx.mux);

    (void)now_us;
}

esp_err_t bl0937_init(const bl0937_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    if (cfg->sample_period_ms < 100) return ESP_ERR_INVALID_ARG;

    if (s_ctx.running) return ESP_ERR_INVALID_STATE;

    s_ctx.cfg = *cfg;
    s_ctx.phase = PHASE_VOLTAGE;
    s_ctx.meas = (bl0937_measurements_t){0};
    s_ctx.meas.energy_wh = 0.0f;

    ESP_RETURN_ON_ERROR(gpio_setup_sel(s_ctx.cfg.gpio_sel), TAG, "SEL gpio setup failed");

    // Configure inputs
    gpio_config_t in = {
        .pin_bit_mask = (1ULL << s_ctx.cfg.gpio_cf) | (1ULL << s_ctx.cfg.gpio_cf1),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&in), TAG, "gpio_config(inputs) failed");

    #if defined(SOC_PCNT_SUPPORTED) && SOC_PCNT_SUPPORTED
    s_ctx.pcnt_active = false;
    if (!s_ctx.cfg.force_isr_fallback) {
        esp_err_t err = pcnt_setup_unit(&s_ctx.unit_cf,  &s_ctx.chan_cf,  s_ctx.cfg.gpio_cf,  s_ctx.cfg.glitch_filter_ns);
        if (err == ESP_OK) {
            err = pcnt_setup_unit(&s_ctx.unit_cf1, &s_ctx.chan_cf1, s_ctx.cfg.gpio_cf1, s_ctx.cfg.glitch_filter_ns);
        }
        if (err == ESP_OK) {
            s_ctx.pcnt_active = true;
            ESP_LOGI(TAG, "Using PCNT pulse counting");
        } else {
            ESP_LOGW(TAG, "PCNT setup failed (%s), falling back to GPIO ISR", esp_err_to_name(err));
            // Clean up partial PCNT allocs
            if (s_ctx.unit_cf1) { pcnt_del_channel(s_ctx.chan_cf1); pcnt_del_unit(s_ctx.unit_cf1); s_ctx.unit_cf1 = NULL; }
            if (s_ctx.unit_cf)  { pcnt_del_channel(s_ctx.chan_cf);  pcnt_del_unit(s_ctx.unit_cf);  s_ctx.unit_cf  = NULL; }
        }
    }
    #endif

    s_ctx.isr_active = false;

    #if !(defined(SOC_PCNT_SUPPORTED) && SOC_PCNT_SUPPORTED)
    // targets without PCNT => always ISR
    s_ctx.cfg.force_isr_fallback = true;
    #endif

    #if defined(SOC_PCNT_SUPPORTED) && SOC_PCNT_SUPPORTED
    if (!s_ctx.pcnt_active)
        #endif
    {
        // ISR fallback
        // This may already be installed by the application. Treat "already installed" as success.
        esp_err_t isrsvc = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
        if (isrsvc != ESP_OK && isrsvc != ESP_ERR_INVALID_STATE) {
            ESP_RETURN_ON_ERROR(isrsvc, TAG, "gpio_install_isr_service failed");
        }
        gpio_set_intr_type(s_ctx.cfg.gpio_cf, GPIO_INTR_POSEDGE);
        gpio_set_intr_type(s_ctx.cfg.gpio_cf1, GPIO_INTR_POSEDGE);
        ESP_RETURN_ON_ERROR(gpio_isr_handler_add(s_ctx.cfg.gpio_cf,  isr_cf,  NULL), TAG, "isr add CF failed");
        ESP_RETURN_ON_ERROR(gpio_isr_handler_add(s_ctx.cfg.gpio_cf1, isr_cf1, NULL), TAG, "isr add CF1 failed");
        portENTER_CRITICAL(&s_ctx.mux);
        s_ctx.isr_cf = 0;
        s_ctx.isr_cf1 = 0;
        portEXIT_CRITICAL(&s_ctx.mux);
        s_ctx.isr_active = true;
        ESP_LOGI(TAG, "Using GPIO ISR pulse counting");
    }

    // Create periodic timer at half-period
    const int half_ms = s_ctx.cfg.sample_period_ms / 2;
    if (half_ms <= 0) return ESP_ERR_INVALID_ARG;

    esp_timer_create_args_t tcfg = {
        .callback = timer_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "bl0937",
        .skip_unhandled_events = true,
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&tcfg, &s_ctx.timer), TAG, "esp_timer_create failed");

    s_ctx.initialized = true;
    return ESP_OK;
}

esp_err_t bl0937_start(void)
{
    if (!s_ctx.initialized) return ESP_ERR_INVALID_STATE;
    if (s_ctx.running) return ESP_OK;

    // Ensure SEL starts at voltage phase
    s_ctx.phase = PHASE_VOLTAGE;
    gpio_set_level(s_ctx.cfg.gpio_sel, 1);

    #if defined(SOC_PCNT_SUPPORTED) && SOC_PCNT_SUPPORTED
    if (s_ctx.pcnt_active) {
        pcnt_unit_clear_count(s_ctx.unit_cf);
        pcnt_unit_clear_count(s_ctx.unit_cf1);
    }
    #endif
    if (s_ctx.isr_active) {
        portENTER_CRITICAL(&s_ctx.mux);
        s_ctx.isr_cf = 0;
        s_ctx.isr_cf1 = 0;
        portEXIT_CRITICAL(&s_ctx.mux);
    }

    const int half_ms = s_ctx.cfg.sample_period_ms / 2;
    ESP_RETURN_ON_ERROR(esp_timer_start_periodic(s_ctx.timer, (uint64_t)half_ms * 1000ULL), TAG, "timer start failed");
    s_ctx.running = true;
    return ESP_OK;
}

esp_err_t bl0937_stop(void)
{
    if (!s_ctx.running) return ESP_OK;
    esp_err_t err = esp_timer_stop(s_ctx.timer);
    if (err != ESP_OK) return err;
    s_ctx.running = false;
    return ESP_OK;
}

esp_err_t bl0937_deinit(void)
{
    if (!s_ctx.initialized) return ESP_OK;
    if (s_ctx.running) {
        bl0937_stop();
    }

    if (s_ctx.timer) {
        esp_timer_delete(s_ctx.timer);
        s_ctx.timer = NULL;
    }

    #if defined(SOC_PCNT_SUPPORTED) && SOC_PCNT_SUPPORTED/**
    Copyright 2026 Achim Pieters | StudioPieters®

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"), to deal
    in the Software without restriction, including without limitation the rights
    to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
    copies of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in all
    copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NON INFRINGEMENT. IN NO EVENT SHALL THE
    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
    WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
    CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

    for more information visit https://www.studiopieters.nl
        **/

        #include "bl0937.h"
        #include "sdkconfig.h"
        #ifndef CONFIG_BL0937_ISR_FALLBACK
        #define CONFIG_BL0937_ISR_FALLBACK 0
        #endif

        #include <string.h>
        #include <stdlib.h>

        #include "esp_log.h"
        #include "esp_timer.h"
        #include "esp_check.h"
        #include "esp_rom_sys.h"
        #include "freertos/FreeRTOS.h"
        #include "freertos/portmacro.h"
        #include "driver/gpio.h"

        #if __has_include("soc/soc_caps.h")
        #include "soc/soc_caps.h"
        #endif

        #if defined(SOC_PCNT_SUPPORTED) && SOC_PCNT_SUPPORTED
        #include "driver/pulse_cnt.h"
        #endif

        // Datasheet constants (BL0937 V1.02):
        // FCF  = 1721506 * (V(V) * V(I)) / Vref^2
        // FCFU = 15397   * V(V) / Vref
        // FCFI = 94638   * V(I) / Vref
        #define BL0937_K_CF   (1721506.0f)
        #define BL0937_K_CFU  (15397.0f)
        #define BL0937_K_CFI  (94638.0f)

        static const char *TAG = "bl0937";

        typedef enum {
            PHASE_VOLTAGE = 0,
            PHASE_CURRENT = 1,
        } bl0937_phase_t;

        typedef struct {
            bl0937_config_t cfg;
            bool initialized;
            bool running;

            esp_timer_handle_t timer;

            // phase state machine
            bl0937_phase_t phase;
            int64_t phase_start_us;
            int64_t last_full_update_us;

            // counters for current half-phase
            uint32_t cf_count_accum;
            uint32_t cf_count_prev;
            uint32_t cf1_count_prev;

            // last half-phase captured counts
            uint32_t last_cf_count; // counts during half-phase
            uint32_t last_cfu_count; // counts during voltage half-phase
            uint32_t last_cfi_count; // counts during current half-phase

            // computed measurement snapshot
            bl0937_measurements_t meas;

            portMUX_TYPE mux;

            #if defined(SOC_PCNT_SUPPORTED) && SOC_PCNT_SUPPORTED
            // PCNT resources
            pcnt_unit_handle_t unit_cf;
            pcnt_channel_handle_t chan_cf;
            pcnt_unit_handle_t unit_cf1;
            pcnt_channel_handle_t chan_cf1;
            bool pcnt_active;
            #endif

            // ISR fallback counters
            volatile uint32_t isr_cf;
            volatile uint32_t isr_cf1;
            bool isr_active;
        } bl0937_ctx_t;

        static bl0937_ctx_t s_ctx = {
            .mux = portMUX_INITIALIZER_UNLOCKED,
        };

        static float fparse(const char *s, float dflt)
        {
            if (!s) return dflt;
            char *end = NULL;
            float v = strtof(s, &end);
            if (end == s) return dflt;
            return v;
        }

        bl0937_config_t bl0937_config_default(void)
        {
            bl0937_config_t c = {0};
            #if CONFIG_BL0937_ENABLE
            c.gpio_cf  = (gpio_num_t)CONFIG_ESP_BL0937_CF_GPIO;
            c.gpio_cf1 = (gpio_num_t)CONFIG_ESP_BL0937_CF1_GPIO;
            c.gpio_sel = (gpio_num_t)CONFIG_ESP_BL0937_SEL_GPIO;

            c.shunt_uohm  = CONFIG_BL0937_SHUNT_UOHM;
            c.v_div_ratio = fparse(CONFIG_BL0937_V_DIV_RATIO, 0.00100f);
            c.vref_v      = fparse(CONFIG_BL0937_VREF_V, 1.218f);

            c.sample_period_ms = CONFIG_BL0937_SAMPLE_PERIOD_MS;
            c.sel_settle_us    = CONFIG_BL0937_SEL_SETTLE_US;

            c.voltage_calibration = fparse(CONFIG_BL0937_VOLTAGE_CAL, 1.0f);
            c.current_calibration = fparse(CONFIG_BL0937_CURRENT_CAL, 1.0f);
            c.power_calibration   = fparse(CONFIG_BL0937_POWER_CAL,   1.0f);

            c.glitch_filter_ns     = CONFIG_BL0937_GLITCH_FILTER_NS;
            c.force_isr_fallback   = CONFIG_BL0937_ISR_FALLBACK;
            #else
            c.gpio_cf  = GPIO_NUM_4;
            c.gpio_cf1 = GPIO_NUM_5;
            c.gpio_sel = GPIO_NUM_18;
            c.shunt_uohm = 1000;
            c.v_div_ratio = 0.00100f;
            c.vref_v = 1.218f;
            c.sample_period_ms = 1000;
            c.sel_settle_us = 200;
            c.voltage_calibration = 1.0f;
            c.current_calibration = 1.0f;
            c.power_calibration = 1.0f;
            c.glitch_filter_ns = 1000;
            c.force_isr_fallback = false;
            #endif
            return c;
        }

        static inline float counts_to_hz(uint32_t counts, float window_s)
        {
            if (window_s <= 0.0f) return 0.0f;
            return (float)counts / window_s;
        }

        static void compute_physical(bl0937_measurements_t *m, const bl0937_config_t *cfg)
        {
            // Defaults: invalid until we have parameters and non-zero windows.
            m->valid_voltage = false;
            m->valid_current = false;
            m->valid_power   = false;

            const float vref = cfg->vref_v;
            const float vdiv = cfg->v_div_ratio;
            const float rsh  = (cfg->shunt_uohm > 0) ? ((float)cfg->shunt_uohm * 1e-6f) : 0.0f;

            // Voltage:
            if (vref > 0.0f && vdiv > 0.0f && m->cfu_hz > 0.0f) {
                const float v_pin = (vref / BL0937_K_CFU) * m->cfu_hz; // V(V)
                const float v_line = v_pin / vdiv;
                m->voltage_v = v_line * cfg->voltage_calibration;
                m->valid_voltage = true;
            }

            // Current:
            if (vref > 0.0f && rsh > 0.0f && m->cfi_hz > 0.0f) {
                const float v_i = (vref / BL0937_K_CFI) * m->cfi_hz; // V(I)
                const float i_line = v_i / rsh;
                m->current_a = i_line * cfg->current_calibration;
                m->valid_current = true;
            }

            // Active power:
            if (vref > 0.0f && vdiv > 0.0f && rsh > 0.0f && m->cf_hz > 0.0f) {
                // FCF = K * V(V)*V(I) / Vref^2
                // => V(V)*V(I) = FCF * Vref^2 / K
                // P = Vline * Iline = (V(V)/vdiv) * (V(I)/rsh) = (V(V)*V(I)) / (vdiv * rsh)
                const float vv_vi = (m->cf_hz * vref * vref) / BL0937_K_CF;
                const float p = vv_vi / (vdiv * rsh);
                m->power_w = p * cfg->power_calibration;
                m->valid_power = true;
            }
        }


        // GPIO ISR fallback handlers (also used when PCNT is available but fallback is forced)
        static void IRAM_ATTR isr_cf(void *arg) {
            (void)arg;
            s_ctx.isr_cf++;
        }

        static void IRAM_ATTR isr_cf1(void *arg) {
            (void)arg;
            s_ctx.isr_cf1++;
        }

        #if !(defined(SOC_PCNT_SUPPORTED) && SOC_PCNT_SUPPORTED)
        #if 0
        static void IRAM_ATTR isr_cf(void *arg)
        {
        (void)arg;
        s_ctx.isr_cf++;
}

static void IRAM_ATTR isr_cf1(void *arg)
{
(void)arg;
s_ctx.isr_cf1++;
}
#endif
#endif

static esp_err_t gpio_setup_sel(gpio_num_t sel)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << sel,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "gpio_config(sel) failed");
    return gpio_set_level(sel, 1); // start in voltage mode (SEL=1)
}

#if defined(SOC_PCNT_SUPPORTED) && SOC_PCNT_SUPPORTED
static esp_err_t pcnt_setup_unit(pcnt_unit_handle_t *unit, pcnt_channel_handle_t *chan, gpio_num_t gpio_pulse, int glitch_filter_ns)
{
    pcnt_unit_config_t ucfg = {
        .high_limit = 32767,
        .low_limit  = -32768,
    };
    ESP_RETURN_ON_ERROR(pcnt_new_unit(&ucfg, unit), TAG, "pcnt_new_unit failed");

    pcnt_chan_config_t ccfg = {
        .edge_gpio_num = gpio_pulse,
        .level_gpio_num = -1,
    };
    ESP_RETURN_ON_ERROR(pcnt_new_channel(*unit, &ccfg, chan), TAG, "pcnt_new_channel failed");

    ESP_RETURN_ON_ERROR(pcnt_channel_set_edge_action(*chan, PCNT_CHANNEL_EDGE_ACTION_INCREASE, PCNT_CHANNEL_EDGE_ACTION_HOLD),
                        TAG, "pcnt_channel_set_edge_action failed");
    ESP_RETURN_ON_ERROR(pcnt_channel_set_level_action(*chan, PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_KEEP),
                        TAG, "pcnt_channel_set_level_action failed");

    if (glitch_filter_ns > 0) {
        pcnt_glitch_filter_config_t fcfg = {
            .max_glitch_ns = glitch_filter_ns,
        };
        // Not all targets support filters; if it fails, warn and continue.
        esp_err_t err = pcnt_unit_set_glitch_filter(*unit, &fcfg);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "PCNT glitch filter unsupported/failed: %s", esp_err_to_name(err));
        }
    }

    ESP_RETURN_ON_ERROR(pcnt_unit_enable(*unit), TAG, "pcnt_unit_enable failed");
    ESP_RETURN_ON_ERROR(pcnt_unit_clear_count(*unit), TAG, "pcnt_unit_clear_count failed");
    ESP_RETURN_ON_ERROR(pcnt_unit_start(*unit), TAG, "pcnt_unit_start failed");
    return ESP_OK;
}

static __attribute__((unused)) esp_err_t pcnt_read_count(pcnt_unit_handle_t unit, int *out)
{
    int val = 0;
    ESP_RETURN_ON_ERROR(pcnt_unit_get_count(unit, &val), TAG, "pcnt_unit_get_count failed");
    *out = val;
    return ESP_OK;
}

static inline void pcnt_clear_count_safe(pcnt_unit_handle_t unit)
{
    // Best-effort: ignore errors (e.g. unit not started) since we can recover on next sample.
    (void)pcnt_unit_clear_count(unit);
}
#endif

static void timer_cb(void *arg)
{
    (void)arg;

    // Half-period cadence: measure voltage then current
    const int half_ms = s_ctx.cfg.sample_period_ms / 2;
    const float half_s = (float)half_ms / 1000.0f;

    int64_t now_us = esp_timer_get_time();

    // Read counts for the *completed* half-window.
    // NOTE: We clear counters *after* switching SEL and allowing it to settle,
    // so that any unstable pulses during SEL switching do not contaminate the
    // next measurement window.
    uint32_t cf_counts = 0;
    uint32_t cf1_counts = 0;

    #if defined(SOC_PCNT_SUPPORTED) && SOC_PCNT_SUPPORTED
    if (s_ctx.pcnt_active) {
        int cf = 0, cf1 = 0;
        // Read without clearing; clear happens later after SEL settle.
        if (pcnt_unit_get_count(s_ctx.unit_cf, &cf) == ESP_OK) {
            if (cf < 0) cf = 0;
            cf_counts = (uint32_t)cf;
        }
        if (pcnt_unit_get_count(s_ctx.unit_cf1, &cf1) == ESP_OK) {
            if (cf1 < 0) cf1 = 0;
            cf1_counts = (uint32_t)cf1;
        }
    } else
        #endif
        if (s_ctx.isr_active) {
            // Atomically snapshot ISR counters (clear happens later after SEL settle)
            portENTER_CRITICAL(&s_ctx.mux);
            cf_counts = s_ctx.isr_cf;
            cf1_counts = s_ctx.isr_cf1;
            portEXIT_CRITICAL(&s_ctx.mux);
        }

        // We have just completed the window for the *current* s_ctx.phase.
        const bl0937_phase_t prev_phase = s_ctx.phase;
        const bl0937_phase_t next_phase = (prev_phase == PHASE_VOLTAGE) ? PHASE_CURRENT : PHASE_VOLTAGE;

        // Switch SEL to next phase.
        gpio_set_level(s_ctx.cfg.gpio_sel, (next_phase == PHASE_VOLTAGE) ? 1 : 0);

        // Allow SEL to settle (esp_timer callback is NOT a hard ISR).
        if (s_ctx.cfg.sel_settle_us > 0) {
            esp_rom_delay_us((uint32_t)s_ctx.cfg.sel_settle_us);
        }

        // Clear counters AFTER settle so the next window starts clean.
        #if defined(SOC_PCNT_SUPPORTED) && SOC_PCNT_SUPPORTED
        if (s_ctx.pcnt_active) {
            (void)pcnt_unit_clear_count(s_ctx.unit_cf);
            (void)pcnt_unit_clear_count(s_ctx.unit_cf1);
        }
        #endif
        if (s_ctx.isr_active) {
            portENTER_CRITICAL(&s_ctx.mux);
            s_ctx.isr_cf = 0;
            s_ctx.isr_cf1 = 0;
            portEXIT_CRITICAL(&s_ctx.mux);
        }

        // Store captured counts for the completed phase and advance phase state.
        portENTER_CRITICAL(&s_ctx.mux);
        s_ctx.last_cf_count = cf_counts;
        if (prev_phase == PHASE_VOLTAGE) {
            s_ctx.last_cfu_count = cf1_counts;
        } else {
            s_ctx.last_cfi_count = cf1_counts;
        }
        s_ctx.phase = next_phase;
        portEXIT_CRITICAL(&s_ctx.mux);

        // Only publish a full update once per full period (i.e., after we just finished CURRENT phase).
        const bool do_full = (prev_phase == PHASE_CURRENT);
        if (!do_full) return;

        // Build measurement snapshot
        bl0937_measurements_t m = {0};

    uint32_t cf_last, cfu_last, cfi_last;
    portENTER_CRITICAL(&s_ctx.mux);
    cf_last  = s_ctx.last_cf_count;// last half-window CF
    cfu_last = s_ctx.last_cfu_count;
    cfi_last = s_ctx.last_cfi_count;
    portEXIT_CRITICAL(&s_ctx.mux);

    // CF is measured per half window; approximate full-period CF Hz by averaging the two halves.
    // We only stored the last half. For better stability, we treat CF Hz as last_half/half_s.
    // Users can increase sample_period_ms if they need smoothing.
    m.cf_hz  = counts_to_hz(cf_last,  half_s);
    m.cfu_hz = counts_to_hz(cfu_last, half_s);
    m.cfi_hz = counts_to_hz(cfi_last, half_s);

    compute_physical(&m, &s_ctx.cfg);

    // Energy accumulation (Wh) using dt = full period
    const float dt_h = ((float)s_ctx.cfg.sample_period_ms / 1000.0f) / 3600.0f;
    if (m.valid_power) {
        m.energy_wh = 0.0f; // will be overwritten from accumulated value below
        portENTER_CRITICAL(&s_ctx.mux);
        s_ctx.meas.energy_wh += m.power_w * dt_h;
        m.energy_wh = s_ctx.meas.energy_wh;
        portEXIT_CRITICAL(&s_ctx.mux);
    } else {
        portENTER_CRITICAL(&s_ctx.mux);
        m.energy_wh = s_ctx.meas.energy_wh;
        portEXIT_CRITICAL(&s_ctx.mux);
    }

    // Publish snapshot
    portENTER_CRITICAL(&s_ctx.mux);
    // preserve accumulated energy in s_ctx.meas; overwrite the rest
    float e = s_ctx.meas.energy_wh;
    s_ctx.meas = m;
    s_ctx.meas.energy_wh = e;
    portEXIT_CRITICAL(&s_ctx.mux);

    (void)now_us;
}

esp_err_t bl0937_init(const bl0937_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    if (cfg->sample_period_ms < 100) return ESP_ERR_INVALID_ARG;

    if (s_ctx.running) return ESP_ERR_INVALID_STATE;

    s_ctx.cfg = *cfg;
    s_ctx.phase = PHASE_VOLTAGE;
    s_ctx.meas = (bl0937_measurements_t){0};
    s_ctx.meas.energy_wh = 0.0f;

    ESP_RETURN_ON_ERROR(gpio_setup_sel(s_ctx.cfg.gpio_sel), TAG, "SEL gpio setup failed");

    // Configure inputs
    gpio_config_t in = {
        .pin_bit_mask = (1ULL << s_ctx.cfg.gpio_cf) | (1ULL << s_ctx.cfg.gpio_cf1),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&in), TAG, "gpio_config(inputs) failed");

    #if defined(SOC_PCNT_SUPPORTED) && SOC_PCNT_SUPPORTED
    s_ctx.pcnt_active = false;
    if (!s_ctx.cfg.force_isr_fallback) {
        esp_err_t err = pcnt_setup_unit(&s_ctx.unit_cf,  &s_ctx.chan_cf,  s_ctx.cfg.gpio_cf,  s_ctx.cfg.glitch_filter_ns);
        if (err == ESP_OK) {
            err = pcnt_setup_unit(&s_ctx.unit_cf1, &s_ctx.chan_cf1, s_ctx.cfg.gpio_cf1, s_ctx.cfg.glitch_filter_ns);
        }
        if (err == ESP_OK) {
            s_ctx.pcnt_active = true;
            ESP_LOGI(TAG, "Using PCNT pulse counting");
        } else {
            ESP_LOGW(TAG, "PCNT setup failed (%s), falling back to GPIO ISR", esp_err_to_name(err));
            // Clean up partial PCNT allocs
            if (s_ctx.unit_cf1) { pcnt_del_channel(s_ctx.chan_cf1); pcnt_del_unit(s_ctx.unit_cf1); s_ctx.unit_cf1 = NULL; }
            if (s_ctx.unit_cf)  { pcnt_del_channel(s_ctx.chan_cf);  pcnt_del_unit(s_ctx.unit_cf);  s_ctx.unit_cf  = NULL; }
        }
    }
    #endif

    s_ctx.isr_active = false;

    #if !(defined(SOC_PCNT_SUPPORTED) && SOC_PCNT_SUPPORTED)
    // targets without PCNT => always ISR
    s_ctx.cfg.force_isr_fallback = true;
    #endif

    #if defined(SOC_PCNT_SUPPORTED) && SOC_PCNT_SUPPORTED
    if (!s_ctx.pcnt_active)
        #endif
    {
        // ISR fallback
        // This may already be installed by the application. Treat "already installed" as success.
        esp_err_t isrsvc = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
        if (isrsvc != ESP_OK && isrsvc != ESP_ERR_INVALID_STATE) {
            ESP_RETURN_ON_ERROR(isrsvc, TAG, "gpio_install_isr_service failed");
        }
        gpio_set_intr_type(s_ctx.cfg.gpio_cf, GPIO_INTR_POSEDGE);
        gpio_set_intr_type(s_ctx.cfg.gpio_cf1, GPIO_INTR_POSEDGE);
        ESP_RETURN_ON_ERROR(gpio_isr_handler_add(s_ctx.cfg.gpio_cf,  isr_cf,  NULL), TAG, "isr add CF failed");/**
        Copyright 2026 Achim Pieters | StudioPieters®

        Permission is hereby granted, free of charge, to any person obtaining a copy
        of this software and associated documentation files (the "Software"), to deal
        in the Software without restriction, including without limitation the rights
        to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
        copies of the Software, and to permit persons to whom the Software is
        furnished to do so, subject to the following conditions:

        The above copyright notice and this permission notice shall be included in all
        copies or substantial portions of the Software.

        THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
        IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
        FITNESS FOR A PARTICULAR PURPOSE AND NON INFRINGEMENT. IN NO EVENT SHALL THE
        AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
        WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
        CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

        for more information visit https://www.studiopieters.nl
            **/

            #include "bl0937.h"
            #include "sdkconfig.h"
            #ifndef CONFIG_BL0937_ISR_FALLBACK
            #define CONFIG_BL0937_ISR_FALLBACK 0
            #endif

            #include <string.h>
            #include <stdlib.h>

            #include "esp_log.h"
            #include "esp_timer.h"
            #include "esp_check.h"
            #include "esp_rom_sys.h"
            #include "freertos/FreeRTOS.h"
            #include "freertos/portmacro.h"
            #include "driver/gpio.h"

            #if __has_include("soc/soc_caps.h")
            #include "soc/soc_caps.h"
            #endif

            #if defined(SOC_PCNT_SUPPORTED) && SOC_PCNT_SUPPORTED
            #include "driver/pulse_cnt.h"
            #endif

            // Datasheet constants (BL0937 V1.02):
            // FCF  = 1721506 * (V(V) * V(I)) / Vref^2
            // FCFU = 15397   * V(V) / Vref
            // FCFI = 94638   * V(I) / Vref
            #define BL0937_K_CF   (1721506.0f)
            #define BL0937_K_CFU  (15397.0f)
            #define BL0937_K_CFI  (94638.0f)

            static const char *TAG = "bl0937";

            typedef enum {
                PHASE_VOLTAGE = 0,
                PHASE_CURRENT = 1,
            } bl0937_phase_t;

            typedef struct {
                bl0937_config_t cfg;
                bool initialized;
                bool running;

                esp_timer_handle_t timer;

                // phase state machine
                bl0937_phase_t phase;
                int64_t phase_start_us;
                int64_t last_full_update_us;

                // counters for current half-phase
                uint32_t cf_count_accum;
                uint32_t cf_count_prev;
                uint32_t cf1_count_prev;

                // last half-phase captured counts
                uint32_t last_cf_count; // counts during half-phase
                uint32_t last_cfu_count; // counts during voltage half-phase
                uint32_t last_cfi_count; // counts during current half-phase

                // computed measurement snapshot
                bl0937_measurements_t meas;

                portMUX_TYPE mux;

                #if defined(SOC_PCNT_SUPPORTED) && SOC_PCNT_SUPPORTED
                // PCNT resources
                pcnt_unit_handle_t unit_cf;
                pcnt_channel_handle_t chan_cf;
                pcnt_unit_handle_t unit_cf1;
                pcnt_channel_handle_t chan_cf1;
                bool pcnt_active;
                #endif

                // ISR fallback counters
                volatile uint32_t isr_cf;
                volatile uint32_t isr_cf1;
                bool isr_active;
            } bl0937_ctx_t;

            static bl0937_ctx_t s_ctx = {
                .mux = portMUX_INITIALIZER_UNLOCKED,
            };

            static float fparse(const char *s, float dflt)
            {
                if (!s) return dflt;
                char *end = NULL;
                float v = strtof(s, &end);
                if (end == s) return dflt;
                return v;
            }

            bl0937_config_t bl0937_config_default(void)
            {
                bl0937_config_t c = {0};
                #if CONFIG_BL0937_ENABLE
                c.gpio_cf  = (gpio_num_t)CONFIG_ESP_BL0937_CF_GPIO;
                c.gpio_cf1 = (gpio_num_t)CONFIG_ESP_BL0937_CF1_GPIO;
                c.gpio_sel = (gpio_num_t)CONFIG_ESP_BL0937_SEL_GPIO;

                c.shunt_uohm  = CONFIG_BL0937_SHUNT_UOHM;
                c.v_div_ratio = fparse(CONFIG_BL0937_V_DIV_RATIO, 0.00100f);
                c.vref_v      = fparse(CONFIG_BL0937_VREF_V, 1.218f);

                c.sample_period_ms = CONFIG_BL0937_SAMPLE_PERIOD_MS;
                c.sel_settle_us    = CONFIG_BL0937_SEL_SETTLE_US;

                c.voltage_calibration = fparse(CONFIG_BL0937_VOLTAGE_CAL, 1.0f);
                c.current_calibration = fparse(CONFIG_BL0937_CURRENT_CAL, 1.0f);
                c.power_calibration   = fparse(CONFIG_BL0937_POWER_CAL,   1.0f);

                c.glitch_filter_ns     = CONFIG_BL0937_GLITCH_FILTER_NS;
                c.force_isr_fallback   = CONFIG_BL0937_ISR_FALLBACK;
                #else
                c.gpio_cf  = GPIO_NUM_4;
                c.gpio_cf1 = GPIO_NUM_5;
                c.gpio_sel = GPIO_NUM_18;
                c.shunt_uohm = 1000;
                c.v_div_ratio = 0.00100f;
                c.vref_v = 1.218f;
                c.sample_period_ms = 1000;
                c.sel_settle_us = 200;
                c.voltage_calibration = 1.0f;
                c.current_calibration = 1.0f;
                c.power_calibration = 1.0f;
                c.glitch_filter_ns = 1000;
                c.force_isr_fallback = false;
                #endif
                return c;
            }

            static inline float counts_to_hz(uint32_t counts, float window_s)
            {
                if (window_s <= 0.0f) return 0.0f;
                return (float)counts / window_s;
            }

            static void compute_physical(bl0937_measurements_t *m, const bl0937_config_t *cfg)
            {
                // Defaults: invalid until we have parameters and non-zero windows.
                m->valid_voltage = false;
                m->valid_current = false;
                m->valid_power   = false;

                const float vref = cfg->vref_v;
                const float vdiv = cfg->v_div_ratio;
                const float rsh  = (cfg->shunt_uohm > 0) ? ((float)cfg->shunt_uohm * 1e-6f) : 0.0f;

                // Voltage:
                if (vref > 0.0f && vdiv > 0.0f && m->cfu_hz > 0.0f) {
                    const float v_pin = (vref / BL0937_K_CFU) * m->cfu_hz; // V(V)
                    const float v_line = v_pin / vdiv;
                    m->voltage_v = v_line * cfg->voltage_calibration;
                    m->valid_voltage = true;
                }

                // Current:
                if (vref > 0.0f && rsh > 0.0f && m->cfi_hz > 0.0f) {
                    const float v_i = (vref / BL0937_K_CFI) * m->cfi_hz; // V(I)
                    const float i_line = v_i / rsh;
                    m->current_a = i_line * cfg->current_calibration;
                    m->valid_current = true;
                }

                // Active power:
                if (vref > 0.0f && vdiv > 0.0f && rsh > 0.0f && m->cf_hz > 0.0f) {
                    // FCF = K * V(V)*V(I) / Vref^2
                    // => V(V)*V(I) = FCF * Vref^2 / K
                    // P = Vline * Iline = (V(V)/vdiv) * (V(I)/rsh) = (V(V)*V(I)) / (vdiv * rsh)
                    const float vv_vi = (m->cf_hz * vref * vref) / BL0937_K_CF;
                    const float p = vv_vi / (vdiv * rsh);
                    m->power_w = p * cfg->power_calibration;
                    m->valid_power = true;
                }
            }


            // GPIO ISR fallback handlers (also used when PCNT is available but fallback is forced)
            static void IRAM_ATTR isr_cf(void *arg) {
                (void)arg;
                s_ctx.isr_cf++;
            }

            static void IRAM_ATTR isr_cf1(void *arg) {
                (void)arg;
                s_ctx.isr_cf1++;
            }

            #if !(defined(SOC_PCNT_SUPPORTED) && SOC_PCNT_SUPPORTED)
            #if 0
            static void IRAM_ATTR isr_cf(void *arg)
            {
            (void)arg;
            s_ctx.isr_cf++;
    }

    static void IRAM_ATTR isr_cf1(void *arg)
    {
    (void)arg;
    s_ctx.isr_cf1++;
    }
    #endif
    #endif

    static esp_err_t gpio_setup_sel(gpio_num_t sel)
    {
        gpio_config_t io = {
            .pin_bit_mask = 1ULL << sel,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "gpio_config(sel) failed");
        return gpio_set_level(sel, 1); // start in voltage mode (SEL=1)
    }

    #if defined(SOC_PCNT_SUPPORTED) && SOC_PCNT_SUPPORTED
    static esp_err_t pcnt_setup_unit(pcnt_unit_handle_t *unit, pcnt_channel_handle_t *chan, gpio_num_t gpio_pulse, int glitch_filter_ns)
    {
        pcnt_unit_config_t ucfg = {
            .high_limit = 32767,
            .low_limit  = -32768,
        };
        ESP_RETURN_ON_ERROR(pcnt_new_unit(&ucfg, unit), TAG, "pcnt_new_unit failed");

        pcnt_chan_config_t ccfg = {
            .edge_gpio_num = gpio_pulse,
            .level_gpio_num = -1,
        };
        ESP_RETURN_ON_ERROR(pcnt_new_channel(*unit, &ccfg, chan), TAG, "pcnt_new_channel failed");

        ESP_RETURN_ON_ERROR(pcnt_channel_set_edge_action(*chan, PCNT_CHANNEL_EDGE_ACTION_INCREASE, PCNT_CHANNEL_EDGE_ACTION_HOLD),
                            TAG, "pcnt_channel_set_edge_action failed");
        ESP_RETURN_ON_ERROR(pcnt_channel_set_level_action(*chan, PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_KEEP),
                            TAG, "pcnt_channel_set_level_action failed");

        if (glitch_filter_ns > 0) {
            pcnt_glitch_filter_config_t fcfg = {
                .max_glitch_ns = glitch_filter_ns,
            };
            // Not all targets support filters; if it fails, warn and continue.
            esp_err_t err = pcnt_unit_set_glitch_filter(*unit, &fcfg);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "PCNT glitch filter unsupported/failed: %s", esp_err_to_name(err));
            }
        }

        ESP_RETURN_ON_ERROR(pcnt_unit_enable(*unit), TAG, "pcnt_unit_enable failed");
        ESP_RETURN_ON_ERROR(pcnt_unit_clear_count(*unit), TAG, "pcnt_unit_clear_count failed");
        ESP_RETURN_ON_ERROR(pcnt_unit_start(*unit), TAG, "pcnt_unit_start failed");
        return ESP_OK;
    }

    static __attribute__((unused)) esp_err_t pcnt_read_count(pcnt_unit_handle_t unit, int *out)
    {
        int val = 0;
        ESP_RETURN_ON_ERROR(pcnt_unit_get_count(unit, &val), TAG, "pcnt_unit_get_count failed");
        *out = val;
        return ESP_OK;
    }

    static inline void pcnt_clear_count_safe(pcnt_unit_handle_t unit)
    {
        // Best-effort: ignore errors (e.g. unit not started) since we can recover on next sample.
        (void)pcnt_unit_clear_count(unit);
    }
    #endif

    static void timer_cb(void *arg)
    {
        (void)arg;

        // Half-period cadence: measure voltage then current
        const int half_ms = s_ctx.cfg.sample_period_ms / 2;
        const float half_s = (float)half_ms / 1000.0f;

        int64_t now_us = esp_timer_get_time();

        // Read counts for the *completed* half-window.
        // NOTE: We clear counters *after* switching SEL and allowing it to settle,
        // so that any unstable pulses during SEL switching do not contaminate the
        // next measurement window.
        uint32_t cf_counts = 0;
        uint32_t cf1_counts = 0;

        #if defined(SOC_PCNT_SUPPORTED) && SOC_PCNT_SUPPORTED
        if (s_ctx.pcnt_active) {
            int cf = 0, cf1 = 0;
            // Read without clearing; clear happens later after SEL settle.
            if (pcnt_unit_get_count(s_ctx.unit_cf, &cf) == ESP_OK) {
                if (cf < 0) cf = 0;
                cf_counts = (uint32_t)cf;
            }
            if (pcnt_unit_get_count(s_ctx.unit_cf1, &cf1) == ESP_OK) {
                if (cf1 < 0) cf1 = 0;
                cf1_counts = (uint32_t)cf1;
            }
        } else
            #endif
            if (s_ctx.isr_active) {
                // Atomically snapshot ISR counters (clear happens later after SEL settle)
                portENTER_CRITICAL(&s_ctx.mux);
                cf_counts = s_ctx.isr_cf;
                cf1_counts = s_ctx.isr_cf1;
                portEXIT_CRITICAL(&s_ctx.mux);
            }

            // We have just completed the window for the *current* s_ctx.phase.
            const bl0937_phase_t prev_phase = s_ctx.phase;
            const bl0937_phase_t next_phase = (prev_phase == PHASE_VOLTAGE) ? PHASE_CURRENT : PHASE_VOLTAGE;

            // Switch SEL to next phase.
            gpio_set_level(s_ctx.cfg.gpio_sel, (next_phase == PHASE_VOLTAGE) ? 1 : 0);

            // Allow SEL to settle (esp_timer callback is NOT a hard ISR).
            if (s_ctx.cfg.sel_settle_us > 0) {
                esp_rom_delay_us((uint32_t)s_ctx.cfg.sel_settle_us);
            }

            // Clear counters AFTER settle so the next window starts clean.
            #if defined(SOC_PCNT_SUPPORTED) && SOC_PCNT_SUPPORTED
            if (s_ctx.pcnt_active) {
                (void)pcnt_unit_clear_count(s_ctx.unit_cf);
                (void)pcnt_unit_clear_count(s_ctx.unit_cf1);
            }
            #endif
            if (s_ctx.isr_active) {
                portENTER_CRITICAL(&s_ctx.mux);
                s_ctx.isr_cf = 0;
                s_ctx.isr_cf1 = 0;
                portEXIT_CRITICAL(&s_ctx.mux);
            }

            // Store captured counts for the completed phase and advance phase state.
            portENTER_CRITICAL(&s_ctx.mux);
            s_ctx.last_cf_count = cf_counts;
            if (prev_phase == PHASE_VOLTAGE) {
                s_ctx.last_cfu_count = cf1_counts;
            } else {
                s_ctx.last_cfi_count = cf1_counts;
            }
            s_ctx.phase = next_phase;
            portEXIT_CRITICAL(&s_ctx.mux);

            // Only publish a full update once per full period (i.e., after we just finished CURRENT phase).
            const bool do_full = (prev_phase == PHASE_CURRENT);
            if (!do_full) return;

            // Build measurement snapshot
            bl0937_measurements_t m = {0};

        uint32_t cf_last, cfu_last, cfi_last;
        portENTER_CRITICAL(&s_ctx.mux);
        cf_last  = s_ctx.last_cf_count;// last half-window CF
        cfu_last = s_ctx.last_cfu_count;
        cfi_last = s_ctx.last_cfi_count;
        portEXIT_CRITICAL(&s_ctx.mux);

        // CF is measured per half window; approximate full-period CF Hz by averaging the two halves.
        // We only stored the last half. For better stability, we treat CF Hz as last_half/half_s.
        // Users can increase sample_period_ms if they need smoothing.
        m.cf_hz  = counts_to_hz(cf_last,  half_s);
        m.cfu_hz = counts_to_hz(cfu_last, half_s);
        m.cfi_hz = counts_to_hz(cfi_last, half_s);

        compute_physical(&m, &s_ctx.cfg);

        // Energy accumulation (Wh) using dt = full period
        const float dt_h = ((float)s_ctx.cfg.sample_period_ms / 1000.0f) / 3600.0f;
        if (m.valid_power) {
            m.energy_wh = 0.0f; // will be overwritten from accumulated value below
            portENTER_CRITICAL(&s_ctx.mux);
            s_ctx.meas.energy_wh += m.power_w * dt_h;
            m.energy_wh = s_ctx.meas.energy_wh;
            portEXIT_CRITICAL(&s_ctx.mux);
        } else {
            portENTER_CRITICAL(&s_ctx.mux);
            m.energy_wh = s_ctx.meas.energy_wh;
            portEXIT_CRITICAL(&s_ctx.mux);
        }

        // Publish snapshot
        portENTER_CRITICAL(&s_ctx.mux);
        // preserve accumulated energy in s_ctx.meas; overwrite the rest
        float e = s_ctx.meas.energy_wh;
        s_ctx.meas = m;
        s_ctx.meas.energy_wh = e;
        portEXIT_CRITICAL(&s_ctx.mux);

        (void)now_us;
    }

    esp_err_t bl0937_init(const bl0937_config_t *cfg)
    {
        if (!cfg) return ESP_ERR_INVALID_ARG;
        if (cfg->sample_period_ms < 100) return ESP_ERR_INVALID_ARG;

        if (s_ctx.running) return ESP_ERR_INVALID_STATE;

        s_ctx.cfg = *cfg;
        s_ctx.phase = PHASE_VOLTAGE;
        s_ctx.meas = (bl0937_measurements_t){0};
        s_ctx.meas.energy_wh = 0.0f;

        ESP_RETURN_ON_ERROR(gpio_setup_sel(s_ctx.cfg.gpio_sel), TAG, "SEL gpio setup failed");

        // Configure inputs
        gpio_config_t in = {
            .pin_bit_mask = (1ULL << s_ctx.cfg.gpio_cf) | (1ULL << s_ctx.cfg.gpio_cf1),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_RETURN_ON_ERROR(gpio_config(&in), TAG, "gpio_config(inputs) failed");

        #if defined(SOC_PCNT_SUPPORTED) && SOC_PCNT_SUPPORTED
        s_ctx.pcnt_active = false;
        if (!s_ctx.cfg.force_isr_fallback) {
            esp_err_t err = pcnt_setup_unit(&s_ctx.unit_cf,  &s_ctx.chan_cf,  s_ctx.cfg.gpio_cf,  s_ctx.cfg.glitch_filter_ns);
            if (err == ESP_OK) {
                err = pcnt_setup_unit(&s_ctx.unit_cf1, &s_ctx.chan_cf1, s_ctx.cfg.gpio_cf1, s_ctx.cfg.glitch_filter_ns);
            }
            if (err == ESP_OK) {
                s_ctx.pcnt_active = true;
                ESP_LOGI(TAG, "Using PCNT pulse counting");
            } else {
                ESP_LOGW(TAG, "PCNT setup failed (%s), falling back to GPIO ISR", esp_err_to_name(err));
                // Clean up partial PCNT allocs
                if (s_ctx.unit_cf1) { pcnt_del_channel(s_ctx.chan_cf1); pcnt_del_unit(s_ctx.unit_cf1); s_ctx.unit_cf1 = NULL; }
                if (s_ctx.unit_cf)  { pcnt_del_channel(s_ctx.chan_cf);  pcnt_del_unit(s_ctx.unit_cf);  s_ctx.unit_cf  = NULL; }
            }
        }
        #endif

        s_ctx.isr_active = false;

        #if !(defined(SOC_PCNT_SUPPORTED) && SOC_PCNT_SUPPORTED)
        // targets without PCNT => always ISR
        s_ctx.cfg.force_isr_fallback = true;
        #endif

        #if defined(SOC_PCNT_SUPPORTED) && SOC_PCNT_SUPPORTED
        if (!s_ctx.pcnt_active)
            #endif
        {
            // ISR fallback
            // This may already be installed by the application. Treat "already installed" as success.
            esp_err_t isrsvc = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
            if (isrsvc != ESP_OK && isrsvc != ESP_ERR_INVALID_STATE) {
                ESP_RETURN_ON_ERROR(isrsvc, TAG, "gpio_install_isr_service failed");
            }
            gpio_set_intr_type(s_ctx.cfg.gpio_cf, GPIO_INTR_POSEDGE);
            gpio_set_intr_type(s_ctx.cfg.gpio_cf1, GPIO_INTR_POSEDGE);
            ESP_RETURN_ON_ERROR(gpio_isr_handler_add(s_ctx.cfg.gpio_cf,  isr_cf,  NULL), TAG, "isr add CF failed");
            ESP_RETURN_ON_ERROR(gpio_isr_handler_add(s_ctx.cfg.gpio_cf1, isr_cf1, NULL), TAG, "isr add CF1 failed");
            portENTER_CRITICAL(&s_ctx.mux);
            s_ctx.isr_cf = 0;
            s_ctx.isr_cf1 = 0;
            portEXIT_CRITICAL(&s_ctx.mux);
            s_ctx.isr_active = true;
            ESP_LOGI(TAG, "Using GPIO ISR pulse counting");
        }

        // Create periodic timer at half-period
        const int half_ms = s_ctx.cfg.sample_period_ms / 2;
        if (half_ms <= 0) return ESP_ERR_INVALID_ARG;

        esp_timer_create_args_t tcfg = {
            .callback = timer_cb,
            .arg = NULL,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "bl0937",
            .skip_unhandled_events = true,
        };
        ESP_RETURN_ON_ERROR(esp_timer_create(&tcfg, &s_ctx.timer), TAG, "esp_timer_create failed");

        s_ctx.initialized = true;
        return ESP_OK;
    }

    esp_err_t bl0937_start(void)
    {
        if (!s_ctx.initialized) return ESP_ERR_INVALID_STATE;
        if (s_ctx.running) return ESP_OK;

        // Ensure SEL starts at voltage phase
        s_ctx.phase = PHASE_VOLTAGE;
        gpio_set_level(s_ctx.cfg.gpio_sel, 1);

        #if defined(SOC_PCNT_SUPPORTED) && SOC_PCNT_SUPPORTED
        if (s_ctx.pcnt_active) {
            pcnt_unit_clear_count(s_ctx.unit_cf);
            pcnt_unit_clear_count(s_ctx.unit_cf1);
        }
        #endif
        if (s_ctx.isr_active) {
            portENTER_CRITICAL(&s_ctx.mux);
            s_ctx.isr_cf = 0;
            s_ctx.isr_cf1 = 0;
            portEXIT_CRITICAL(&s_ctx.mux);
        }

        const int half_ms = s_ctx.cfg.sample_period_ms / 2;
        ESP_RETURN_ON_ERROR(esp_timer_start_periodic(s_ctx.timer, (uint64_t)half_ms * 1000ULL), TAG, "timer start failed");
        s_ctx.running = true;
        return ESP_OK;
    }

    esp_err_t bl0937_stop(void)
    {
        if (!s_ctx.running) return ESP_OK;
        esp_err_t err = esp_timer_stop(s_ctx.timer);
        if (err != ESP_OK) return err;
        s_ctx.running = false;
        return ESP_OK;
    }

    esp_err_t bl0937_deinit(void)
    {
        if (!s_ctx.initialized) return ESP_OK;
        if (s_ctx.running) {
            bl0937_stop();
        }

        if (s_ctx.timer) {
            esp_timer_delete(s_ctx.timer);
            s_ctx.timer = NULL;
        }

        #if defined(SOC_PCNT_SUPPORTED) && SOC_PCNT_SUPPORTED
        if (s_ctx.pcnt_active) {
            pcnt_unit_stop(s_ctx.unit_cf);
            pcnt_unit_disable(s_ctx.unit_cf);
            pcnt_del_channel(s_ctx.chan_cf);
            pcnt_del_unit(s_ctx.unit_cf);

            pcnt_unit_stop(s_ctx.unit_cf1);
            pcnt_unit_disable(s_ctx.unit_cf1);
            pcnt_del_channel(s_ctx.chan_cf1);
            pcnt_del_unit(s_ctx.unit_cf1);

            s_ctx.unit_cf = NULL; s_ctx.chan_cf = NULL;
            s_ctx.unit_cf1 = NULL; s_ctx.chan_cf1 = NULL;
            s_ctx.pcnt_active = false;
        }
        #endif

        if (s_ctx.isr_active) {
            gpio_isr_handler_remove(s_ctx.cfg.gpio_cf);
            gpio_isr_handler_remove(s_ctx.cfg.gpio_cf1);
            gpio_set_intr_type(s_ctx.cfg.gpio_cf, GPIO_INTR_DISABLE);
            gpio_set_intr_type(s_ctx.cfg.gpio_cf1, GPIO_INTR_DISABLE);
            // Don't uninstall ISR service globally (other code may use it).
            s_ctx.isr_active = false;
        }

        s_ctx.initialized = false;
        memset(&s_ctx.meas, 0, sizeof(s_ctx.meas));
        return ESP_OK;
    }

    bl0937_measurements_t bl0937_get(void)
    {
        bl0937_measurements_t out;
        portENTER_CRITICAL(&s_ctx.mux);
        out = s_ctx.meas;
        portEXIT_CRITICAL(&s_ctx.mux);
        return out;
    }

    void bl0937_reset_energy(void)
    {
        portENTER_CRITICAL(&s_ctx.mux);
        s_ctx.meas.energy_wh = 0.0f;
        portEXIT_CRITICAL(&s_ctx.mux);
    }
        ESP_RETURN_ON_ERROR(gpio_isr_handler_add(s_ctx.cfg.gpio_cf1, isr_cf1, NULL), TAG, "isr add CF1 failed");
        portENTER_CRITICAL(&s_ctx.mux);
        s_ctx.isr_cf = 0;
        s_ctx.isr_cf1 = 0;
        portEXIT_CRITICAL(&s_ctx.mux);
        s_ctx.isr_active = true;
        ESP_LOGI(TAG, "Using GPIO ISR pulse counting");
    }

    // Create periodic timer at half-period
    const int half_ms = s_ctx.cfg.sample_period_ms / 2;
    if (half_ms <= 0) return ESP_ERR_INVALID_ARG;

    esp_timer_create_args_t tcfg = {
        .callback = timer_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "bl0937",
        .skip_unhandled_events = true,
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&tcfg, &s_ctx.timer), TAG, "esp_timer_create failed");

    s_ctx.initialized = true;
    return ESP_OK;
}

esp_err_t bl0937_start(void)
{
    if (!s_ctx.initialized) return ESP_ERR_INVALID_STATE;
    if (s_ctx.running) return ESP_OK;

    // Ensure SEL starts at voltage phase
    s_ctx.phase = PHASE_VOLTAGE;
    gpio_set_level(s_ctx.cfg.gpio_sel, 1);

    #if defined(SOC_PCNT_SUPPORTED) && SOC_PCNT_SUPPORTED
    if (s_ctx.pcnt_active) {
        pcnt_unit_clear_count(s_ctx.unit_cf);
        pcnt_unit_clear_count(s_ctx.unit_cf1);
    }
    #endif
    if (s_ctx.isr_active) {
        portENTER_CRITICAL(&s_ctx.mux);
        s_ctx.isr_cf = 0;
        s_ctx.isr_cf1 = 0;
        portEXIT_CRITICAL(&s_ctx.mux);
    }

    const int half_ms = s_ctx.cfg.sample_period_ms / 2;
    ESP_RETURN_ON_ERROR(esp_timer_start_periodic(s_ctx.timer, (uint64_t)half_ms * 1000ULL), TAG, "timer start failed");
    s_ctx.running = true;
    return ESP_OK;
}

esp_err_t bl0937_stop(void)
{
    if (!s_ctx.running) return ESP_OK;
    esp_err_t err = esp_timer_stop(s_ctx.timer);
    if (err != ESP_OK) return err;
    s_ctx.running = false;
    return ESP_OK;
}

esp_err_t bl0937_deinit(void)
{
    if (!s_ctx.initialized) return ESP_OK;
    if (s_ctx.running) {
        bl0937_stop();
    }

    if (s_ctx.timer) {
        esp_timer_delete(s_ctx.timer);
        s_ctx.timer = NULL;
    }

    #if defined(SOC_PCNT_SUPPORTED) && SOC_PCNT_SUPPORTED
    if (s_ctx.pcnt_active) {
        pcnt_unit_stop(s_ctx.unit_cf);
        pcnt_unit_disable(s_ctx.unit_cf);
        pcnt_del_channel(s_ctx.chan_cf);
        pcnt_del_unit(s_ctx.unit_cf);

        pcnt_unit_stop(s_ctx.unit_cf1);
        pcnt_unit_disable(s_ctx.unit_cf1);
        pcnt_del_channel(s_ctx.chan_cf1);
        pcnt_del_unit(s_ctx.unit_cf1);

        s_ctx.unit_cf = NULL; s_ctx.chan_cf = NULL;
        s_ctx.unit_cf1 = NULL; s_ctx.chan_cf1 = NULL;
        s_ctx.pcnt_active = false;
    }
    #endif

    if (s_ctx.isr_active) {
        gpio_isr_handler_remove(s_ctx.cfg.gpio_cf);
        gpio_isr_handler_remove(s_ctx.cfg.gpio_cf1);
        gpio_set_intr_type(s_ctx.cfg.gpio_cf, GPIO_INTR_DISABLE);
        gpio_set_intr_type(s_ctx.cfg.gpio_cf1, GPIO_INTR_DISABLE);
        // Don't uninstall ISR service globally (other code may use it).
        s_ctx.isr_active = false;
    }

    s_ctx.initialized = false;
    memset(&s_ctx.meas, 0, sizeof(s_ctx.meas));
    return ESP_OK;
}

bl0937_measurements_t bl0937_get(void)
{
    bl0937_measurements_t out;
    portENTER_CRITICAL(&s_ctx.mux);
    out = s_ctx.meas;
    portEXIT_CRITICAL(&s_ctx.mux);
    return out;
}

void bl0937_reset_energy(void)
{
    portENTER_CRITICAL(&s_ctx.mux);
    s_ctx.meas.energy_wh = 0.0f;
    portEXIT_CRITICAL(&s_ctx.mux);
}
    if (s_ctx.pcnt_active) {
        pcnt_unit_stop(s_ctx.unit_cf);
        pcnt_unit_disable(s_ctx.unit_cf);
        pcnt_del_channel(s_ctx.chan_cf);
        pcnt_del_unit(s_ctx.unit_cf);

        pcnt_unit_stop(s_ctx.unit_cf1);
        pcnt_unit_disable(s_ctx.unit_cf1);
        pcnt_del_channel(s_ctx.chan_cf1);
        pcnt_del_unit(s_ctx.unit_cf1);

        s_ctx.unit_cf = NULL; s_ctx.chan_cf = NULL;
        s_ctx.unit_cf1 = NULL; s_ctx.chan_cf1 = NULL;
        s_ctx.pcnt_active = false;
    }
    #endif

    if (s_ctx.isr_active) {
        gpio_isr_handler_remove(s_ctx.cfg.gpio_cf);
        gpio_isr_handler_remove(s_ctx.cfg.gpio_cf1);
        gpio_set_intr_type(s_ctx.cfg.gpio_cf, GPIO_INTR_DISABLE);
        gpio_set_intr_type(s_ctx.cfg.gpio_cf1, GPIO_INTR_DISABLE);
        // Don't uninstall ISR service globally (other code may use it).
        s_ctx.isr_active = false;
    }

    s_ctx.initialized = false;
    memset(&s_ctx.meas, 0, sizeof(s_ctx.meas));
    return ESP_OK;
}

bl0937_measurements_t bl0937_get(void)
{
    bl0937_measurements_t out;
    portENTER_CRITICAL(&s_ctx.mux);
    out = s_ctx.meas;
    portEXIT_CRITICAL(&s_ctx.mux);
    return out;
}

void bl0937_reset_energy(void)
{
    portENTER_CRITICAL(&s_ctx.mux);
    s_ctx.meas.energy_wh = 0.0f;
    portEXIT_CRITICAL(&s_ctx.mux);
}
