#include <Arduino.h>
#include "FS.h"
#include "SD_MMC.h"
#include "SPI.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "es8311.h"
#include "Audio.h"
#include "demo_music.h"
#include <lvgl.h>
#include <TFT_eSPI.h>
#include "touch.h"

// --- Hardware Pins & Co ---
#define I2C_NUM         I2C_NUM_0
#define I2C_SDA         GPIO_NUM_16
#define I2C_SCL         GPIO_NUM_15
#define I2C_SPEED       400000

#define EXAMPLE_SAMPLE_RATE     44100
#define EXAMPLE_MCLK_MULTIPLE   I2S_MCLK_MULTIPLE_256
#define EXAMPLE_MCLK_FREQ_HZ    (EXAMPLE_SAMPLE_RATE * EXAMPLE_MCLK_MULTIPLE)
#define VOLUME_NUM              80

#define SD_SCK          GPIO_NUM_38
#define SD_CMD          GPIO_NUM_40
#define SD_D0           GPIO_NUM_39
#define SD_D1           GPIO_NUM_41
#define SD_D2           GPIO_NUM_48
#define SD_D3           GPIO_NUM_47

#define I2S_BCK         GPIO_NUM_5
#define I2S_WS          GPIO_NUM_7
#define I2S_DOUT        GPIO_NUM_8
#define I2S_MCK         GPIO_NUM_4
#define AP_ENABLE       GPIO_NUM_1

#define SCREEN_WIDTH    320
#define SCREEN_HEIGHT   240

Audio audio;
i2c_master_bus_handle_t bus_handle;
i2c_master_dev_handle_t es8311_handle;
TFT_eSPI tft = TFT_eSPI();

// --- Playlist-Speicher ---
int aktuellerTitel = 0;
int gesamtTitel = 0;
String songListe[20]; 
volatile bool liedFertig = false; 
bool isPlaying = false; 
bool istInitialisiert = false;

// --- EQUALIZER CONFIGURATION ---
#define EQ_BAR_COUNT      10   
static lv_obj_t * eq_bars[EQ_BAR_COUNT];
static lv_timer_t * eq_timer = NULL;
static volatile uint8_t eq_band_values[EQ_BAR_COUNT] = {0};

// --- LVGL Puffer & Widgets ---
static lv_disp_draw_buf_t draw_buf;
static lv_style_t style_screen_bg;
static lv_style_t style_btn_container;
static lv_style_t style_premium_btn;

LV_IMG_DECLARE(bg320x240); 
// Das LVGL-Objekt für den Hintergrund
lv_obj_t * img_bg = NULL;

lv_obj_t * label_status = NULL;
lv_obj_t * label_titel = NULL;
lv_obj_t * btn_play_lbl = NULL;

// --- Callback für das Titelende ---
void audio_eof_mp3(const char *info) {
    Serial.printf("Titel beendet: %s\n", info);
    isPlaying = false;
    liedFertig = true; 
}
void audio_eof_file(const char *info){
    Serial.printf("EOF File erreicht: %s\n", info);
    isPlaying = false;
    liedFertig = true;
}

// --- Audio Task mit mathematischer Equalizer-Generierung ---
void audioTask(void *parameter) {
  while (true) {
    audio.loop();

    if (isPlaying) {
      // VU-Level holen (0 - 127 Basisenergie)
      uint16_t vu = audio.getVUlevel();
      uint8_t left_channel = (vu >> 8) & 0xFF;  
      uint8_t right_channel = vu & 0xFF;        
      uint8_t raw_energy = (left_channel + right_channel) / 2; 

      // Verteilung der Frequenzen auf die 10 Bars simulieren
      for (int b = 0; b < EQ_BAR_COUNT; b++) {
          int32_t band_target = 0;

          if (b < 3) { // BASS: reagiert stark auf Impulse
              band_target = (raw_energy * 0.25f);
              if (band_target > 35) band_target = 35;
              eq_band_values[b] = (eq_band_values[b] * 3 + band_target * 7) / 10;
          } 
          else if (b < 7) { // MITTEN: Trägerer Verlauf mit Phasenverschiebung
              band_target = (raw_energy * 0.2f) + 1;
              if (band_target > 35) band_target = 35;
              eq_band_values[b] = (eq_band_values[b] * 6 + band_target * 4) / 10;
          } 
          else { // HÖHEN: Reagiert zittrig und schnell
              band_target = (raw_energy * 0.25f) + 1; // Sanfter, kleinerer Faktor
              if (band_target > 35) band_target = 35;
              eq_band_values[b] = (eq_band_values[b] * 8 + band_target * 2) / 10;
          }
      }
    } else {
      // Wenn Pause/Stop: Bars langsam auf Ruhezustand (Wert 2) absenken
      for (int b = 0; b < EQ_BAR_COUNT; b++) {
          if (eq_band_values[b] > 2) eq_band_values[b]--;
      }
    }

    vTaskDelay(pdMS_TO_TICKS(2)); 
  }
}



void audioInit() {
  xTaskCreatePinnedToCore(audioTask, "audioplay", 8192, NULL, 5, NULL, 1);
}

// --- I2C & Codec ---
esp_err_t I2C_init(void) {
  i2c_master_bus_config_t touch_i2c_cfg = {
      .i2c_port = I2C_NUM,
      .sda_io_num = I2C_SDA,
      .scl_io_num = I2C_SCL,
      .clk_source = I2C_CLK_SRC_DEFAULT,
      .glitch_ignore_cnt = 7,
      .flags = { .enable_internal_pullup = true }
  };
  esp_err_t err = i2c_new_master_bus(&touch_i2c_cfg, &bus_handle);
  if(err != ESP_OK) return err;

  i2c_device_config_t dev_config = {
      .dev_addr_length = I2C_ADDR_BIT_LEN_7,
      .device_address = ES8311_ADDRRES_0,
      .scl_speed_hz = I2C_SPEED,
  };
  return i2c_master_bus_add_device(bus_handle, &dev_config, &es8311_handle);
}

esp_err_t es8311_codec_init(void) {
  const es8311_clock_config_t es_clk = {
      .mclk_inverted = false, .sclk_inverted = false,
      .mclk_from_mclk_pin = true, .mclk_frequency = EXAMPLE_MCLK_FREQ_HZ,
      .sample_frequency = EXAMPLE_SAMPLE_RATE
  };
  esp_err_t err = es8311_init(es8311_handle, &es_clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16);
  if(err != ESP_OK) return err;
  err = es8311_sample_frequency_config(es8311_handle, EXAMPLE_SAMPLE_RATE * EXAMPLE_MCLK_MULTIPLE, EXAMPLE_SAMPLE_RATE);
  if(err != ESP_OK) return err;
  err = es8311_voice_volume_set(es8311_handle, VOLUME_NUM, NULL);
  if(err != ESP_OK) return err;
  return es8311_microphone_config(es8311_handle, false);
}

// --- SD-Karte durchsuchen ---
void ladePlaylist() {
  File root = SD_MMC.open("/");
  if (!root || !root.isDirectory()) return;

  File file = root.openNextFile();
  while (file && gesamtTitel < 20) {
    if (!file.isDirectory()) {
      String name = file.name();
      if (name.endsWith(".mp3") || name.endsWith(".MP3")) {
        songListe[gesamtTitel] = name;
        gesamtTitel++;
      }
    }
    file = root.openNextFile();
  }
  root.close();
  Serial.printf("%d MP3-Titel geladen.\n", gesamtTitel);
}

// --- LVGL Display & Touch Callbacks ---
void my_disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p) {
    uint32_t w = (area->x2 - area->x1 + 1);
    uint32_t h = (area->y2 - area->y1 + 1);
    tft.startWrite();
    tft.setAddrWindow(area->x1, area->y1, w, h);
    tft.pushColors((uint16_t *)&color_p->full, w * h, true);
    tft.endWrite();
    lv_disp_flush_ready(disp);
}

void my_touchpad_read(lv_indev_drv_t * indev_driver, lv_indev_data_t * data) {
    if(touch_touched()) {
        data->state = LV_INDEV_STATE_PR;
        data->point.x = map(touch_last_x, 0, 240, 0, SCREEN_WIDTH - 1); 
        data->point.y = map((320 - 1) - touch_last_y, 0, 320, 0, SCREEN_HEIGHT - 1); 
    } else {
        data->state = LV_INDEV_STATE_REL;
    }
}

// --- UI Aktualisierung ---
void update_ui() {
    if (label_status == NULL || label_titel == NULL || btn_play_lbl == NULL) return;

    char buf_status[32];
    if (gesamtTitel == 0) {
        snprintf(buf_status, sizeof(buf_status), "Keine Titel");
        lv_label_set_text(label_status, buf_status);
        lv_label_set_text(label_titel, "Keine MP3s auf SD gefunden");
        lv_label_set_text(btn_play_lbl, LV_SYMBOL_PLAY " Play");
        return;
    }

    snprintf(buf_status, sizeof(buf_status), "Titel %d von %d", aktuellerTitel + 1, gesamtTitel);
    lv_label_set_text(label_status, buf_status);
    lv_label_set_text(label_titel, songListe[aktuellerTitel].c_str());
    lv_label_set_text(btn_play_lbl, isPlaying ? LV_SYMBOL_PAUSE " Pause" : LV_SYMBOL_PLAY " Play");
}

// --- Button Event Handler ---
static void btn_event_cb(lv_event_t * e) {
    uintptr_t id = (uintptr_t)lv_event_get_user_data(e);
    if (gesamtTitel == 0) return;

    if(id == 3) { // >> VORWÄRTS
        aktuellerTitel++;
        if(aktuellerTitel >= gesamtTitel) aktuellerTitel = 0;
        
        demo_music_play(aktuellerTitel);
        isPlaying = true;
        update_ui();
    } 
    else if(id == 1) { // << ZURÜCK
        aktuellerTitel--;
        if(aktuellerTitel < 0) aktuellerTitel = gesamtTitel - 1;
        
        demo_music_play(aktuellerTitel);
        isPlaying = true;
        update_ui();
    } 
    else if(id == 2) { // PLAY / PAUSE
        isPlaying = !isPlaying;
        
        if (!istInitialisiert) {
            audio.setVolume(4); 
            demo_music_play(aktuellerTitel); 
            istInitialisiert = true; 
        } else {
            audio.pauseResume();
        }
        update_ui();
    }
}

static void volume_slider_event_cb(lv_event_t * e) {
    lv_obj_t * slider = lv_event_get_target(e);
    int vol = (int)lv_slider_get_value(slider);
    audio.setVolume(vol);
}

// --- Equalizer Timer Callback ---
static void eq_timer_cb(lv_timer_t * timer) {
    for(int i = 0; i < EQ_BAR_COUNT; i++) {
        // Setzt den berechneten Wert flüssig auf die LVGL-Bar
        lv_bar_set_value(eq_bars[i], eq_band_values[i], LV_ANIM_OFF);
    }
}


// --- UI Aufbau ---
void init_ui() {
    // Hintergrund
    img_bg = lv_img_create(lv_scr_act());
    lv_img_set_src(img_bg, &bg320x240);
    lv_obj_align(img_bg, LV_ALIGN_CENTER, 0, 0);

    // Layout-Styles
    lv_style_init(&style_btn_container);
    lv_style_set_bg_opa(&style_btn_container, LV_OPA_20);
    lv_style_set_bg_color(&style_btn_container, lv_color_make(100, 150, 255));
    lv_style_set_border_color(&style_btn_container, lv_color_make(100, 170, 255));
    lv_style_set_border_width(&style_btn_container, 1);
    lv_style_set_radius(&style_btn_container, 16);

    lv_style_init(&style_premium_btn);
    lv_style_set_radius(&style_premium_btn, 12);
    lv_style_set_bg_opa(&style_premium_btn, LV_OPA_COVER);
    lv_style_set_bg_color(&style_premium_btn, lv_color_make(30, 80, 180));
    lv_style_set_bg_grad_color(&style_premium_btn, lv_color_make(15, 40, 110));
    lv_style_set_bg_grad_dir(&style_premium_btn, LV_GRAD_DIR_VER);
    lv_style_set_text_color(&style_premium_btn, lv_color_white());

    // 1. Text-Labels erzeugen
    label_status = lv_label_create(lv_scr_act());
    lv_obj_align(label_status, LV_ALIGN_TOP_MID, 0, 20);
    lv_obj_set_style_text_color(label_status, lv_color_make(150, 180, 220), 0);
    
    label_titel = lv_label_create(lv_scr_act());
    lv_obj_align(label_titel, LV_ALIGN_TOP_MID, 0, 50);
    lv_obj_set_width(label_titel, 280); 
    lv_label_set_long_mode(label_titel, LV_LABEL_LONG_SCROLL_CIRCULAR); 
    lv_obj_set_style_text_align(label_titel, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(label_titel, lv_color_white(), 0);


    // =================================================================
    // ANIMIERTER EQUALIZER CONTAINER 
    // =================================================================
    lv_obj_t * eq_container = lv_obj_create(lv_scr_act());
    lv_obj_set_size(eq_container, 190, 50); 
    lv_obj_align(eq_container, LV_ALIGN_BOTTOM_MID, 0, -125); 
    lv_obj_set_style_bg_opa(eq_container, LV_OPA_TRANSP, 0); 
    lv_obj_set_style_border_opa(eq_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(eq_container, 0, 0);
    lv_obj_set_scroll_dir(eq_container, LV_DIR_NONE);
    lv_obj_set_layout(eq_container, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(eq_container, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(eq_container, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);

    static lv_style_t style_eq_bar;
    lv_style_init(&style_eq_bar);
    lv_style_set_bg_color(&style_eq_bar, lv_color_make(0, 210, 255)); 
    lv_style_set_bg_grad_color(&style_eq_bar, lv_color_make(0, 80, 200)); 
    lv_style_set_bg_grad_dir(&style_eq_bar, LV_GRAD_DIR_VER);
    lv_style_set_radius(&style_eq_bar, 3); 

    static lv_style_t style_eq_bg;
    lv_style_init(&style_eq_bg);
    lv_style_set_bg_color(&style_eq_bg, lv_color_make(10, 10, 20));
    lv_style_set_radius(&style_eq_bg, 3);

    for(int i = 0; i < EQ_BAR_COUNT; i++) {
        eq_bars[i] = lv_bar_create(eq_container);
        lv_obj_set_size(eq_bars[i], 10, 40); // Leicht angepasst für optimale Breite im Container
        lv_obj_add_style(eq_bars[i], &style_eq_bg, LV_PART_MAIN);
        lv_obj_add_style(eq_bars[i], &style_eq_bar, LV_PART_INDICATOR);
        
        lv_bar_set_range(eq_bars[i], 0, 35);
        lv_bar_set_value(eq_bars[i], 1, LV_ANIM_OFF);
    }

    eq_timer = lv_timer_create(eq_timer_cb, 60, NULL);
    // =================================================================


    // 2. Lautstärke-Slider
    lv_obj_t * slider = lv_slider_create(lv_scr_act());
    lv_obj_set_size(slider, 220, 12); 
    lv_obj_align(slider, LV_ALIGN_CENTER, 0, 10); 
    lv_slider_set_range(slider, 0, 21);
    lv_slider_set_value(slider, 5, LV_ANIM_OFF); 
    lv_obj_add_event_cb(slider, volume_slider_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t * volume_symbol = lv_label_create(lv_scr_act());
    lv_label_set_text(volume_symbol, LV_SYMBOL_VOLUME_MAX);
    lv_obj_set_style_text_color(volume_symbol, lv_color_make(0, 150, 255), 0);
    lv_obj_align_to(volume_symbol, slider, LV_ALIGN_OUT_LEFT_MID, -15, 0);

    // 3. Buttons Container
    lv_obj_t * btn_container = lv_obj_create(lv_scr_act());
    lv_obj_set_size(btn_container, 290, 75);
    lv_obj_align(btn_container, LV_ALIGN_BOTTOM_MID, 0, -15);
    lv_obj_add_style(btn_container, &style_btn_container, 0);
    
    lv_obj_set_layout(btn_container, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(btn_container, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_container, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t * btn_prev = lv_btn_create(btn_container);
    lv_obj_add_style(btn_prev, &style_premium_btn, 0);
    lv_obj_t * lbl1 = lv_label_create(btn_prev); 
    lv_label_set_text(lbl1, LV_SYMBOL_PREV);
    lv_obj_add_event_cb(btn_prev, btn_event_cb, LV_EVENT_CLICKED, (void*)1);

    lv_obj_t * btn_play = lv_btn_create(btn_container);
    lv_obj_add_style(btn_play, &style_premium_btn, 0);
    lv_obj_set_width(btn_play, 110); 
    btn_play_lbl = lv_label_create(btn_play); 
    lv_label_set_text(btn_play_lbl, LV_SYMBOL_PLAY " Play");
    lv_obj_set_style_text_align(btn_play_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(btn_play_lbl, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_event_cb(btn_play, btn_event_cb, LV_EVENT_CLICKED, (void*)2);

    lv_obj_t * btn_next = lv_btn_create(btn_container);
    lv_obj_add_style(btn_next, &style_premium_btn, 0);
    lv_obj_t * lbl3 = lv_label_create(btn_next); 
    lv_label_set_text(lbl3, LV_SYMBOL_NEXT);
    lv_obj_add_event_cb(btn_next, btn_event_cb, LV_EVENT_CLICKED, (void*)3);

    update_ui();
}

void setup() {
    Serial.begin(115200);
    delay(1000);

    I2C_init();
    SD_MMC.setPins(SD_SCK, SD_CMD, SD_D0, SD_D1, SD_D2, SD_D3);
    if (!SD_MMC.begin("/sdcard", true)) {
        Serial.println("SD-Karte konnte nicht gemountet werden!");
    }

    tft.begin();
    tft.setRotation(1); 
    touch_init(SCREEN_WIDTH, SCREEN_HEIGHT, 1); 

    pinMode(AP_ENABLE, OUTPUT);
    digitalWrite(AP_ENABLE, LOW);
    es8311_codec_init();
    audioInit();
    audio.setPinout(I2S_BCK, I2S_WS, I2S_DOUT, I2S_MCK);
    audio.setVolume(5);
    
    demo_music();
    ladePlaylist();

    lv_init();
    
    uint32_t buffer_size = SCREEN_WIDTH * SCREEN_HEIGHT;
    lv_color_t *buf1 = (lv_color_t *)ps_malloc(buffer_size * sizeof(lv_color_t));
    lv_color_t *buf2 = (lv_color_t *)ps_malloc(buffer_size * sizeof(lv_color_t));

    if (buf1 == NULL || buf2 == NULL) {
        static lv_color_t fallback_buf[SCREEN_WIDTH * 15];
        lv_disp_draw_buf_init(&draw_buf, fallback_buf, NULL, SCREEN_WIDTH * 15);
    } else {
        lv_disp_draw_buf_init(&draw_buf, buf1, buf2, buffer_size);
    }

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = SCREEN_WIDTH;
    disp_drv.ver_res = SCREEN_HEIGHT;
    disp_drv.flush_cb = my_disp_flush;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);

    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = my_touchpad_read;
    lv_indev_drv_register(&indev_drv);
    
    init_ui();
}

void loop() {
    lv_timer_handler(); 
    delay(5);

    // Sicherheits-Check: Wenn der Player im "Playing"-Modus ist, aber intern gar nichts mehr läuft
    if (isPlaying && !audio.isRunning()) {
        Serial.println("Sicherheits-Check gegriffen: Lied läuft nicht mehr, wechsle Titel...");
        liedFertig = true;
    }

    // --- PLAYLIST WEITERSCHALTUNG ---
    if(liedFertig) {
        liedFertig = false; 
        
        Serial.println("Wechsle zum nächsten Titel...");
        audio.stopSong();   // Stoppt den aktuellen Stream sauber
        delay(200);         // Dem Task etwas mehr Zeit zum Atmen geben (wichtig beim ESP32-S3!)
        
        aktuellerTitel++;
        if(aktuellerTitel >= gesamtTitel) {
            aktuellerTitel = 0; 
        }
        
        demo_music_play(aktuellerTitel);
        isPlaying = true; 
        update_ui();
    }
}