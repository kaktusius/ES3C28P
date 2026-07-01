#pragma once
#include <Arduino.h>
#include "driver/i2c_master.h"

#define TOUCH_FT6336_INT 17
#define TOUCH_FT6336_RST 18
#define TOUCH_I2C_ADDR   0x38  

extern i2c_master_bus_handle_t bus_handle;
i2c_master_dev_handle_t touch_handle = NULL;

int touch_last_x = 0;
int touch_last_y = 0;
uint16_t _touch_width = 320;
uint16_t _touch_height = 240;

void touch_init(unsigned short int w, unsigned short int h, unsigned char r)
{
  _touch_width = w;
  _touch_height = h;

  pinMode(TOUCH_FT6336_INT, INPUT);
  pinMode(TOUCH_FT6336_RST, OUTPUT);
  
  digitalWrite(TOUCH_FT6336_RST, LOW);
  delay(10);
  digitalWrite(TOUCH_FT6336_RST, HIGH);
  delay(50);

  if (bus_handle != NULL && touch_handle == NULL) {
    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = TOUCH_I2C_ADDR,
        .scl_speed_hz = 400000,
    };
    i2c_master_bus_add_device(bus_handle, &dev_config, &touch_handle);
  }
}

bool touch_touched(void)
{
  if (touch_handle == NULL) return false;

  uint8_t reg = 0x02; 
  uint8_t data[5];    

  esp_err_t err = i2c_master_transmit_receive(touch_handle, &reg, 1, data, 5, 50);
  
  if (err == ESP_OK) {
    int punkte = data[0] & 0x0F; 
    
    if (punkte > 0 && punkte <= 2) {
      // Wir holen die rohen Sensor-Koordinaten des Touch-Panels ab
      int raw_x = ((data[1] & 0x0F) << 8) | data[2];
      int raw_y = ((data[3] & 0x0F) << 8) | data[4];

      // Normierung auf die native Panel-Auflösung (0-239 und 0-319)
      // Das füttert deine mathematische Drehung im Haupt-Sketch perfekt!
      touch_last_x = map(raw_y, 0, 320, 0, 240 - 1);
      touch_last_y = map(raw_x, 0, 240, 0, 320 - 1);

      return true;
    }
  }
  return false;
}

bool touch_has_signal(void) { return true; }
bool touch_released(void)   { return true; }