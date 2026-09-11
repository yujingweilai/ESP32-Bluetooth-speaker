#include <string.h>
#include "oled_ssd1315.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "font.h"
#include "esp_check.h"

// Blinking effects (BT and Charge) are handled independently.

#define TAG "oled"

#define CMD_DISPLAY_OFF 0xAE
#define CMD_DISPLAY_ON 0xAF
#define CMD_SET_DISP_CLK_DIV 0xD5
#define CMD_SET_MULTIPLEX 0xA8
#define CMD_SET_DISP_OFFSET 0xD3
#define CMD_SET_START_LINE0 0x40
#define CMD_CHARGE_PUMP 0x8D
#define CMD_SET_SEG_REMAP_0 0xA0
#define CMD_SET_SEG_REMAP_1 0xA1
#define CMD_SET_COM_SCAN_DEC 0xC8
#define CMD_SET_COM_SCAN_INC 0xC0
#define CMD_SET_COM_PINS 0xDA
#define CMD_SET_CONTRAST 0x81
#define CMD_SET_PRECHARGE 0xD9
#define CMD_SET_VCOM_DESEL 0xDB
#define CMD_ENTIRE_DISPLAY_ON 0xA4
#define CMD_NORMAL_DISPLAY 0xA6
#define CMD_INVERT_DISPLAY 0xA7
#define CMD_MEMORY_MODE 0x20
#define CMD_COLUMN_ADDR 0x21
#define CMD_PAGE_ADDR 0x22
#define CMD_DCDC_CONTROL 0xAD
#define CMD_DCDC_ON 0x30
#define CMD_DCDC_OFF 0x00

struct oled_ssd1315
{
    i2c_master_dev_handle_t i2c_dev;
    int rst;
    bool flip_h, flip_v;
    bool use_cp;
    uint8_t contrast;
    uint8_t col_offset;
    uint8_t fb[OLED_W * OLED_H / 8];
    SemaphoreHandle_t lock;
    TaskHandle_t anim_task;
    uint16_t anim_tick_ms;

    struct
    {
        bool enabled;
        uint16_t period_ms;
        TickType_t last_toggle;
        bool on;
        int x, y;
        int bw, bh;
    } btblink;

    struct
    {
        bool enabled;
        uint16_t period_ms;
        TickType_t last_toggle;
        bool on;
        int bx, by, bw, bh;
        int fx, fy, fw, fh;
        bool use_fill;
    } chgblink;

    struct
    {
        bool enabled;
        TickType_t start_tick;
        uint16_t duration_ms;
        int x, y;
        int track_x, track_y;
        int restore_track;
    } lockimg;
};

static inline esp_err_t i2c_write_cmd(oled_t *d, const uint8_t *cmd, size_t n, int timeout_ms)
{
    uint8_t buf[1 + 32];
    esp_err_t err = ESP_OK;
    while (n)
    {
        size_t chunk = (n > 31) ? 31 : n;
        buf[0] = 0x00;
        memcpy(&buf[1], cmd, chunk);
        err = i2c_master_transmit(d->i2c_dev, buf, chunk + 1, timeout_ms);
        if (err != ESP_OK)
            return err;
        cmd += chunk;
        n -= chunk;
    }
    return err;
}

static inline esp_err_t i2c_write_data(oled_t *d, const uint8_t *data, size_t n, int timeout_ms)
{
    uint8_t buf[1 + 64];
    esp_err_t err = ESP_OK;
    while (n)
    {
        size_t chunk = (n > 64) ? 64 : n;
        buf[0] = 0x40;
        memcpy(&buf[1], data, chunk);
        err = i2c_master_transmit(d->i2c_dev, buf, chunk + 1, timeout_ms);
        if (err != ESP_OK)
            return err;
        data += chunk;
        n -= chunk;
    }
    return err;
}

static inline void clear_rect(oled_t *d, int x, int y, int w, int h)
{
    if (!d)
        return;
    for (int yy = 0; yy < h; ++yy)
        oled_draw_hline(d, x, y + yy, w, false);
}

static inline void draw_block_scaled(oled_t *d, int x, int y, int scale, bool on)
{
    for (int dy = 0; dy < scale; ++dy)
        for (int dx = 0; dx < scale; ++dx)
            oled_draw_pixel(d, x + dx, y + dy, on);
}

static esp_err_t hw_reset(const oled_t *d)
{
    if (d->rst < 0)
        return ESP_OK;
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << d->rst,
        .mode = GPIO_MODE_OUTPUT,
        .pull_down_en = 0,
        .pull_up_en = 0,
        .intr_type = GPIO_INTR_DISABLE};
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "gpio cfg");
    gpio_set_level(d->rst, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(d->rst, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    return ESP_OK;
}

static esp_err_t set_window(oled_t *d)
{
    uint8_t col0 = d->col_offset;
    uint8_t col1 = d->col_offset + (OLED_W - 1);
    const uint8_t seq[] = {
        CMD_MEMORY_MODE, 0x00,
        CMD_COLUMN_ADDR, col0, col1,
        CMD_PAGE_ADDR, 0x00, (OLED_H / 8) - 1
    };
    return i2c_write_cmd(d, seq, sizeof(seq), 20);
}

static esp_err_t init_panel(oled_t *d)
{
    ESP_RETURN_ON_ERROR(hw_reset(d), TAG, "rst");

    const uint8_t contrast = d->contrast ? d->contrast : 0xCF;

    const uint8_t seq[] = {
        CMD_DISPLAY_OFF,
        CMD_SET_DISP_CLK_DIV, 0x80,
        CMD_SET_MULTIPLEX, (uint8_t)(OLED_H - 1),
        CMD_SET_DISP_OFFSET, 0x00,
        (uint8_t)(CMD_SET_START_LINE0 | 0x00),

        (uint8_t)(d->flip_h ? CMD_SET_SEG_REMAP_1 : CMD_SET_SEG_REMAP_0),
        (uint8_t)(d->flip_v ? CMD_SET_COM_SCAN_INC : CMD_SET_COM_SCAN_DEC),

        CMD_SET_COM_PINS, 0x12,
        CMD_SET_CONTRAST, contrast,
        CMD_SET_PRECHARGE, 0xF1,
        CMD_SET_VCOM_DESEL, 0x30,
        CMD_ENTIRE_DISPLAY_ON,
        CMD_NORMAL_DISPLAY,
        CMD_DCDC_CONTROL, CMD_DCDC_ON,
        CMD_CHARGE_PUMP, (uint8_t)(d->use_cp ? 0x14 : 0x10)};

    ESP_RETURN_ON_ERROR(i2c_write_cmd(d, seq, sizeof(seq), 50), TAG, "init");
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(set_window(d), TAG, "win");
    return ESP_OK;
}

static void oled_anim_task(void *arg)
{
    oled_t *d = (oled_t *)arg;
    TickType_t delay_ticks;

    for (;;)
    {
        delay_ticks = pdMS_TO_TICKS(d->anim_tick_ms ? d->anim_tick_ms : 100);
        TickType_t now = xTaskGetTickCount();
        bool changed = false;

        /* 后台动画任务在同一把锁内完成 framebuffer 修改与发送，避免输出混合帧 */
        oled_lock(d);
        if (d->btblink.enabled &&
            (now - d->btblink.last_toggle) >= pdMS_TO_TICKS(d->btblink.period_ms))
        {
            d->btblink.last_toggle = now;
            d->btblink.on = !d->btblink.on;
            if (d->btblink.on)
                oled_draw_bt(d, d->btblink.x, d->btblink.y);
            else
                oled_clear_bt(d, d->btblink.x, d->btblink.y);
            changed = true;
        }

        //TODO: IF the battery is at 0, this won't "flash" because the image isn't changing.  We need to make sure a minimum bar is always shown.
        if (d->chgblink.enabled &&
            (now - d->chgblink.last_toggle) >= pdMS_TO_TICKS(d->chgblink.period_ms))
        {
            d->chgblink.last_toggle = now;
            d->chgblink.on = !d->chgblink.on;
            if (d->chgblink.use_fill && d->chgblink.fw > 0 && d->chgblink.fh > 0)
            {
                /* 修复闪烁残留：每次更新时先清除内部最大可能区域（对应 100% 时的区域），然后再绘制新的闪烁部分 */
                /* max_inner_w 对应 pct=100 时的宽度： w-2-2 = OLED_W-6-4 = OLED_W-10 */
                int max_inner_w = (OLED_W - 6) - 4; 
                clear_rect(d, d->chgblink.fx, d->chgblink.fy, max_inner_w, d->chgblink.fh);
                
                if (d->chgblink.on)
                    oled_fill_rect(d, d->chgblink.fx, d->chgblink.fy, d->chgblink.fw, d->chgblink.fh, true);
                // else 已经通过 clear_rect 清除，不需要再处理

                changed = true;
            }
        }

        if (d->lockimg.enabled &&
            (now - d->lockimg.start_tick) >= pdMS_TO_TICKS(d->lockimg.duration_ms))
        {
            /* 2 秒到期后由同一个动画任务负责恢复曲目号，保持与蓝牙闪烁一致的后台机制 */
            oled_clear_lock_img(d, d->lockimg.x, d->lockimg.y);
            if (d->lockimg.restore_track >= 0)
                oled_draw_track_num(d, d->lockimg.track_x, d->lockimg.track_y, d->lockimg.restore_track);
            d->lockimg.enabled = false;
            changed = true;
        }

        if (changed)
        {
            oled_update_locked(d);
        }
        oled_unlock(d);

        vTaskDelay(delay_ticks);
    }
}

esp_err_t oled_create(const oled_cfg_t *cfg, oled_t **out)
{
    if (!cfg || !out)
        return ESP_ERR_INVALID_ARG;
    oled_t *d = calloc(1, sizeof(*d));
    if (!d)
        return ESP_ERR_NO_MEM;

    d->i2c_dev = cfg->i2c_dev;
    d->rst = cfg->rst_gpio_num;
    d->flip_h = cfg->flip_h;
    d->flip_v = cfg->flip_v;
    d->use_cp = cfg->use_charge_pump;
    d->contrast = cfg->contrast ? cfg->contrast : 0xCF;
    d->col_offset = cfg->col_offset ? cfg->col_offset : 34;

    d->lock = xSemaphoreCreateMutex();
    if (!d->lock)
    {
        free(d);
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreTake(d->lock, portMAX_DELAY);
    esp_err_t err = init_panel(d);
    xSemaphoreGive(d->lock);
    if (err != ESP_OK)
    {
        vSemaphoreDelete(d->lock);
        free(d);
        return err;
    }

    memset(d->fb, 0x00, sizeof(d->fb));
    xSemaphoreTake(d->lock, portMAX_DELAY);
    err = set_window(d);
    if (err == ESP_OK)
        err = i2c_write_data(d, d->fb, sizeof(d->fb), 50);
    xSemaphoreGive(d->lock);
    if (err != ESP_OK)
    {
        vSemaphoreDelete(d->lock);
        free(d);
        return err;
    }

    d->anim_tick_ms = 100;
    if (xTaskCreate(oled_anim_task, "oled_anim", 3072, d, 5, &d->anim_task) != pdPASS)
    {
        ESP_LOGW(TAG, "anim task create failed; blink effects disabled");
        d->anim_task = NULL;
    }

    *out = d;
    return ESP_OK;
}

void oled_destroy(oled_t *d)
{
    if (!d)
        return;
    if (d->lock)
        vSemaphoreDelete(d->lock);
    if (d->anim_task)
    {
        vTaskDelete(d->anim_task);
        d->anim_task = NULL;
    }

    free(d);
}

esp_err_t oled_power_on(oled_t *d)
{
    if (!d)
        return ESP_ERR_INVALID_ARG;
    const uint8_t cmd[] = {CMD_DISPLAY_ON};
    xSemaphoreTake(d->lock, portMAX_DELAY);
    esp_err_t e = i2c_write_cmd(d, cmd, sizeof(cmd), 20);
    xSemaphoreGive(d->lock);
    return e;
}

esp_err_t oled_power_off(oled_t *d)
{
    if (!d)
        return ESP_ERR_INVALID_ARG;
    const uint8_t cmd[] = {CMD_DISPLAY_OFF};
    xSemaphoreTake(d->lock, portMAX_DELAY);
    esp_err_t e = i2c_write_cmd(d, cmd, sizeof(cmd), 20);
    xSemaphoreGive(d->lock);
    return e;
}

esp_err_t oled_set_contrast(oled_t *d, uint8_t val)
{
    if (!d)
        return ESP_ERR_INVALID_ARG;
    uint8_t cmd[] = {CMD_SET_CONTRAST, val};
    xSemaphoreTake(d->lock, portMAX_DELAY);
    esp_err_t e = i2c_write_cmd(d, cmd, sizeof(cmd), 20);
    xSemaphoreGive(d->lock);
    if (e == ESP_OK)
        d->contrast = val;
    return e;
}

esp_err_t oled_invert(oled_t *d, bool invert)
{
    if (!d)
        return ESP_ERR_INVALID_ARG;
    uint8_t cmd = invert ? CMD_INVERT_DISPLAY : CMD_NORMAL_DISPLAY;
    xSemaphoreTake(d->lock, portMAX_DELAY);
    esp_err_t e = i2c_write_cmd(d, &cmd, 1, 20);
    xSemaphoreGive(d->lock);
    return e;
}

static inline void set_fb(uint8_t fb[], int x, int y, bool on)
{
    if (x < 0 || x >= OLED_W || y < 0 || y >= OLED_H)
        return;
    int idx = (y >> 3) * OLED_W + x;
    uint8_t mask = (1u << (y & 7));
    if (on)
        fb[idx] |= mask;
    else
        fb[idx] &= (uint8_t)~mask;
}

uint8_t *oled_fb(oled_t *d) { return d ? d->fb : NULL; }

void oled_lock(oled_t *d)
{
    if (!d)
        return;

    xSemaphoreTake(d->lock, portMAX_DELAY);
}

void oled_unlock(oled_t *d)
{
    if (!d)
        return;

    xSemaphoreGive(d->lock);
}

void oled_update_locked(oled_t *d)
{
    if (!d)
        return;

    (void)set_window(d);
    (void)i2c_write_data(d, d->fb, sizeof(d->fb), 50);
}

void oled_clear(oled_t *d)
{
    if (!d)
        return;
    memset(d->fb, 0x00, sizeof(d->fb));
}

void oled_draw_pixel(oled_t *d, int x, int y, bool on)
{
    if (!d)
        return;
    set_fb(d->fb, x, y, on);
}

void oled_draw_hline(oled_t *d, int x, int y, int w, bool on)
{
    if (!d)
        return;
    for (int i = 0; i < w; i++)
        set_fb(d->fb, x + i, y, on);
}

void oled_draw_vline(oled_t *d, int x, int y, int h, bool on)
{
    if (!d)
        return;
    for (int i = 0; i < h; i++)
        set_fb(d->fb, x, y + i, on);
}

void oled_draw_rect(oled_t *d, int x, int y, int w, int h, bool on)
{
    if (!d)
        return;
    oled_draw_hline(d, x, y, w, on);
    oled_draw_hline(d, x, y + h - 1, w, on);
    oled_draw_vline(d, x, y, h, on);
    oled_draw_vline(d, x + w - 1, y, h, on);
}

void oled_fill_rect(oled_t *d, int x, int y, int w, int h, bool on)
{
    if (!d)
        return;
    for (int yy = 0; yy < h; yy++)
        oled_draw_hline(d, x, y + yy, w, on);
}

void oled_draw_char(oled_t *d, int x, int y, char c)
{
    if (!d)
        return;
    const uint8_t *g = font5x7(c);
    for (int col = 0; col < 5; col++)
    {
        uint8_t colbits = g[col];
        for (int row = 0; row < 7; row++)
        {
            bool on = (colbits >> row) & 1u;
            oled_draw_pixel(d, x + col, y + row, on);
        }
    }
}

void oled_draw_text(oled_t *d, int x, int y, const char *s)
{
    if (!d || !s)
        return;
    int cx = x;
    while (*s)
    {
        oled_draw_char(d, cx, y, *s++);
        cx += FONT_5x7.advance;
        if (cx >= OLED_W)
            break;
    }
}

void oled_clear_text(oled_t *d, int x, int y, const char *s)
{
    if (!d || !s)
        return;
    int cx = x;
    while (*s)
    {
        clear_rect(d, cx, y, FONT_5x7.width, FONT_5x7.height);
        cx += FONT_5x7.advance;
        if (cx >= OLED_W)
            break;
    }
}

void oled_draw_battery(oled_t *d, int pct, bool outline_only)
{
    int x = 0;
    int y = 0;
    int h = 6;
    int w = OLED_W - h;

    if (!d)
        return;
    if (w < 12)
        w = 12;
    if (h < 6)
        h = 6;

    int cap_w = w - 2;
    int cap_h = h - 2;
    oled_fill_rect(d, x + w - 1, y + (h / 2 - 1), (h / 2), h / 2, true);
    oled_draw_rect(d, x, y, w, h, true);

    if (!outline_only)
    {
        if (pct < 0)
            pct = 0;
        if (pct > 100)
            pct = 100;
        int inner_w = cap_w - 2;
        int fill_w = (inner_w * pct) / 100;
        if (pct == 0 && inner_w > 0)
            fill_w = 1;
        int fx = x + 2;
        int fy = y + 2;
        int fh = cap_h - 2;
        clear_rect(d, fx, fy, inner_w, fh);
        oled_fill_rect(d, fx, fy, fill_w, fh, true);

        d->chgblink.fx = fx;
        d->chgblink.fy = fy;
        d->chgblink.fw = fill_w;
        d->chgblink.fh = fh;
        d->chgblink.use_fill = (fill_w > 0 && fh > 0);
    }
}

void oled_draw_volume_bar(oled_t *d, int pct)
{
    if (!d)
        return;
    if (pct < 0)
        pct = 0;
    if (pct > 100)
        pct = 100;

    const int bar_h = 12;
    const int x0 = 0;
    const int y0 = OLED_H - bar_h;
    const int pad_lr = 1;
    const int inner_w = (OLED_W - x0) - 2 * pad_lr;
    const int step_w = 6;
    const int gap_w = 1;
    int steps = (inner_w + gap_w) / (step_w + gap_w);
    if (steps < 1)
        steps = 1;

    int active = (pct * steps + 99) / 100;
    if (active > steps)
        active = steps;

    int used_w = steps * step_w + (steps - 1) * gap_w;
    int x = x0 + pad_lr + (inner_w - used_w) / 2;

    const int min_h = 2;
    const int max_h = bar_h - 2;
    for (int i = 0; i < steps; ++i)
    {
        int h = (steps == 1) ? max_h : (min_h + (i * (max_h - min_h)) / (steps - 1));
        if (h < 1)
            h = 1;

        int sx = x + i * (step_w + gap_w);
        int sy = y0 + (bar_h - 1) - h;

        oled_draw_rect(d, sx, sy, step_w, h, true);

        if (i < active)
        {
            if (step_w > 2 && h > 2)
                oled_fill_rect(d, sx + 1, sy + 1, step_w - 2, h - 2, true);
        }
        else
        {
            if (step_w > 2 && h > 2)
                oled_fill_rect(d, sx + 1, sy + 1, step_w - 2, h - 2, false);
        }
    }
}

void oled_clear_track_num(oled_t *d, int x0, int y0) {
    const int glyph_w = FONT_5x7.advance;
    const int total_w = 3 * glyph_w;

    clear_rect(d, x0, y0, total_w, FONT_5x7.height);
}

void oled_draw_track_num(oled_t *d, int x0, int y0, int track)
{
    if (!d) return;
    if (track < 0)   track = 0;
    if (track > 999) track = 999;

    char buf[4];
    buf[0] = '0' + (track / 100) % 10;
    buf[1] = '0' + (track / 10)  % 10;
    buf[2] = '0' + (track % 10);
    buf[3] = '\0';

    const int glyph_w = FONT_5x7.advance;
    const int total_w = 3 * glyph_w;

    oled_fill_rect(d, x0, y0, total_w, FONT_5x7.height, false);

    int x = x0;
    for (int i = 0; i < 3; ++i, x += glyph_w) {
        oled_draw_char(d, x, y0, buf[i]);
    }
}

static const unsigned char lock_img_10X12[24] = // 10x12 image depth 1 从左到右
{
    0x78,0x00,0x84,0x00,0x02,0x01,0x86,0x01,0xFE,0x01,0x02,0x01,0x01,0x02,0x31,0x02,
    0x31,0x02,0x01,0x02,0x02,0x01,0xFC,0x00,
};

void oled_draw_lock_img(oled_t *d, int x, int y)
{
    if (!d)
        return;

    for (int r = 0; r < 12; r++) {
        uint16_t row = ((uint16_t)lock_img_10X12[r * 2 + 1] << 8) | lock_img_10X12[r * 2];
        for (int c = 0; c < 10; c++) {
            /* 这张图的数组定义为从左到右，因此按低位到高位依次取像素 */
            if ((row & (1u << c)) != 0) {
                oled_draw_pixel(d, x + c, y + r, true);
            }
        }
    }
}

void oled_clear_lock_img(oled_t *d, int x, int y)
{
    if (!d)
        return;

    clear_rect(d, x, y, 10, 12);
}

static const uint8_t s_bt_12x18[18 * 2] = {
    0x07,0x00,
    0x07,0x00,
    0x07,0x80,
    0x07,0x60,
    0x07,0x60,
    0x27,0x10,
    0x3F,0x10,
    0x3F,0xF0,
    0x1F,0x80,
    0x1F,0x80,
    0x1F,0x80,
    0x3F,0xF0,
    0x27,0x10,
    0x27,0x10,
    0x07,0x60,
    0x07,0x80,
    0x07,0x80,
    0x07,0x00,
};

void oled_draw_bt(oled_t *d, int x, int y)
{
    if (!d) return;
    for (int r = 0; r < 18; r++) {
        uint16_t row = ((uint16_t)s_bt_12x18[r*2] << 8) | s_bt_12x18[r*2 + 1];
        for (int c = 0; c < 12; c++) {
            bool on = (row & (1u << (15 - c))) != 0;
            if (on) oled_draw_pixel(d, x + c, y + r, true);
        }
    }
}

void oled_clear_bt(oled_t *d, int x, int y)
{
    if (!d)
        return;
    clear_rect(d, x, y, 12, 18);
}

void oled_lock_img_show_timed(oled_t *d, int img_x, int img_y, int track_x, int track_y, int restore_track, uint16_t duration_ms)
{
    if (!d || duration_ms == 0)
        return;

    /* 锁图片显示入口与蓝牙/电量共用同一把 OLED 互斥锁，避免与后台动画任务并发访问同一帧缓冲 */
    oled_lock(d);

    /* 显示图片前先清掉曲目号区域，避免数字与图片叠加 */
    oled_clear_track_num(d, track_x, track_y);
    oled_clear_lock_img(d, img_x, img_y);
    oled_draw_lock_img(d, img_x, img_y);

    d->lockimg.enabled = true;
    d->lockimg.start_tick = xTaskGetTickCount();
    d->lockimg.duration_ms = duration_ms;
    d->lockimg.x = img_x;
    d->lockimg.y = img_y;
    d->lockimg.track_x = track_x;
    d->lockimg.track_y = track_y;
    d->lockimg.restore_track = restore_track;

    oled_update_locked(d);
    oled_unlock(d);
}

void oled_lock_img_set_restore_track(oled_t *d, int track)
{
    if (!d)
        return;

    /* 恢复曲目号缓存与锁图片状态共享同一把锁，避免 2 秒恢复时读到中间态 */
    oled_lock(d);
    d->lockimg.restore_track = track;
    oled_unlock(d);
}

bool oled_lock_img_is_active(oled_t *d)
{
    bool is_active;

    if (!d)
        return false;

    /* 读取锁图片状态时也走同一把锁，保证与后台任务中的状态切换一致 */
    oled_lock(d);
    is_active = d->lockimg.enabled;
    oled_unlock(d);

    return is_active;
}

void oled_bt_blink_start(oled_t *d, int x, int y, uint16_t period_ms)
{
    if (!d || period_ms == 0)
        return;

    oled_lock(d);
    d->btblink.enabled = true;
    d->btblink.period_ms = period_ms;
    d->btblink.last_toggle = xTaskGetTickCount();
    d->btblink.on = true;
    d->btblink.x = x;
    d->btblink.y = y;
    d->btblink.bw = 8;
    d->btblink.bh = 12;

    oled_draw_bt(d, x, y);
    oled_update_locked(d);
    oled_unlock(d);
}

void oled_bt_blink_stop(oled_t *d, bool en)
{
    if (!d)
        return;
    oled_lock(d);
    if (d->btblink.enabled)
    {
        if (en)
            oled_draw_bt(d, d->btblink.x, d->btblink.y);
        else
            oled_clear_bt(d, d->btblink.x, d->btblink.y);

        oled_update_locked(d);
    }
    d->btblink.enabled = false;
    d->btblink.on = en;
    oled_unlock(d);
}

void oled_charge_blink_start(oled_t *d, uint16_t period_ms)
{
    //TODO: These should be defines as they are the same as draw battery
    int x = 0;
    int y = 0;
    int h = 6;
    int w = OLED_W - h;

    if (!d || w <= 4 || h <= 4 || period_ms == 0)
        return;
    oled_lock(d);
    d->chgblink.enabled = true;
    d->chgblink.period_ms = period_ms;
    d->chgblink.last_toggle = xTaskGetTickCount();
    d->chgblink.on = true;
    d->chgblink.bx = x;
    d->chgblink.by = y;
    d->chgblink.bw = w;
    d->chgblink.bh = h;

    oled_update_locked(d);
    oled_unlock(d);
}

void oled_charge_blink_stop(oled_t *d)
{
    if (!d)
        return;
    oled_lock(d);
    if (d->chgblink.enabled)
    {
        /* 修复闪烁停止残留空心框问题：如果停止的瞬间刚好处于空心状态，补画实心电量块 */
        if (!d->chgblink.on && d->chgblink.use_fill && d->chgblink.fw > 0 && d->chgblink.fh > 0)
        {
            oled_fill_rect(d, d->chgblink.fx, d->chgblink.fy, d->chgblink.fw, d->chgblink.fh, true);
        }
        oled_update_locked(d);
    }
    d->chgblink.enabled = false;
    oled_unlock(d);
}

void oled_anim_set_tick_ms(oled_t *d, uint16_t tick_ms)
{
    if (!d)
        return;
    d->anim_tick_ms = (tick_ms ? tick_ms : 100);
}

void oled_update(oled_t *d)
{
    if (!d)
        return;
    oled_lock(d);
    oled_update_locked(d);
    oled_unlock(d);
}
