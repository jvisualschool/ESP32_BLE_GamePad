/* ============================================================================
 *  ESP32-S3 QSPI Smart Display + BLE Gamepad  v1.0
 *  4-page LVGL UI: Welcome / Hardware / Software / Monitor
 *
 *  Hardware  : ESP32-S3, 3.5" QSPI TFT (AXS15231B, 320×480), I2C Touch
 *  Gamepad   : IINE L1161 — Xbox Wireless Controller mode (C)
 *  BLE       : HID Central, service UUID 0x1812
 *
 *  Button mapping:
 *    A  →  Brightness rotation : 20 → 40 → 60 → 80 → 100 → 20 → ...
 *    B  →  Next page           : 1 → 2 → 3 → 4 → 1 → ...
 *    X  →  (no function)
 *    Y  →  (no function)
 *
 *  Touch gesture:
 *    Swipe Left / Right  →  Next / Prev page
 *    Swipe Up / Down     →  Brightness +20% / -20%
 * ============================================================================
 */

#include <Arduino.h>
#include <lvgl.h>
#include <esp_timer.h>
#include <esp_system.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>

#include "display.h"
#include "esp_bsp.h"
#include "lv_port.h"

// ── Configuration ────────────────────────────────────────────────────────────
#define LVGL_PORT_ROTATION_DEGREE  90

// IINE L1161 (Xbox Wireless Controller mode) BLE HID report byte indices
// Report has no Report-ID prefix; axes are 16-bit LE, center = 0x8000.
// We use the HIGH byte as an 8-bit proxy (center ≈ 0x80).
#define GP_LX_IDX    1   // Left-stick  X high byte  (center 0x80)
#define GP_LY_IDX    3   // Left-stick  Y high byte  (center 0x80)
#define GP_HAT_IDX   12  // D-pad: lower nibble, 0=none 1=N 2=NE 3=E 4=SE 5=S 6=SW 7=W 8=NW
#define GP_BTN_IDX   13  // Face buttons : A=0x01  B=0x02  X=0x04  Y=0x08  LB=0x10  RB=0x20
#define GP_BTN2_IDX  14  // Extra buttons: Menu=0x01  View=0x02  L3=0x04  R3=0x08

#define HAT_NEUTRAL  0x00

#define BTN_A  0x01   // A (bottom)  → Brightness rotation
#define BTN_B  0x02   // B (right)   → Next page
#define BTN_X  0x04   // X (left)    → (no function)
#define BTN_Y  0x08   // Y (top)     → (no function)

#define DEBOUNCE_MS  300

// ── Enumerations ─────────────────────────────────────────────────────────────
enum Page { PAGE_WELCOME = 0, PAGE_HARDWARE, PAGE_SOFTWARE, PAGE_MONITOR, PAGE_COUNT };

// ── Application state ────────────────────────────────────────────────────────
static Page    current_page       = PAGE_WELCOME;
static int     target_brightness  = 100;
static int     current_brightness = 100;

// Gamepad state — written by BLE notify callback, consumed by main loop
struct GPState {
    volatile uint8_t lx    = 128;
    volatile uint8_t ly    = 128;
    volatile uint8_t hat   = HAT_NEUTRAL;
    volatile uint8_t btns  = 0;
    volatile uint8_t btns2 = 0;
    volatile uint8_t raw[17] = {};
    volatile bool    fresh = false;
};
static GPState gp;

// ── LVGL object handles ──────────────────────────────────────────────────────
static lv_obj_t *screens[PAGE_COUNT];

static lv_obj_t *lbl_welcome_ble = nullptr;   // Page 1: BLE status (blink → solid)
static bool      prev_connected  = false;

static lv_obj_t *lbl_hw_ble_stat = nullptr;   // Page 2: live BLE status row

static lv_obj_t *lbl_mon_raw   = nullptr;     // Page 4: raw bytes [0-7]
static lv_obj_t *lbl_mon_raw2  = nullptr;     // Page 4: raw bytes [8-16]
static lv_obj_t *lbl_mon_joy   = nullptr;     // Page 4: LX / LY / HAT
static lv_obj_t *lbl_mon_btns  = nullptr;     // Page 4: parsed button bytes
static lv_obj_t *lbl_mon_event = nullptr;     // Page 4: last pressed
static lv_obj_t *btn_ind[4]    = {};          // Page 4: A/B/X/Y visual boxes

static lv_obj_t *osd_overlay = nullptr;       // Brightness OSD container
static lv_obj_t *osd_segs[5] = {};            // OSD segment boxes [0]=20% … [4]=100%
static lv_obj_t *osd_pct_lbl = nullptr;       // OSD percentage label
static uint32_t  osd_hide_at = 0;             // timestamp to auto-hide OSD

// ── BLE state ─────────────────────────────────────────────────────────────────
static BLEAdvertisedDevice *targetDevice = nullptr;
static BLEClient           *pClient      = nullptr;
static bool     doConnect     = false;
static bool     connected     = false;
static bool     scanning      = false;
static uint32_t lastReconnect = 0;

// ── Forward declarations ──────────────────────────────────────────────────────
static void show_brightness_osd(int pct);
static void switch_to_page(Page p, bool forward);

// ═════════════════════════════════════════════════════════════════════════════
//  BRIGHTNESS OSD  —  vertical 5-step bar, right side, auto-hides after 1.5 s
// ═════════════════════════════════════════════════════════════════════════════

static void create_brightness_osd() {
    lv_obj_t *top = lv_layer_top();

    osd_overlay = lv_obj_create(top);
    lv_obj_set_size(osd_overlay, 78, 240);
    lv_obj_align(osd_overlay, LV_ALIGN_RIGHT_MID, -18, 0);
    lv_obj_set_style_bg_color(osd_overlay,     lv_color_hex(0x111111), 0);
    lv_obj_set_style_bg_opa(osd_overlay,       LV_OPA_80,              0);
    lv_obj_set_style_border_color(osd_overlay, lv_color_hex(0x555555), 0);
    lv_obj_set_style_border_width(osd_overlay, 1,                      0);
    lv_obj_set_style_radius(osd_overlay,       10,                     0);
    lv_obj_set_style_pad_all(osd_overlay,      6,                      0);
    lv_obj_clear_flag(osd_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(osd_overlay, LV_OBJ_FLAG_HIDDEN);

    // Sun icon
    lv_obj_t *icon = lv_label_create(osd_overlay);
    lv_label_set_text(icon, LV_SYMBOL_IMAGE);
    lv_obj_set_style_text_font(icon,  &lv_font_montserrat_16,   0);
    lv_obj_set_style_text_color(icon, lv_color_hex(0xFFCC00), 0);
    lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 2);

    // 5 segment boxes: index 4 = top (100%), index 0 = bottom (20%)
    for (int i = 4; i >= 0; i--) {
        int row = 4 - i;   // visual row 0=top … 4=bottom
        osd_segs[i] = lv_obj_create(osd_overlay);
        lv_obj_set_size(osd_segs[i], 60, 28);
        lv_obj_align(osd_segs[i], LV_ALIGN_TOP_MID, 0, 26 + row * 36);
        lv_obj_set_style_bg_color(osd_segs[i],     lv_color_hex(0x2A2A00), 0);
        lv_obj_set_style_bg_opa(osd_segs[i],       LV_OPA_COVER,           0);
        lv_obj_set_style_border_color(osd_segs[i], lv_color_hex(0x555500), 0);
        lv_obj_set_style_border_width(osd_segs[i], 1,                      0);
        lv_obj_set_style_radius(osd_segs[i],       4,                      0);
        lv_obj_clear_flag(osd_segs[i], LV_OBJ_FLAG_SCROLLABLE);

        char txt[6];
        snprintf(txt, sizeof(txt), "%d%%", (i + 1) * 20);
        lv_obj_t *seg_lbl = lv_label_create(osd_segs[i]);
        lv_label_set_text(seg_lbl, txt);
        lv_obj_set_style_text_font(seg_lbl,  &lv_font_montserrat_12,   0);
        lv_obj_set_style_text_color(seg_lbl, lv_color_hex(0x888800), 0);
        lv_obj_align(seg_lbl, LV_ALIGN_CENTER, 0, 0);
    }

    // Percentage label at bottom
    osd_pct_lbl = lv_label_create(osd_overlay);
    lv_label_set_text(osd_pct_lbl, "100%");
    lv_obj_set_style_text_font(osd_pct_lbl,  &lv_font_montserrat_14,   0);
    lv_obj_set_style_text_color(osd_pct_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(osd_pct_lbl, LV_ALIGN_BOTTOM_MID, 0, -2);
}

static void show_brightness_osd(int pct) {
    if (!osd_overlay || !osd_pct_lbl) return;

    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", pct);
    lv_label_set_text(osd_pct_lbl, buf);

    for (int i = 0; i < 5; i++) {
        if (!osd_segs[i]) continue;
        bool lit = (i + 1) * 20 <= pct;
        lv_obj_set_style_bg_color(osd_segs[i],
            lv_color_hex(lit ? 0xFFCC00 : 0x2A2A00), 0);
        lv_obj_set_style_border_color(osd_segs[i],
            lv_color_hex(lit ? 0xFFAA00 : 0x555500), 0);
        lv_obj_t *seg_lbl = lv_obj_get_child(osd_segs[i], 0);
        if (seg_lbl)
            lv_obj_set_style_text_color(seg_lbl,
                lv_color_hex(lit ? 0x000000 : 0x888800), 0);
    }

    lv_obj_clear_flag(osd_overlay, LV_OBJ_FLAG_HIDDEN);
    osd_hide_at = millis() + 1500;
}

static void poll_osd_hide() {
    if (!osd_overlay) return;
    if (osd_hide_at && millis() > osd_hide_at) {
        osd_hide_at = 0;
        bsp_display_lock(0);
        lv_obj_add_flag(osd_overlay, LV_OBJ_FLAG_HIDDEN);
        bsp_display_unlock();
    }
}

// ── Touch gesture ─────────────────────────────────────────────────────────────
static void screen_gesture_cb(lv_event_t *e) {
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;
    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    switch (dir) {
        case LV_DIR_LEFT:
            switch_to_page((Page)((current_page + 1) % PAGE_COUNT), true);
            break;
        case LV_DIR_RIGHT:
            switch_to_page((Page)((current_page + PAGE_COUNT - 1) % PAGE_COUNT), false);
            break;
        case LV_DIR_TOP:
            target_brightness = (target_brightness + 20 > 100) ? 100 : target_brightness + 20;
            show_brightness_osd(target_brightness);
            break;
        case LV_DIR_BOTTOM:
            target_brightness = (target_brightness - 20 < 20) ? 20 : target_brightness - 20;
            show_brightness_osd(target_brightness);
            break;
        default: break;
    }
}

static void set_gesture_bubble(lv_obj_t *obj) {
    lv_obj_add_flag(obj, LV_OBJ_FLAG_GESTURE_BUBBLE);
    uint32_t cnt = lv_obj_get_child_cnt(obj);
    for (uint32_t i = 0; i < cnt; i++)
        set_gesture_bubble(lv_obj_get_child(obj, i));
}

// ═════════════════════════════════════════════════════════════════════════════
//  PAGE SWITCHING
// ═════════════════════════════════════════════════════════════════════════════

static void switch_to_page(Page p, bool forward) {
    if (p == current_page) return;
    current_page = p;
    lv_scr_load_anim(screens[p],
        forward ? LV_SCR_LOAD_ANIM_MOVE_LEFT : LV_SCR_LOAD_ANIM_MOVE_RIGHT,
        200, 0, false);
    Serial.printf("[UI] Page %d\n", p + 1);
}

// ═════════════════════════════════════════════════════════════════════════════
//  BLE — HID CENTRAL
// ═════════════════════════════════════════════════════════════════════════════

void notifyCallback(BLERemoteCharacteristic *pChar, uint8_t *pData,
                    size_t length, bool isNotify) {
    if (length < 15) return;

    uint8_t cur_btns = pData[GP_BTN_IDX];
    uint8_t cur_hat  = pData[GP_HAT_IDX] & 0x0F;

    // Button-change tracking must precede the idle guard so release events
    // (btns → 0x00) are never silently dropped.
    static uint8_t prev_btns_log = 0;
    bool btns_changed = (cur_btns != prev_btns_log);

    // Idle guard — discard joystick-at-center spam when nothing is happening
    if (!btns_changed) {
        bool idle = true;
        if (pData[GP_LX_IDX] != 0x7F && pData[GP_LX_IDX] != 0x80) idle = false;
        else if (pData[GP_LY_IDX] != 0x7F && pData[GP_LY_IDX] != 0x80) idle = false;
        else if (cur_hat  != HAT_NEUTRAL) idle = false;
        else if (cur_btns != 0x00)        idle = false;
        else if (pData[GP_BTN2_IDX] != 0x00) idle = false;
        if (idle) return;
    }

    if (btns_changed) {
        for (int bit = 0; bit < 8; bit++) {
            uint8_t mask = (1u << bit);
            if ((cur_btns & mask) && !(prev_btns_log & mask))
                Serial.printf("[GP] PRESS   bit%d mask=0x%02X\n", bit, mask);
            else if (!(cur_btns & mask) && (prev_btns_log & mask))
                Serial.printf("[GP] RELEASE bit%d mask=0x%02X\n", bit, mask);
        }
        prev_btns_log = cur_btns;
    }

    static uint8_t prev_hat_log = HAT_NEUTRAL;
    if (cur_hat != prev_hat_log) {
        static const char *hat_name[] = {
            "---","N","NE","E","SE","S","SW","W","NW"
        };
        Serial.printf("[GP] HAT %s\n", cur_hat < 9 ? hat_name[cur_hat] : "?");
        prev_hat_log = cur_hat;
    }

    gp.lx    = pData[GP_LX_IDX];
    gp.ly    = pData[GP_LY_IDX];
    gp.hat   = cur_hat;
    gp.btns  = cur_btns;
    gp.btns2 = pData[GP_BTN2_IDX];
    for (int i = 0; i < 17 && i < (int)length; i++) gp.raw[i] = pData[i];
    gp.fresh = true;
}

bool enableCCCD(BLERemoteCharacteristic *pChar) {
    BLERemoteDescriptor *pDesc = pChar->getDescriptor(BLEUUID((uint16_t)0x2902));
    if (!pDesc) return false;
    uint8_t on[] = {0x01, 0x00};
    pDesc->writeValue(on, 2, true);
    Serial.println("[BLE]   CCCD enabled");
    return true;
}

class ClientCallbacks : public BLEClientCallbacks {
    void onConnect(BLEClient *) override    { Serial.println("[BLE] Connected"); }
    void onDisconnect(BLEClient *) override { Serial.println("[BLE] Disconnected"); connected = false; }
};

class AdvertisedDeviceCallbacks : public BLEAdvertisedDeviceCallbacks {
    void onResult(BLEAdvertisedDevice dev) override {
        if (!dev.haveName()) return;
        if (dev.getName() == "Xbox Wireless Controller") {
            Serial.printf("[BLE] Found target (RSSI %d)\n", dev.getRSSI());
            BLEDevice::getScan()->stop();
            scanning = false;
            if (targetDevice) delete targetDevice;
            targetDevice = new BLEAdvertisedDevice(dev);
            doConnect = true;
        }
    }
};

void connectToServer() {
    Serial.println("[BLE] Connecting...");
    BLESecurity *pSec = new BLESecurity();
    pSec->setAuthenticationMode(ESP_LE_AUTH_BOND);
    pSec->setCapability(ESP_IO_CAP_NONE);
    pSec->setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
    pSec->setRespEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);

    pClient = BLEDevice::createClient();
    pClient->setClientCallbacks(new ClientCallbacks());
    if (!pClient->connect(targetDevice)) {
        Serial.println("[BLE] Connection FAILED");
        connected = false;
        delete pSec;
        return;
    }
    connected = true;
    delay(1000); // wait for service discovery to stabilize

    BLERemoteService *pSvc = pClient->getService(BLEUUID((uint16_t)0x1812));
    if (!pSvc) { Serial.println("[BLE] HID service not found"); return; }

    int subscribed = 0;
    auto *chars = pSvc->getCharacteristics();
    if (chars) {
        for (auto &pair : *chars) {
            BLERemoteCharacteristic *pChar = pair.second;
            if (pChar->canNotify()) {
                pChar->registerForNotify(notifyCallback);
                delay(100);
                enableCCCD(pChar);
                subscribed++;
            }
        }
    }
    Serial.printf("[BLE] Subscribed to %d characteristic(s)\n", subscribed);
    delete pSec;
}

void startBLEScan() {
    if (scanning) return;
    scanning = true;
    BLEScan *pScan = BLEDevice::getScan();
    pScan->clearResults();
    pScan->start(10, false);
    Serial.println("[BLE] Scanning...");
}

void processBLE() {
    if (doConnect) { doConnect = false; connectToServer(); return; }
    if (!connected && !doConnect && !scanning) {
        if (millis() - lastReconnect > 5000) {
            lastReconnect = millis();
            startBLEScan();
        }
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  GAMEPAD INPUT → UI ACTIONS  (called from loop() with LVGL lock held)
// ═════════════════════════════════════════════════════════════════════════════

static void processGamepadInput() {
    if (!gp.fresh) return;
    gp.fresh = false;

    const uint8_t btns = gp.btns;
    static uint8_t  prev_btns = 0;
    static uint32_t t_A = 0, t_B = 0;
    uint32_t now = millis();

    // A → Brightness rotation: 20 → 40 → 60 → 80 → 100 → 20 → ...
    if ((btns & BTN_A) && !(prev_btns & BTN_A) && (now - t_A > DEBOUNCE_MS)) {
        target_brightness = (target_brightness >= 100) ? 20 : target_brightness + 20;
        show_brightness_osd(target_brightness);
        t_A = now;
        Serial.printf("[MAP] A → brightness %d%%\n", target_brightness);
    }

    // B → Next page: 1 → 2 → 3 → 4 → 1 → ...
    if ((btns & BTN_B) && !(prev_btns & BTN_B) && (now - t_B > DEBOUNCE_MS)) {
        switch_to_page((Page)((current_page + 1) % PAGE_COUNT), true);
        t_B = now;
    }

    // Update Hardware page BLE status
    if (lbl_hw_ble_stat)
        lv_label_set_text(lbl_hw_ble_stat, connected
            ? LV_SYMBOL_OK " IINE L1161  Connected"
            : LV_SYMBOL_CLOSE " IINE L1161  Searching...");

    // Update Monitor page (page 4 only)
    if (current_page == PAGE_MONITOR) {
        char buf[80];

        if (lbl_mon_raw) {
            snprintf(buf, sizeof(buf), "[0-7]:  %02X %02X %02X %02X  %02X %02X %02X %02X",
                     gp.raw[0], gp.raw[1], gp.raw[2], gp.raw[3],
                     gp.raw[4], gp.raw[5], gp.raw[6], gp.raw[7]);
            lv_label_set_text(lbl_mon_raw, buf);
        }
        if (lbl_mon_raw2) {
            snprintf(buf, sizeof(buf), "[8-16]: %02X %02X %02X %02X  %02X %02X %02X %02X %02X",
                     gp.raw[8],  gp.raw[9],  gp.raw[10], gp.raw[11],
                     gp.raw[12], gp.raw[13], gp.raw[14], gp.raw[15], gp.raw[16]);
            lv_label_set_text(lbl_mon_raw2, buf);
        }

        if (lbl_mon_joy) {
            static const char *hat_name[] = {
                "---","N","NE","E","SE","S","SW","W","NW",
                "?9","?A","?B","?C","?D","?E","?F"
            };
            snprintf(buf, sizeof(buf), "LX=%3d  LY=%3d   HAT: %s",
                     gp.lx, gp.ly, hat_name[gp.hat & 0x0F]);
            lv_label_set_text(lbl_mon_joy, buf);
        }

        // A/B/X/Y visual button indicators
        static const uint32_t btn_on[] = { 0xFF4444, 0x44FF44, 0x4499FF, 0xFFFF44 };
        const uint8_t masks[] = { BTN_A, BTN_B, BTN_X, BTN_Y };
        for (int i = 0; i < 4; i++) {
            if (!btn_ind[i]) continue;
            bool pressed = (btns & masks[i]) != 0;
            lv_obj_set_style_bg_color(btn_ind[i],
                lv_color_hex(pressed ? btn_on[i] : 0x001500), 0);
            lv_obj_t *lbl = lv_obj_get_child(btn_ind[i], 0);
            if (lbl)
                lv_obj_set_style_text_color(lbl,
                    lv_color_hex(pressed ? 0x000000 : btn_on[i]), 0);
        }

        // Last pressed event (sticky)
        static char last_evt[48] = "Last: --";
        uint8_t newly = btns & ~prev_btns;
        if (newly) {
            static const char *names[] = { "A", "B", "X", "Y" };
            for (int i = 0; i < 4; i++) {
                if (newly & masks[i])
                    snprintf(last_evt, sizeof(last_evt),
                             "Last: %s   raw=0x%02X", names[i], btns);
            }
        }
        if (lbl_mon_event) lv_label_set_text(lbl_mon_event, last_evt);

        if (lbl_mon_btns) {
            snprintf(buf, sizeof(buf), "byte[12]=0x%02X  byte[13]=0x%02X  byte[14]=0x%02X",
                     gp.raw[12], gp.btns, gp.btns2);
            lv_label_set_text(lbl_mon_btns, buf);
        }
    }

    prev_btns = btns;
}

// ═════════════════════════════════════════════════════════════════════════════
//  UI HELPERS
// ═════════════════════════════════════════════════════════════════════════════

static lv_obj_t *make_label(lv_obj_t *parent, const char *text,
                             const lv_font_t *font, lv_color_t color,
                             lv_align_t align, lv_coord_t x, lv_coord_t y) {
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_style_text_color(lbl, color, 0);
    lv_obj_align(lbl, align, x, y);
    return lbl;
}

static lv_obj_t *make_hline(lv_obj_t *parent, lv_coord_t y, lv_color_t col) {
    static lv_point_t pts[] = {{0, 0}, {440, 0}};
    lv_obj_t *ln = lv_line_create(parent);
    lv_line_set_points(ln, pts, 2);
    lv_obj_set_style_line_width(ln, 1, 0);
    lv_obj_set_style_line_color(ln, col, 0);
    lv_obj_align(ln, LV_ALIGN_TOP_MID, 0, y);
    return ln;
}

static lv_obj_t *make_info_row(lv_obj_t *parent, const char *label,
                                const char *value, lv_coord_t y,
                                lv_color_t col_label, lv_color_t col_value) {
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, 440, 34);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_set_style_bg_opa(row,     LV_OPA_0, 0);
    lv_obj_set_style_border_opa(row, LV_OPA_0, 0);
    lv_obj_set_style_pad_all(row,    0,         0);

    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_font(lbl,  &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl, col_label,               0);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 4, 0);

    lv_obj_t *val = lv_label_create(row);
    lv_label_set_text(val, value);
    lv_obj_set_style_text_font(val,  &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(val, col_value,               0);
    lv_obj_align(val, LV_ALIGN_RIGHT_MID, -4, 0);

    return row;
}

// ═════════════════════════════════════════════════════════════════════════════
//  PAGE 1 — WELCOME
// ═════════════════════════════════════════════════════════════════════════════

static void create_welcome_screen() {
    lv_obj_t *scr = screens[PAGE_WELCOME];
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(scr,   LV_OPA_COVER,           0);

    lv_obj_t *box = lv_obj_create(scr);
    lv_obj_set_size(box, 440, 270);
    lv_obj_align(box, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(box,     lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_color(box, lv_color_hex(0x00FF44), 0);
    lv_obj_set_style_border_width(box, 3, 0);
    lv_obj_set_style_radius(box,       0, 0);
    lv_obj_set_style_pad_all(box,      0, 0);

    lv_obj_t *hdr = lv_obj_create(scr);
    lv_obj_set_size(hdr, 440, 42);
    lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, 60);
    lv_obj_set_style_bg_color(hdr,   lv_color_hex(0x00FF44), 0);
    lv_obj_set_style_bg_opa(hdr,     LV_OPA_COVER,           0);
    lv_obj_set_style_border_opa(hdr, LV_OPA_0,               0);
    lv_obj_set_style_radius(hdr,     0,                       0);
    lv_obj_set_style_pad_all(hdr,    0,                       0);

    lv_obj_t *hdr_lbl = lv_label_create(hdr);
    lv_label_set_text(hdr_lbl, ">> RETRO DEV ARCADE <<");
    lv_obj_set_style_text_font(hdr_lbl,  &lv_font_montserrat_20,   0);
    lv_obj_set_style_text_color(hdr_lbl, lv_color_hex(0x000000), 0);
    lv_obj_align(hdr_lbl, LV_ALIGN_CENTER, 0, 0);

    make_label(scr, "WELCOME,  PLAYER  ONE!", &lv_font_montserrat_22,
               lv_color_hex(0x00FF44), LV_ALIGN_TOP_MID, 0, 115);
    make_label(scr, "ESP32-S3,  QSPI & BLE Edition", &lv_font_montserrat_16,
               lv_color_hex(0xFFD700), LV_ALIGN_TOP_MID, 0, 150);
    make_label(scr, "- - - - - - - - - - - - - - - - - - - -",
               &lv_font_montserrat_14, lv_color_hex(0x336633),
               LV_ALIGN_TOP_MID, 0, 178);

    lbl_welcome_ble = make_label(scr, "[ Gamepad Waiting... ]", &lv_font_montserrat_28,
                                 lv_color_hex(0xFF2222), LV_ALIGN_TOP_MID, 0, 200);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, lbl_welcome_ble);
    lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_time(&a, 600);
    lv_anim_set_playback_time(&a, 600);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_exec_cb(&a, [](void *obj, int32_t v) {
        lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
    });
    lv_anim_start(&a);

    make_label(scr, LV_SYMBOL_BULLET " . . .", &lv_font_montserrat_14,
               lv_color_hex(0x555555), LV_ALIGN_BOTTOM_MID, 0, -36);
}

// ═════════════════════════════════════════════════════════════════════════════
//  PAGE 2 — HARDWARE
// ═════════════════════════════════════════════════════════════════════════════

static void create_hardware_screen() {
    lv_obj_t *scr = screens[PAGE_HARDWARE];
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x050D1A), 0);
    lv_obj_set_style_bg_opa(scr,   LV_OPA_COVER,           0);

    lv_color_t c_title = lv_color_hex(0x00BFFF);
    lv_color_t c_key   = lv_color_hex(0x7ECFFF);
    lv_color_t c_val   = lv_color_hex(0xCCEEFF);
    lv_color_t c_line  = lv_color_hex(0x1A3A5A);

    make_label(scr, LV_SYMBOL_SETTINGS "  HARDWARE  STATUS",
               &lv_font_montserrat_22, c_title, LV_ALIGN_TOP_MID, 0, 12);
    make_hline(scr, 46, c_line);

    const struct { const char *k; const char *v; } rows[] = {
        { LV_SYMBOL_PLAY  "  MCU",     "ESP32-S3  Dual LX7 @240 MHz"   },
        { LV_SYMBOL_IMAGE "  FLASH",   "16 MB SPI Flash"                },
        { LV_SYMBOL_SAVE  "  PSRAM",   "8 MB OPI (Octal SPI)"           },
        { LV_SYMBOL_WIFI  "  RADIO",   "Wi-Fi 802.11 b/g/n + BT 5.0"   },
        { LV_SYMBOL_IMAGE "  DISPLAY", "3.5\"  320x480  QSPI AXS15231B" },
        { LV_SYMBOL_IMAGE "  TOUCH",   "I2C  Capacitive  GPIO4/8"       },
    };

    int y = 56;
    for (auto &r : rows) { make_info_row(scr, r.k, r.v, y, c_key, c_val); y += 36; }

    make_hline(scr, y + 6, c_line);
    lbl_hw_ble_stat = make_label(scr, LV_SYMBOL_CLOSE " IINE L1161  Searching...",
                                 &lv_font_montserrat_16, lv_color_hex(0xFF6633),
                                 LV_ALIGN_TOP_MID, 0, y + 16);

    make_label(scr, ". " LV_SYMBOL_BULLET " . .", &lv_font_montserrat_14,
               lv_color_hex(0x555555), LV_ALIGN_BOTTOM_MID, 0, -14);
}

// ═════════════════════════════════════════════════════════════════════════════
//  PAGE 3 — SOFTWARE
// ═════════════════════════════════════════════════════════════════════════════

static void create_software_screen() {
    lv_obj_t *scr = screens[PAGE_SOFTWARE];
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0F0800), 0);
    lv_obj_set_style_bg_opa(scr,   LV_OPA_COVER,           0);

    lv_color_t c_title = lv_color_hex(0xFFAA00);
    lv_color_t c_key   = lv_color_hex(0xFFCC66);
    lv_color_t c_val   = lv_color_hex(0xFFEECC);
    lv_color_t c_line  = lv_color_hex(0x3A2800);

    make_label(scr, LV_SYMBOL_LIST "  SOFTWARE  STACK",
               &lv_font_montserrat_22, c_title, LV_ALIGN_TOP_MID, 0, 12);
    make_hline(scr, 46, c_line);

    const struct { const char *k; const char *v; } rows[] = {
        { LV_SYMBOL_PLAY   "  FRAMEWORK",  "Arduino (ESP32 Core v3.3.6)"      },
        { LV_SYMBOL_PLAY   "  ESP-IDF",    "v5.1"                              },
        { LV_SYMBOL_IMAGE  "  LVGL",       "v8.3.9"                            },
        { LV_SYMBOL_WIFI   "  BLE",        "HID Central  IINE L1161 (Xbox)"   },
        { LV_SYMBOL_CHARGE "  PARTITION",  "app 3MB / FAT 9MB / total 16MB"   },
        { LV_SYMBOL_OK     "  STATUS",     "RUNNING  v1.0"                     },
    };

    int y = 56;
    for (auto &r : rows) { make_info_row(scr, r.k, r.v, y, c_key, c_val); y += 36; }

    make_hline(scr, y + 6, c_line);
    lv_obj_t *fqbn = lv_label_create(scr);
    lv_label_set_text(fqbn, "FQBN: esp32:esp32:esp32s3  CDCOnBoot=cdc  16M  opi");
    lv_obj_set_style_text_font(fqbn,  &lv_font_montserrat_12,   0);
    lv_obj_set_style_text_color(fqbn, lv_color_hex(0x886633), 0);
    lv_obj_align(fqbn, LV_ALIGN_TOP_MID, 0, y + 16);

    make_label(scr, ". . " LV_SYMBOL_BULLET " .", &lv_font_montserrat_14,
               lv_color_hex(0x555555), LV_ALIGN_BOTTOM_MID, 0, -14);
}

// ═════════════════════════════════════════════════════════════════════════════
//  PAGE 4 — INPUT MONITOR
// ═════════════════════════════════════════════════════════════════════════════

static void create_monitor_screen() {
    lv_obj_t *scr = screens[PAGE_MONITOR];
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000F00), 0);
    lv_obj_set_style_bg_opa(scr,   LV_OPA_COVER,           0);

    lv_color_t c_title = lv_color_hex(0x00FF00);
    lv_color_t c_label = lv_color_hex(0x88FF88);
    lv_color_t c_val   = lv_color_hex(0xFFFFFF);
    lv_color_t c_line  = lv_color_hex(0x004400);

    make_label(scr, LV_SYMBOL_KEYBOARD "  INPUT MONITOR",
               &lv_font_montserrat_22, c_title, LV_ALIGN_TOP_MID, 0, 12);
    make_hline(scr, 44, c_line);

    // RAW HID bytes
    make_label(scr, "RAW BYTES:", &lv_font_montserrat_12,
               c_label, LV_ALIGN_TOP_LEFT, 20, 52);
    lbl_mon_raw = make_label(scr, "[0-7]:  00 00 00 00  00 00 00 00",
                             &lv_font_montserrat_14, c_val, LV_ALIGN_TOP_LEFT, 20, 66);
    lbl_mon_raw2 = make_label(scr, "[8-16]: 00 00 00 00  00 00 00 00 00",
                              &lv_font_montserrat_14, c_val, LV_ALIGN_TOP_LEFT, 20, 84);
    make_hline(scr, 102, c_line);

    // Joystick + D-pad
    lbl_mon_joy = make_label(scr, "LX=128  LY=128   HAT: ---",
                             &lv_font_montserrat_16, c_val, LV_ALIGN_TOP_LEFT, 20, 110);
    make_hline(scr, 134, c_line);

    // A / B / X / Y visual button indicators
    static const uint32_t btn_colors[] = { 0xFF4444, 0x44FF44, 0x4499FF, 0xFFFF44 };
    static const char    *btn_names[]  = { "A", "B", "X", "Y" };
    const int bw = 58, bh = 54, gap = 14;
    const int bx_start = (480 - (4 * bw + 3 * gap)) / 2;

    for (int i = 0; i < 4; i++) {
        btn_ind[i] = lv_obj_create(scr);
        lv_obj_set_size(btn_ind[i], bw, bh);
        lv_obj_align(btn_ind[i], LV_ALIGN_TOP_LEFT, bx_start + i * (bw + gap), 142);
        lv_obj_set_style_bg_color(btn_ind[i],     lv_color_hex(0x001500),       0);
        lv_obj_set_style_bg_opa(btn_ind[i],       LV_OPA_COVER,                 0);
        lv_obj_set_style_border_color(btn_ind[i], lv_color_hex(btn_colors[i]),  0);
        lv_obj_set_style_border_width(btn_ind[i], 2,                             0);
        lv_obj_set_style_radius(btn_ind[i],       8,                             0);
        lv_obj_clear_flag(btn_ind[i], LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *lbl = lv_label_create(btn_ind[i]);
        lv_label_set_text(lbl, btn_names[i]);
        lv_obj_set_style_text_font(lbl,  &lv_font_montserrat_22,   0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(btn_colors[i]), 0);
        lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);
    }
    make_hline(scr, 206, c_line);

    // Last event + button byte values
    lbl_mon_event = make_label(scr, "Last: --",
                               &lv_font_montserrat_16, lv_color_hex(0xAAFFAA),
                               LV_ALIGN_TOP_LEFT, 20, 214);
    lbl_mon_btns  = make_label(scr, "byte[12]=0x00  byte[13]=0x00  byte[14]=0x00",
                               &lv_font_montserrat_12, c_label,
                               LV_ALIGN_TOP_LEFT, 20, 238);

    make_label(scr, "A: Brightness  20/40/60/80/100%     B: Next Page",
               &lv_font_montserrat_12, lv_color_hex(0x447744),
               LV_ALIGN_BOTTOM_MID, 0, -40);
    make_label(scr, ". . . " LV_SYMBOL_BULLET, &lv_font_montserrat_14,
               lv_color_hex(0x555555), LV_ALIGN_BOTTOM_MID, 0, -14);
}

// ═════════════════════════════════════════════════════════════════════════════
//  WELCOME PAGE — BLE STATUS UPDATE
// ═════════════════════════════════════════════════════════════════════════════

static void start_ble_blink() {
    if (!lbl_welcome_ble) return;
    lv_anim_del(lbl_welcome_ble, NULL);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, lbl_welcome_ble);
    lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_time(&a, 600);
    lv_anim_set_playback_time(&a, 600);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_exec_cb(&a, [](void *obj, int32_t v) {
        lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
    });
    lv_anim_start(&a);
}

static void update_welcome_ble_label(bool is_connected) {
    if (!lbl_welcome_ble) return;
    if (is_connected) {
        lv_anim_del(lbl_welcome_ble, NULL);
        lv_obj_set_style_opa(lbl_welcome_ble, LV_OPA_COVER, 0);
        lv_label_set_text(lbl_welcome_ble, "[ CONNECTED !!! ]");
        lv_obj_set_style_text_color(lbl_welcome_ble, lv_color_hex(0x00FF44), 0);
    } else {
        lv_label_set_text(lbl_welcome_ble, "[ Gamepad Waiting... ]");
        lv_obj_set_style_text_color(lbl_welcome_ble, lv_color_hex(0xFF2222), 0);
        start_ble_blink();
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  SETUP
// ═════════════════════════════════════════════════════════════════════════════

void setup() {
    Serial.begin(115200);
    delay(800);
    Serial.println("\n=== ESP32-S3 QSPI BLE GamePad v1.0 ===");

    bsp_display_cfg_t cfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size   = EXAMPLE_LCD_QSPI_H_RES * EXAMPLE_LCD_QSPI_V_RES,
#if   LVGL_PORT_ROTATION_DEGREE == 90
        .rotate = LV_DISP_ROT_90,
#elif LVGL_PORT_ROTATION_DEGREE == 270
        .rotate = LV_DISP_ROT_270,
#elif LVGL_PORT_ROTATION_DEGREE == 180
        .rotate = LV_DISP_ROT_180,
#else
        .rotate = LV_DISP_ROT_NONE,
#endif
    };
    bsp_display_start_with_config(&cfg);
    bsp_display_backlight_on();

    BLEDevice::init("");
    BLEScan *pScan = BLEDevice::getScan();
    pScan->setAdvertisedDeviceCallbacks(new AdvertisedDeviceCallbacks());
    pScan->setInterval(45);
    pScan->setWindow(15);
    pScan->setActiveScan(true);
    pScan->start(0, nullptr, false);

    bsp_display_lock(0);

    for (int i = 0; i < PAGE_COUNT; i++) {
        screens[i] = lv_obj_create(NULL);
        lv_obj_set_style_bg_opa(screens[i], LV_OPA_COVER, 0);
    }
    create_welcome_screen();
    create_hardware_screen();
    create_software_screen();
    create_monitor_screen();

    for (int i = 0; i < PAGE_COUNT; i++) {
        lv_obj_add_flag(screens[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(screens[i], screen_gesture_cb, LV_EVENT_GESTURE, NULL);
        uint32_t cnt = lv_obj_get_child_cnt(screens[i]);
        for (uint32_t c = 0; c < cnt; c++)
            set_gesture_bubble(lv_obj_get_child(screens[i], c));
    }

    create_brightness_osd();
    lv_disp_load_scr(screens[PAGE_WELCOME]);

    bsp_display_unlock();
    Serial.println("=== UI Ready ===");
}

// ═════════════════════════════════════════════════════════════════════════════
//  LOOP
// ═════════════════════════════════════════════════════════════════════════════

void loop() {
    processBLE();

    if (connected != prev_connected) {
        prev_connected = connected;
        bsp_display_lock(0);
        update_welcome_ble_label(connected);
        bsp_display_unlock();
    }

    if (gp.fresh) {
        bsp_display_lock(0);
        processGamepadInput();
        bsp_display_unlock();
    }

    poll_osd_hide();

    // Smooth backlight transition: 1% per 10 ms
    if (current_brightness != target_brightness) {
        current_brightness += (current_brightness < target_brightness) ? 1 : -1;
        bsp_display_brightness_set(current_brightness);
    }

    delay(10);
}
