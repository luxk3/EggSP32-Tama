#include <Wire.h>
#include <TFT_eSPI.h>
#include <math.h>
#include <WiFi.h>
#include "tamalib.h"
#include "hw.h"
#include "bitmaps_high_res.h"
#include "Tone32.h"

#if defined(ENABLE_AUTO_SAVE_STATUS) || defined(ENABLE_LOAD_STATE_FROM_EEPROM)
#include "savestate.h"
#endif

// ----------------- BUTTON AND BUZZER CONFIGURATION -----------------
#define PIN_BTN_L 26
#define PIN_BTN_M 25
#define PIN_BTN_R 17
#define PIN_BTN_RESET 15
#define PIN_BUZZER 4
#define BUZZER_CHANNEL 0
#define TONE_CHANNEL 15

#define BUTTON_RESET_TIME_SEC 10

TFT_eSPI tft = TFT_eSPI();  

// ----------------- FUNCTIONS PROTOTYPES -----------------
// function to implement required by tamalib
void hal_halt(void);
void hal_log(log_level_t level, char *buff, ...);
void hal_sleep_until(timestamp_t ts);
timestamp_t hal_get_timestamp(void);
void hal_update_screen(void);
void hal_set_lcd_matrix(u8_t x, u8_t y, bool_t val);
void hal_set_lcd_icon(u8_t icon, bool_t val);

//sound
void hal_set_frequency(u32_t freq);
void hal_play_frequency(bool_t en);
int hal_handler(void);

void esp32_noTone(uint8_t pin, uint8_t channel);
void esp32_tone(uint8_t pin, unsigned int frequency, unsigned long duration, uint8_t channel);

// ----------------- END FUNCTIONS PROTOTYPES -----------------

// ----------------- GLOBAL VARIABLES -----------------
static uint16_t current_freq = 0;

TFT_eSprite top_screen_sprite = TFT_eSprite(&tft);
TFT_eSprite bottom_screen_sprite = TFT_eSprite(&tft);
TFT_eSprite center_screen_sprite = TFT_eSprite(&tft);
TFT_eSprite save_icon_sprite = TFT_eSprite(&tft);
TFT_eSprite background_sprite = TFT_eSprite(&tft);

/** tamalib variables*/
static cpu_state_t cpuState;
static bool_t icon_buffer[ICON_NUM] = {0};
static unsigned long lastSaveTimestamp = 0;
static long last_interaction = 0;
uint32_t right_long_press_started = 2000;

/* Variables for screen upscaling */
// 3276 bytes corrispond to 224x117=26208 pixel area
uint8_t game_area[3276]={0};
uint16_t byte_index=0;
uint8_t bit_position=0;
uint8_t scale=7;

// variable to display the save icon
uint32_t last_time_save_icon_activated = 0;
uint32_t time_duration_save_icon_active = 2000;
uint8_t save_icon_active = 0;


// variable of type hal_t required to register the fuctions by the emulator
static hal_t hal = {
    .halt = &hal_halt,
    .log = &hal_log,
    .sleep_until = &hal_sleep_until,
    .get_timestamp = &hal_get_timestamp,
    .update_screen = &hal_update_screen,
    .set_lcd_matrix = &hal_set_lcd_matrix,
    .set_lcd_icon = &hal_set_lcd_icon,
    .set_frequency = &hal_set_frequency,
    .play_frequency = &hal_play_frequency,
    .handler = &hal_handler,
};

// ----------------- END GLOBAL VARIABLES -----------------

void setup(){

  //  Stop WiFi and Bluetooth
  WiFi.mode(WIFI_OFF);
  btStop();

  // Start Serial
  Serial.begin(SERIAL_BAUD);

  pinMode(PIN_BTN_L, INPUT);
  pinMode(PIN_BTN_M, INPUT);
  pinMode(PIN_BTN_R, INPUT);
  pinMode(PIN_BTN_RESET, INPUT);

  tft.begin();
  tft.setSwapBytes(true);
  tft.setRotation(4);
  tft.fillScreen(TFT_WHITE);

  // buzzer (PWM)
  ledcSetup(BUZZER_CHANNEL, NOTE_C4, 8);

  // initialization of tamalib
  tamalib_register_hal(&hal);
  tamalib_set_framerate(TAMA_DISPLAY_FRAMERATE);
  tamalib_init(1000000);

  // if enable, initialize the EEPROM
  #if defined(ENABLE_AUTO_SAVE_STATUS) || defined(ENABLE_LOAD_STATE_FROM_EEPROM)
  initEEPROM();
  #endif  

  // if enable, load the state from the EEPROM
  #ifdef ENABLE_LOAD_STATE_FROM_EEPROM
  if (validEEPROM())
  {
    loadStateFromEEPROM(&cpuState);
  }
  else
  {
    Serial.println(F("No magic number in state, skipping state restore"));
  }
  #endif


}


void loop(){


  tamalib_mainloop_step_by_step();
  
  // AUTOSAVE
  #ifdef ENABLE_AUTO_SAVE_STATUS
  if ((millis() - lastSaveTimestamp) > (AUTO_SAVE_MINUTES * 60 * 1000))
  {
    // save that the icon should be activated
    last_time_save_icon_activated = millis();
    // icon on
    save_icon_active=1;

    lastSaveTimestamp = millis();
    
    saveStateToEEPROM(&cpuState);
  }

  // deactivate the save icon if on and after 2 sconds  
  if (save_icon_active && ((millis() - last_time_save_icon_activated) > ( time_duration_save_icon_active)) ){
    save_icon_active=0;
  }
  #endif

  // press of reset button for more than BUTTON_RESET_TIME_SEC (10 sec)
  if (digitalRead(PIN_BTN_RESET) == BUTTON_VOLTAGE_LEVEL_PRESSED)
  {
    if (millis() - right_long_press_started > BUTTON_RESET_TIME_SEC * 1000)
    {
      eraseStateFromEEPROM();
      ESP.restart();
    }
     else
    {
      right_long_press_started = millis();
    }
  }
 
}


void hal_halt(){
    Serial.println("CPU Halted!");
}

void hal_log(log_level_t level, char *buff, ...){
     Serial.println(buff);
}

void hal_sleep_until(timestamp_t ts){
    
    // TODO enter deep sleep esp32
}

timestamp_t hal_get_timestamp()
{
  // tamalib requires microseconds (us), millis returns milliseconds  
  return millis() * 1000 ;
}


/**
 * I used the approach of filling a frame buffer. This function is called at every pixel. Drawing directly to the screen is too slow.
 * This function fills the game_area buffer. Then this is added to a canvas in the function hal_update_screen and drawn at once. 
 * 
 * Note: The code upscaled the graphics by `scale` times (default 7).
 */

void hal_set_lcd_matrix(u8_t x, u8_t y, bool_t val){
    
    for (uint8_t dx = 0; dx < scale; ++dx) {
          for (uint8_t dy = 0; dy < scale; ++dy) {

            byte_index = floor(((y * scale + dy) * 32 * scale + (x * scale + dx)) / 8);
            bit_position = 7 - (((y * scale + dy) * 32 * scale +(x * scale + dx)) % 8);
              
            if(val){
              game_area[byte_index] = game_area[byte_index] | (1 << bit_position);
            }
            else{

              game_area[byte_index] = game_area[byte_index] & ~(1 << bit_position);

            }
          }
        }
}


/**
 * This functions set the icon buffer. The icons are 8. The buffer contains 0 if the icon is not selected, and 1 if is selected.
 * This code fills an array keeping track which icon is selected. Icons are added to the canvas in the function hal_update_screen.
 * 
 */
void hal_set_lcd_icon(u8_t icon, bool_t val){
  icon_buffer[icon] = val;
}


/*
  The upscaled screen is drawn like this:
(0,0)
  +----------------------------------------------------+
  |                                                    |
  |                top_screen_sprite                   |
  | (0, 60)                                            |
  +----------------------------------------------------+
  |                                                    |  |
  |                                                    |  |
  |                                                    |  |
  |               center_screen_sprite                 |  |____ 240 x 120 pixels
  |                                                    |  |
  |                                                    |  |
  | (0,180)                                            |  |
  +----------------------------------------------------+
  |                                                    |
  |               bottom_screen_sprite                 |
  |                                                    |
  +----------------------------------------------------+ (240, 240)
 
  The icons are 60x60 pixels. The top of the screen contains the first 4 icons, the bottom the other 4.
  The icon images are scaled to 60x60 pixels
  The center screen is upscaled from 32x16 to 224x117 pixels (7 times the size) for a total of 26208 pixels and in bytes 3276.
 */

void hal_update_screen(){
    // creation of the sprites. Color dept was necessary otherwise would have not been drawn.
    // 'background_sprite' is the main canvas. The other sprites are added to this one.
    background_sprite.setColorDepth(8);
    background_sprite.setSwapBytes(true);
    background_sprite.createSprite(240, 240);
    background_sprite.fillSprite(TFT_WHITE);

    top_screen_sprite.setColorDepth(8);
    top_screen_sprite.createSprite(240, 60);
    
    bottom_screen_sprite.setColorDepth(8);
    bottom_screen_sprite.createSprite(240, 60);

    center_screen_sprite.setColorDepth(8);
    center_screen_sprite.createSprite(240, 120);
    center_screen_sprite.fillSprite(TFT_WHITE);


    // ------------- DRAW ICONS -------------
    for(uint8_t i=0; i<ICON_NUM; ++i){
      // //   //top of the screen
      if(i < 4){
        
        if(icon_buffer[i]){
          // tft.drawBitmap(icon * 60 , 0, tamagotchi_icon_allArray[icon], 60, 60, TFT_RED, TFT_WHITE);
          top_screen_sprite.drawBitmap(i * 60 , 0, tamagotchi_icon_allArray[i], 60, 60, TFT_RED, TFT_WHITE);
        }
        else{
          top_screen_sprite.drawBitmap(i * 60 , 0, tamagotchi_icon_allArray[i], 60, 60, TFT_DARKGREY, TFT_WHITE);
        }
      }
      //bottom of the screen
      if(i>=4){
        if(icon_buffer[i]){
          bottom_screen_sprite.drawBitmap((i-4) * 60 , 0, tamagotchi_icon_allArray[i], 60, 60, TFT_RED, TFT_WHITE);
        }
        else{
           bottom_screen_sprite.drawBitmap((i-4) * 60 , 0, tamagotchi_icon_allArray[i], 60, 60, TFT_DARKGREY, TFT_WHITE);
        }
      }
    }
    // ------------- END DRAW ICONS -------------

    // ------------- DRAW GAME AREA -------------
    center_screen_sprite.drawBitmap(8,2, game_area, 224, 117, TFT_BLACK, TFT_WHITE);
    // ------------- END DRAW GAME AREA -------------


    // ------------- ADD SCREEN PARTS TO BACKGROUND SPRITE -------------
    top_screen_sprite.pushToSprite(&background_sprite, 0, 0);
    center_screen_sprite.pushToSprite(&background_sprite, 0, 60);
    bottom_screen_sprite.pushToSprite(&background_sprite, 0, 180);

    if(save_icon_active){
      save_icon_sprite.setColorDepth(8);
      save_icon_sprite.createSprite(32, 32);
      save_icon_sprite.fillSprite(TFT_WHITE);
      
      save_icon_sprite.drawBitmap(0,0, save_icon, 32, 32, TFT_WHITE, TFT_BLUE);
      
      save_icon_sprite.pushToSprite(&background_sprite, 208, 0);

    }
    // ------------- END ADD SCREEN PARTS TO BACKGROUND SPRITE -------------

    // ------------- PUSH BACKGROUND SPRITE TO SCREEN -------------
    background_sprite.pushSprite(0, 0);
    
   
}

void hal_set_frequency(u32_t freq)
{
  current_freq = freq;
}

void hal_play_frequency(bool_t en)
{
  if (en)
  {

    esp32_tone(PIN_BUZZER, current_freq, 500, BUZZER_CHANNEL);

  }
  else
  {

    esp32_noTone(PIN_BUZZER, BUZZER_CHANNEL);
  }

}


int hal_handler(){

  if (digitalRead(PIN_BTN_L) == BUTTON_VOLTAGE_LEVEL_PRESSED)
  {
    tamalib_set_button(BTN_LEFT, BTN_STATE_PRESSED);
  }
  else
  {
    tamalib_set_button(BTN_LEFT, BTN_STATE_RELEASED);
    
  }

  if (digitalRead(PIN_BTN_M) == BUTTON_VOLTAGE_LEVEL_PRESSED)
  {
    tamalib_set_button(BTN_MIDDLE, BTN_STATE_PRESSED);
  }
  else
  {
    tamalib_set_button(BTN_MIDDLE, BTN_STATE_RELEASED);
  }

  if (digitalRead(PIN_BTN_R) == BUTTON_VOLTAGE_LEVEL_PRESSED)
  {
    tamalib_set_button(BTN_RIGHT, BTN_STATE_PRESSED);
  }
  else
  {
    tamalib_set_button(BTN_RIGHT, BTN_STATE_RELEASED);
  }
  return 0;
}

void esp32_noTone(uint8_t pin, uint8_t channel)
{
  ledcDetachPin(pin);
  ledcWrite(channel, 0);
}

void esp32_tone(uint8_t pin, unsigned int frequency, unsigned long duration, uint8_t channel)
{
  if (!ledcRead(channel))
  {
    ledcAttachPin(pin, channel);
  }

  ledcWriteTone(channel, frequency);
}