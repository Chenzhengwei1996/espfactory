// Copyright 2020 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.


#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/ledc.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "driver/timer.h"
#include "soc/timer_group_struct.h"
#include "soc/ledc_struct.h"
#include "soc/ledc_reg.h"
#include "hal/gpio_ll.h"
#include "soc/timer_group_caps.h"
#include "pwm_audio.h"
#include "sdkconfig.h"

#ifndef CONFIG_IDF_TARGET_ESP32S2
#ifndef CONFIG_IDF_TARGET_ESP32
#error No defined idf target esp32 or esp32s2
#endif
#endif


#ifdef CONFIG_IDF_TARGET_ESP32S2
#include "esp32s2/rom/ets_sys.h"
#endif /**< CONFIG_IDF_TARGET_ESP32S2 */
#ifdef CONFIG_IDF_TARGET_ESP32
#include "esp32/rom/ets_sys.h"
#endif /**< CONFIG_IDF_TARGET_ESP32 */



static const char *TAG = "pwm_audio";

#define PWM_AUDIO_CHECK(a, str, ret_val)                          \
    if (!(a))                                                     \
    {                                                             \
        ESP_LOGE(TAG, "%s(%d): %s", __FUNCTION__, __LINE__, str); \
        return (ret_val);                                         \
    }

static const char *PWM_AUDIO_PARAM_ADDR_ERROR = "PWM AUDIO PARAM ADDR ERROR";
static const char *PWM_AUDIO_PARAM_ERROR      = "PWM AUDIO PARAM ERROR";
static const char *PWM_AUDIO_FRAMERATE_ERROR  = "PWM AUDIO FRAMERATE ERROR";
static const char *PWM_AUDIO_STATUS_ERROR     = "PWM AUDIO STATUS ERROR";
static const char *PWM_AUDIO_TG_NUM_ERROR     = "PWM AUDIO TIMER GROUP NUMBER ERROR";
static const char *PWM_AUDIO_TIMER_NUM_ERROR  = "PWM AUDIO TIMER NUMBER ERROR";
static const char *PWM_AUDIO_ALLOC_ERROR      = "PWM AUDIO ALLOC ERROR";
static const char *PWM_AUDIO_RESOLUTION_ERROR = "PWM AUDIO RESOLUTION ERROR";

#define BUFFER_MIN_SIZE (256UL)
#define SAMPLE_RATE_MAX (48000)
#define SAMPLE_RATE_MIN (8000)
#define CHANNEL_LEFT_INDEX  (0)
#define CHANNEL_RIGHT_INDEX (1)
#define CHANNEL_LEFT_MASK   (0x01)
#define CHANNEL_RIGHT_MASK  (0x02)


typedef struct {
    char *buf;                         /**< Original pointer */
    uint32_t volatile head;            /**< ending pointer */
    uint32_t volatile tail;            /**< Read pointer */
    uint32_t size;                     /**< Buffer size */
    uint32_t is_give;                  /**< semaphore give flag */
    SemaphoreHandle_t semaphore_rb;    /**< Semaphore for ringbuffer */

} ringBuf;
typedef ringBuf *ringbuf_handle_t;

typedef struct {
    pwm_audio_config_t    config;                          /**< pwm audio config struct */
    ledc_channel_config_t ledc_channel[PWM_AUDIO_CH_MAX];  /**< ledc channel config */
    ledc_timer_config_t   ledc_timer;                      /**< ledc timer config  */
    timg_dev_t            *timg_dev;                       /**< timer group register pointer */
    ringbuf_handle_t      ringbuf;                         /**< audio ringbuffer pointer */
    uint32_t              channel_mask;                    /**< channel gpio mask */
    uint32_t              channel_set_num;                 /**< channel audio set number */
    int32_t               framerate;                       /*!< frame rates in Hz */
    int32_t               bits_per_sample;                 /*!< bits per sample (8, 16, 32) */

    pwm_audio_status_t status;
} pwm_audio_handle;
typedef pwm_audio_handle *pwm_audio_handle_t;

/**< ledc some register pointer */
static volatile uint32_t *g_ledc_left_conf0_val  = NULL;
static volatile uint32_t *g_ledc_left_conf1_val  = NULL;
static volatile uint32_t *g_ledc_left_duty_val   = NULL;
static volatile uint32_t *g_ledc_right_conf0_val = NULL;
static volatile uint32_t *g_ledc_right_conf1_val = NULL;
static volatile uint32_t *g_ledc_right_duty_val  = NULL;

/**< pwm audio handle pointer */
static pwm_audio_handle_t g_pwm_audio_handle = NULL;

/**
 * Ringbuffer for pwm audio
 */
static esp_err_t rb_destroy(ringbuf_handle_t rb)
{
    if (rb == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (rb->buf) {
        free(rb->buf);
    }

    if (rb->semaphore_rb) {
        vSemaphoreDelete(rb->semaphore_rb);
    }

    free(rb);
    rb = NULL;
    return ESP_OK;
}
static ringbuf_handle_t rb_create(int size)
{
    if (size < (BUFFER_MIN_SIZE << 2)) {
        ESP_LOGE(TAG, "Invalid buffer size, Minimum = %d", (int32_t)(BUFFER_MIN_SIZE << 2));
        return NULL;
    }

    ringbuf_handle_t rb = NULL;
    char *buf = NULL;

    do {
        bool _success =
            (
                (rb             = malloc(sizeof(ringBuf))) &&
                (buf            = malloc(size))   &&
                (rb->semaphore_rb   = xSemaphoreCreateBinary())

            );

        if (!_success) {
            break;
        }

        rb->is_give = 0;
        rb->buf = buf;
        rb->head = rb->tail = 0;
        rb->size = size;
        return rb;

    } while (0);

    rb_destroy(rb);
    return NULL;
}

static uint32_t IRAM_ATTR rb_get_count(ringbuf_handle_t rb)
{
    uint32_t tail = rb->tail;

    if (rb->head >= tail) {
        return (rb->head - tail);
    }

    return (rb->size - (tail - rb->head));
}

static uint32_t IRAM_ATTR rb_get_free(ringbuf_handle_t rb)
{
    /** < Free a byte to judge the ringbuffer direction */
    return (rb->size - rb_get_count(rb) - 1);
}

static esp_err_t rb_flush(ringbuf_handle_t rb)
{
    rb->tail = rb->head = 0;
    return ESP_OK;
}

static esp_err_t IRAM_ATTR rb_read_byte(ringbuf_handle_t rb, uint8_t *outdata)
{
    uint32_t tail = rb->tail;

    if (tail == rb->head) {
        return ESP_FAIL;
    }

    // Send a byte from the buffer
    *outdata = rb->buf[tail];

    // Update tail position
    tail++;

    if (tail == rb->size) {
        tail = 0;
    }

    rb->tail = tail;
    return ESP_OK;
}

static esp_err_t rb_write_byte(ringbuf_handle_t rb, const uint8_t indata)
{
    // Calculate next head
    uint32_t next_head = rb->head + 1;

    if (next_head == rb->size) {
        next_head = 0;
    }

    if (next_head == rb->tail) {
        return ESP_FAIL;
    }

    // Store data and advance head
    rb->buf[rb->head] = indata;
    rb->head = next_head;
    return ESP_OK;
}

static esp_err_t rb_wait_semaphore(ringbuf_handle_t rb, TickType_t ticks_to_wait)
{
    rb->is_give = 0; /**< As long as it's written, it's allowed to give semaphore again */

    if (xSemaphoreTake(rb->semaphore_rb, ticks_to_wait) == pdTRUE) {
        return ESP_OK;
    }

    return ESP_FAIL;
}


/*
 * Note:
 * In order to improve efficiency, register is operated directly
 */
static inline void ledc_set_left_duty_fast(uint32_t duty_val)
{
    *g_ledc_left_duty_val = (duty_val) << 4; /* Discard decimal part */
    *g_ledc_left_conf0_val |= 0x00000014;
    *g_ledc_left_conf1_val |= 0x80000000;
}
static inline void ledc_set_right_duty_fast(uint32_t duty_val)
{
    *g_ledc_right_duty_val = (duty_val) << 4; /* Discard decimal part */
    *g_ledc_right_conf0_val |= 0x00000014;
    *g_ledc_right_conf1_val |= 0x80000000;
}


/*
 * Timer group ISR handler
 */
static void IRAM_ATTR timer_group_isr(void *para)
{
    pwm_audio_handle_t handle = g_pwm_audio_handle;

    if (handle == NULL) {
        ets_printf("pwm audio not initialized\n");
        return;
    }

#ifdef CONFIG_IDF_TARGET_ESP32S2

    /* Clear the interrupt */
    if ((handle->timg_dev)->int_st.val & BIT(handle->config.timer_num)) {
        (handle->timg_dev)->int_clr.val |= (1UL << handle->config.timer_num);
    }

    /* After the alarm has been triggered
      we need enable it again, so it is triggered the next time */
    handle->timg_dev->hw_timer[handle->config.timer_num].config.alarm_en = TIMER_ALARM_EN;
#endif /**< CONFIG_IDF_TARGET_ESP32S2 */

#ifdef CONFIG_IDF_TARGET_ESP32

    /* Clear the interrupt */
    if (handle->timg_dev->int_st_timers.val & BIT(handle->config.timer_num)) {
        handle->timg_dev->int_clr_timers.val |= (1UL << handle->config.timer_num);
    }

    /* After the alarm has been triggered
      we need enable it again, so it is triggered the next time */
    handle->timg_dev->hw_timer[handle->config.timer_num].config.alarm_en = TIMER_ALARM_EN;

#endif /**< CONFIG_IDF_TARGET_ESP32 */

    static uint8_t wave_h, wave_l;
    static uint16_t value;

    /**
     * It is believed that the channel configured with GPIO needs to output sound
    */
    if (handle->channel_mask & CHANNEL_LEFT_MASK) {
        if (handle->config.duty_resolution > 8) {
            rb_read_byte(handle->ringbuf, &wave_l);

            if (ESP_OK == rb_read_byte(handle->ringbuf, &wave_h)) {
                value = ((wave_h << 8) | wave_l);
                ledc_set_left_duty_fast(value);/**< set the PWM duty */
            }
        } else {
            if (ESP_OK == rb_read_byte(handle->ringbuf, &wave_h)) {
                ledc_set_left_duty_fast(wave_h);/**< set the PWM duty */
            }
        }
    }

    /**
     * If two gpios are configured, but the audio data has only one channel, copy the data to the right channel
     * Instead, the right channel data is discarded
    */
    if (handle->channel_mask & CHANNEL_RIGHT_MASK) {
        if (handle->channel_set_num == 1) {
            if (handle->config.duty_resolution > 8) {
                ledc_set_right_duty_fast(value);/**< set the PWM duty */
            } else {
                ledc_set_right_duty_fast(wave_h);/**< set the PWM duty */
            }
        } else {
            if (handle->config.duty_resolution > 8) {
                rb_read_byte(handle->ringbuf, &wave_l);

                if (ESP_OK == rb_read_byte(handle->ringbuf, &wave_h)) {
                    value = ((wave_h << 8) | wave_l);
                    ledc_set_right_duty_fast(value);/**< set the PWM duty */
                }
            } else {
                if (ESP_OK == rb_read_byte(handle->ringbuf, &wave_h)) {
                    ledc_set_right_duty_fast(wave_h);/**< set the PWM duty */
                }
            }
        }
    } else {
        if (handle->channel_set_num == 2) {
            /**
             * Discard the right channel data only if the right channel is configured but the audio data is stereo
             * Read buffer but do nothing
             */
            if (handle->config.duty_resolution > 8) {
                rb_read_byte(handle->ringbuf, &wave_h);
                rb_read_byte(handle->ringbuf, &wave_h);
            } else {
                rb_read_byte(handle->ringbuf, &wave_h);
            }

            rb_read_byte(handle->ringbuf, &wave_l);
        }
    }

    /**
     * Send semaphore when buffer free is more than BUFFER_MIN_SIZE
     */
    if (0 == handle->ringbuf->is_give && rb_get_free(handle->ringbuf) > BUFFER_MIN_SIZE) {
        /**< The execution time of the following code is 2.71 microsecond */
        handle->ringbuf->is_give = 1; /**< To prevent multiple give semaphores */
        BaseType_t xHigherPriorityTaskWoken;
        xSemaphoreGiveFromISR(handle->ringbuf->semaphore_rb, &xHigherPriorityTaskWoken);

        if (pdFALSE != xHigherPriorityTaskWoken) {
            portYIELD_FROM_ISR();
        }
    }
}

esp_err_t pwm_audio_get_status(pwm_audio_status_t *status)
{
    pwm_audio_handle_t handle = g_pwm_audio_handle;
    *status = handle->status;
    return ESP_OK;
}

esp_err_t pwm_audio_init(const pwm_audio_config_t *cfg)
{
    esp_err_t res = ESP_OK;
    PWM_AUDIO_CHECK(cfg != NULL, PWM_AUDIO_PARAM_ADDR_ERROR, ESP_ERR_INVALID_ARG);
    PWM_AUDIO_CHECK(cfg->tg_num < TIMER_GROUP_MAX, PWM_AUDIO_TG_NUM_ERROR, ESP_ERR_INVALID_ARG);
    PWM_AUDIO_CHECK(cfg->timer_num < TIMER_MAX, PWM_AUDIO_TIMER_NUM_ERROR, ESP_ERR_INVALID_ARG);
    PWM_AUDIO_CHECK(cfg->duty_resolution <= 10 && cfg->duty_resolution >= 8, PWM_AUDIO_RESOLUTION_ERROR, ESP_ERR_INVALID_ARG);

    pwm_audio_handle_t handle = NULL;

    handle = malloc(sizeof(pwm_audio_handle));
    PWM_AUDIO_CHECK(handle != NULL, PWM_AUDIO_ALLOC_ERROR, ESP_ERR_NO_MEM);
    memset(handle, 0, sizeof(pwm_audio_handle));

    handle->ringbuf = rb_create(cfg->ringbuf_len);
    PWM_AUDIO_CHECK(handle->ringbuf != NULL, PWM_AUDIO_ALLOC_ERROR, ESP_ERR_NO_MEM);

    handle->config = *cfg;
    g_pwm_audio_handle = handle;

    /**
     * config ledc to generate pwm
     */
    if (cfg->tg_num == TIMER_GROUP_0) {
        handle->timg_dev = &TIMERG0;
    } else {
        handle->timg_dev = &TIMERG1;
    }

    handle->channel_mask = 0;

    if (handle->config.gpio_num_left >= 0) {
        handle->ledc_channel[CHANNEL_LEFT_INDEX].channel = handle->config.ledc_channel_left;
        handle->ledc_channel[CHANNEL_LEFT_INDEX].duty = 0;
        handle->ledc_channel[CHANNEL_LEFT_INDEX].gpio_num = handle->config.gpio_num_left;
        handle->ledc_channel[CHANNEL_LEFT_INDEX].speed_mode = LEDC_LOW_SPEED_MODE;
        handle->ledc_channel[CHANNEL_LEFT_INDEX].hpoint = 0;
        handle->ledc_channel[CHANNEL_LEFT_INDEX].timer_sel = handle->config.ledc_timer_sel;
        handle->ledc_channel[CHANNEL_LEFT_INDEX].intr_type = LEDC_INTR_DISABLE;
        res = ledc_channel_config(&handle->ledc_channel[CHANNEL_LEFT_INDEX]);
        PWM_AUDIO_CHECK(ESP_OK == res, PWM_AUDIO_PARAM_ERROR, ESP_ERR_INVALID_ARG);
        handle->channel_mask |= CHANNEL_LEFT_MASK;
    }

    if (handle->config.gpio_num_right >= 0) {
        handle->ledc_channel[CHANNEL_RIGHT_INDEX].channel = handle->config.ledc_channel_right;
        handle->ledc_channel[CHANNEL_RIGHT_INDEX].duty = 0;
        handle->ledc_channel[CHANNEL_RIGHT_INDEX].gpio_num = handle->config.gpio_num_right;
        handle->ledc_channel[CHANNEL_RIGHT_INDEX].speed_mode = LEDC_LOW_SPEED_MODE;
        handle->ledc_channel[CHANNEL_RIGHT_INDEX].hpoint = 0;
        handle->ledc_channel[CHANNEL_RIGHT_INDEX].timer_sel = handle->config.ledc_timer_sel;
        handle->ledc_channel[CHANNEL_RIGHT_INDEX].intr_type = LEDC_INTR_DISABLE;
        res = ledc_channel_config(&handle->ledc_channel[CHANNEL_RIGHT_INDEX]);
        PWM_AUDIO_CHECK(ESP_OK == res, PWM_AUDIO_PARAM_ERROR, ESP_ERR_INVALID_ARG);
        handle->channel_mask |= CHANNEL_RIGHT_MASK;
    }

    PWM_AUDIO_CHECK(0 != handle->channel_mask, PWM_AUDIO_PARAM_ERROR, ESP_ERR_INVALID_ARG);

#ifdef CONFIG_IDF_TARGET_ESP32S2
    handle->ledc_timer.clk_cfg = LEDC_USE_APB_CLK;
#endif
    handle->ledc_timer.speed_mode = LEDC_LOW_SPEED_MODE;
    handle->ledc_timer.duty_resolution = handle->config.duty_resolution;
    handle->ledc_timer.timer_num = handle->config.ledc_timer_sel;
    uint32_t freq = (APB_CLK_FREQ / (1 << handle->ledc_timer.duty_resolution));
    handle->ledc_timer.freq_hz = freq - (freq % 1000); // fixed PWM frequency ,It's a multiple of 1000
    res = ledc_timer_config(&handle->ledc_timer);
    PWM_AUDIO_CHECK(res == ESP_OK, PWM_AUDIO_PARAM_ERROR, ESP_ERR_INVALID_ARG);

    /**
     * Get the address of LEDC register to reduce the addressing time
     */
    g_ledc_left_duty_val = &LEDC.channel_group[handle->ledc_timer.speed_mode].\
                           channel[handle->ledc_channel[CHANNEL_LEFT_INDEX].channel].duty.val;
    g_ledc_left_conf0_val = &LEDC.channel_group[handle->ledc_timer.speed_mode].\
                            channel[handle->ledc_channel[CHANNEL_LEFT_INDEX].channel].conf0.val;
    g_ledc_left_conf1_val = &LEDC.channel_group[handle->ledc_timer.speed_mode].\
                            channel[handle->ledc_channel[CHANNEL_LEFT_INDEX].channel].conf1.val;
    g_ledc_right_duty_val = &LEDC.channel_group[handle->ledc_timer.speed_mode].\
                            channel[handle->ledc_channel[CHANNEL_RIGHT_INDEX].channel].duty.val;
    g_ledc_right_conf0_val = &LEDC.channel_group[handle->ledc_timer.speed_mode].\
                             channel[handle->ledc_channel[CHANNEL_RIGHT_INDEX].channel].conf0.val;
    g_ledc_right_conf1_val = &LEDC.channel_group[handle->ledc_timer.speed_mode].\
                             channel[handle->ledc_channel[CHANNEL_RIGHT_INDEX].channel].conf1.val;

    /**< set a initial parameter */
    res = pwm_audio_set_param(16000, 8, 2);
    PWM_AUDIO_CHECK(ESP_OK == res, PWM_AUDIO_PARAM_ERROR, ESP_ERR_INVALID_ARG);

    handle->status = PWM_AUDIO_STATUS_IDLE;

    return res;
}


esp_err_t pwm_audio_set_param(int rate, ledc_timer_bit_t bits, int ch)
{
    esp_err_t res = ESP_OK;

    PWM_AUDIO_CHECK(g_pwm_audio_handle->status != PWM_AUDIO_STATUS_BUSY, PWM_AUDIO_STATUS_ERROR, ESP_ERR_INVALID_ARG);
    PWM_AUDIO_CHECK(rate <= SAMPLE_RATE_MAX && rate >= SAMPLE_RATE_MIN, PWM_AUDIO_FRAMERATE_ERROR, ESP_ERR_INVALID_ARG);
    PWM_AUDIO_CHECK(bits == 32 || bits == 16 || bits == 8, PWM_AUDIO_FRAMERATE_ERROR, ESP_ERR_INVALID_ARG);
    PWM_AUDIO_CHECK(ch <= 2 && ch >= 1, PWM_AUDIO_FRAMERATE_ERROR, ESP_ERR_INVALID_ARG);

    pwm_audio_handle_t handle = g_pwm_audio_handle;

    handle->framerate = rate;
    handle->bits_per_sample = bits;
    handle->channel_set_num = ch;

    /* Select and initialize basic parameters of the timer */
    timer_config_t config = {0};
    config.divider = 16;
    config.counter_dir = TIMER_COUNT_UP;
    config.counter_en = TIMER_PAUSE;
    config.alarm_en = TIMER_ALARM_EN;
    config.intr_type = TIMER_INTR_LEVEL;
    config.auto_reload = 1;
#ifdef TIMER_GROUP_SUPPORTS_XTAL_CLOCK
    config.clk_src = TIMER_SRC_CLK_APB;  /* ESP32-S2 specific control bit !!!*/
#endif
    timer_init(handle->config.tg_num, handle->config.timer_num, &config);

    /* Timer's counter will initially start from value below.
    Also, if auto_reload is set, this value will be automatically reload on alarm */
    timer_set_counter_value(handle->config.tg_num, handle->config.timer_num, 0x00000000ULL);

    /* Configure the alarm value and the interrupt on alarm. */
    timer_set_alarm_value(handle->config.tg_num, handle->config.timer_num, (TIMER_BASE_CLK / config.divider) / handle->framerate);
    timer_enable_intr(handle->config.tg_num, handle->config.timer_num);
    timer_isr_register(handle->config.tg_num, handle->config.timer_num, timer_group_isr, NULL, ESP_INTR_FLAG_IRAM, NULL);
    return res;
}

esp_err_t pwm_audio_set_sample_rate(int rate)
{
    esp_err_t res;
    PWM_AUDIO_CHECK(g_pwm_audio_handle->status != PWM_AUDIO_STATUS_BUSY, PWM_AUDIO_STATUS_ERROR, ESP_ERR_INVALID_ARG);
    PWM_AUDIO_CHECK(rate <= SAMPLE_RATE_MAX && rate >= SAMPLE_RATE_MIN, PWM_AUDIO_FRAMERATE_ERROR, ESP_ERR_INVALID_ARG);

    pwm_audio_handle_t handle = g_pwm_audio_handle;
    handle->framerate = rate;
    uint16_t div = (uint16_t)handle->timg_dev->hw_timer[handle->config.timer_num].config.divider;
    res = timer_set_alarm_value(handle->config.tg_num, handle->config.timer_num, (TIMER_BASE_CLK / div) / handle->framerate);
    return res;
}

esp_err_t pwm_audio_write(uint8_t *inbuf, size_t inbuf_len, size_t *bytes_written, TickType_t ticks_to_wait)
{
    esp_err_t res = ESP_OK;
    pwm_audio_handle_t handle = g_pwm_audio_handle;
    PWM_AUDIO_CHECK(inbuf != NULL && bytes_written != NULL && inbuf_len != 0, PWM_AUDIO_PARAM_ADDR_ERROR, ESP_ERR_INVALID_ARG);

    *bytes_written = 0;
    ringbuf_handle_t rb = handle->ringbuf;

    while (inbuf_len) {
        if (ESP_OK == rb_wait_semaphore(rb, ticks_to_wait)) {
            uint32_t free = rb_get_free(rb);
            uint32_t bytes_can_write = inbuf_len;

            if (inbuf_len > free) {
                bytes_can_write = free;
            }

            bytes_can_write &= 0xfffffffc;/**< Aligned data, bytes_can_write should be an integral multiple of 4 */

            if (0 == bytes_can_write) {
                *bytes_written += inbuf_len;  /**< Discard the last misaligned bytes of data directly */
                return ESP_OK;
            }

            /**< Get the difference between PWM resolution and audio samplewidth */
            int8_t shift = handle->bits_per_sample - handle->config.duty_resolution;
            uint32_t len = bytes_can_write;

            switch (handle->bits_per_sample) {
                case 8: {
                    if (shift < 0) {
                        /**< When the PWM resolution is greater than 8 bits, the value needs to be expanded */
                        uint16_t value;
                        uint8_t temp;
                        shift = -shift;
                        len >>= 1;
                        bytes_can_write >>= 1;

                        for (size_t i = 0; i < len; i++) {
                            temp = inbuf[i] + 0x7f; /**< offset */
                            value = temp << shift;
                            rb_write_byte(rb, value);
                            rb_write_byte(rb, value >> 8);
                        }
                    } else {
                        uint8_t value;

                        for (size_t i = 0; i < len; i++) {
                            value = inbuf[i] + 0x7f; /**< offset */
                            rb_write_byte(rb, value);
                        }
                    }
                }
                break;

                case 16: {
                    len >>= 1;
                    uint16_t *buf_16b = (uint16_t *)inbuf;
                    uint16_t value;
                    int16_t temp;

                    if (handle->config.duty_resolution > 8) {
                        for (size_t i = 0; i < len; i++) {
                            temp = buf_16b[i];
                            value = temp + 0x7fff; /**< offset */
                            value >>= shift;
                            rb_write_byte(rb, value);
                            rb_write_byte(rb, value >> 8);
                        }
                    } else {
                        /**
                         * When the PWM resolution is 8 bit, only one byte is transmitted
                         */
                        for (size_t i = 0; i < len; i++) {
                            temp = buf_16b[i];
                            value = temp + 0x7fff; /**< offset */
                            value >>= shift;
                            rb_write_byte(rb, value);
                        }
                    }
                }
                break;

                case 32: {
                    len >>= 2;
                    uint32_t *buf_32b = (uint32_t *)inbuf;
                    uint32_t value;
                    int32_t temp;

                    if (handle->config.duty_resolution > 8) {
                        for (size_t i = 0; i < len; i++) {
                            temp = buf_32b[i];
                            value = temp + 0x7fffffff; /**< offset */
                            value >>= shift;
                            rb_write_byte(rb, value);
                            rb_write_byte(rb, value >> 8);
                        }
                    } else {
                        /**
                         * When the PWM resolution is 8 bit, only one byte is transmitted
                         */
                        for (size_t i = 0; i < len; i++) {
                            temp = buf_32b[i];
                            value = temp + 0x7fffffff; /**< offset */
                            value >>= shift;
                            rb_write_byte(rb, value);
                        }
                    }
                }
                break;

                default:
                    break;
            }

            inbuf += bytes_can_write;
            inbuf_len -= bytes_can_write;
            *bytes_written += bytes_can_write;

        } else {
            res = ESP_FAIL;
        }
    }

    return res;
}

esp_err_t pwm_audio_start(void)
{
    esp_err_t res;
    pwm_audio_handle_t handle = g_pwm_audio_handle;
    PWM_AUDIO_CHECK(handle->status == PWM_AUDIO_STATUS_IDLE, PWM_AUDIO_STATUS_ERROR, ESP_ERR_INVALID_STATE);

    handle->status = PWM_AUDIO_STATUS_BUSY;

    timer_enable_intr(handle->config.tg_num, handle->config.timer_num);
    res = timer_start(handle->config.tg_num, handle->config.timer_num);
    return res;
}

esp_err_t pwm_audio_stop(void)
{
    pwm_audio_handle_t handle = g_pwm_audio_handle;

    /**< just disable timer ,keep pwm output to reduce switching nosie */
    timer_pause(handle->config.tg_num, handle->config.timer_num);
    timer_disable_intr(handle->config.tg_num, handle->config.timer_num);
    rb_flush(handle->ringbuf);  /**< flush ringbuf, avoid play noise */
    handle->status = PWM_AUDIO_STATUS_IDLE;
    return ESP_OK;
}

esp_err_t pwm_audio_deinit(void)
{
    pwm_audio_handle_t handle = g_pwm_audio_handle;
    PWM_AUDIO_CHECK(handle != NULL, PWM_AUDIO_PARAM_ADDR_ERROR, ESP_FAIL);

    handle->status = PWM_AUDIO_STATUS_UN_INIT;
    pwm_audio_stop();

    for (size_t i = 0; i < PWM_AUDIO_CH_MAX; i++) {
        if (handle->ledc_channel[i].gpio_num >= 0) {
            ledc_stop(handle->ledc_channel[i].speed_mode, handle->ledc_channel[i].channel, 0);
        }
    }

    /**< set the channel gpios input mode */
    for (size_t i = 0; i < PWM_AUDIO_CH_MAX; i++) {
        if (handle->ledc_channel[i].gpio_num >= 0) {
            gpio_set_direction(handle->ledc_channel[i].gpio_num, GPIO_MODE_INPUT);
        }
    }

    free(handle);
    rb_destroy(handle->ringbuf);
    return ESP_OK;
}
