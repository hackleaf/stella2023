#ifndef _MSC_VER
#include <stdbool.h>
#include <sched.h>
#endif
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

#ifdef _MSC_VER
#define snprintf _snprintf
#endif

#include "libretro.h"

#include "StellaLIBRETRO.hxx"
#include "Event.hxx"
#include "NTSCFilter.hxx"
#include "PaletteHandler.hxx"
#include "Version.hxx"

// retrodebug (arret-debugger) glue
#include "Console.hxx"
#include "System.hxx"
#include "M6502.hxx"
#include "M6532.hxx"
#include "TIA.hxx"
#include "Cart.hxx"
#include "retrodebug.h"


static StellaLIBRETRO stella;

static retro_log_printf_t log_cb;
static retro_video_refresh_t video_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;
static retro_environment_t environ_cb;
static retro_audio_sample_t audio_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static bool libretro_supports_bitmasks;

// libretro UI settings
static int setting_ntsc, setting_pal;
static int setting_stereo;
static int setting_phosphor, setting_console, setting_phosphor_blend;
static int stella_paddle_joypad_sensitivity;
static int stella_paddle_analog_sensitivity;
static int stella_paddle_mouse_sensitivity;
static int stella_paddle_analog_deadzone;
static bool stella_paddle_analog_absolute;
static bool stella_lightgun_crosshair;
static int setting_crop_hoverscan, crop_left;
static int setting_crop_voverscan, crop_top;
static NTSCFilter::Preset setting_filter;
static const char* setting_palette;

static bool system_reset;

static unsigned input_devices[4];
static int32_t input_crosshair[2];
static Controller::Type input_type[2];

void libretro_logger(int log_level, const char *source)
{
  retro_log_level log_mode = RETRO_LOG_INFO;
  size_t size  = strlen(source);
  char *string = (char*)malloc(size + 1);
  char *token;

  if (!string)
     return;

  strcpy(string, source);
  token = strtok(string, "\n");

  switch (log_level)
  {
    case 2: log_mode = RETRO_LOG_DEBUG; break;
    case 0: log_mode = RETRO_LOG_ERROR; break;
  }

  while (token != NULL)
  {
    log_cb(log_mode, "%s\n", token);
    token = strtok(NULL, "\n");
  }

  free(string);
  string = NULL;
}

// TODO input:
// https://github.com/libretro/blueMSX-libretro/blob/master/libretro.c
// https://github.com/libretro/libretro-o2em/blob/master/libretro.c

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
uint32_t libretro_read_rom(void* data)
{
  memcpy(data, stella.getROM(), stella.getROMSize());

  return stella.getROMSize();
}

uint32_t libretro_get_rom_size(void)
{
  return stella.getROMSize();
}

#define RETRO_ANALOG_COMMON() \
  bool mouse_l     = input_state_cb(pad, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_LEFT); \
  bool mouse_r     = input_state_cb(pad, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_RIGHT); \
  int32_t mouse_x  = input_state_cb(pad, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_X); \
  int32_t analog_x = input_state_cb(pad, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X); \
  *input_bitmask  |= mouse_l << RETRO_DEVICE_ID_JOYPAD_B; \
  if (stella_paddle_analog_deadzone && abs(analog_x) < stella_paddle_analog_deadzone * 0x7fff / 100) \
    analog_x = 0; \
  if (mouse_r) \
    mouse_x *= 3; \

static void retro_analog_paddle(unsigned pad, int32_t *analog_axis, int32_t *input_bitmask)
{
  RETRO_ANALOG_COMMON();

  if (mouse_x)
    *analog_axis += mouse_x * stella_paddle_mouse_sensitivity;
  else if (!stella_paddle_analog_absolute)
    *analog_axis += analog_x / 50;
  else
    *analog_axis  = analog_x;
  *analog_axis = BSPF::clamp(*analog_axis, -0x7fff, 0x7fff);
}

static void retro_analog_wheel(unsigned pad, int32_t *analog_axis, int32_t *input_bitmask)
{
  RETRO_ANALOG_COMMON();

  if (mouse_x)
    *analog_axis = mouse_x * stella_paddle_mouse_sensitivity * 50;
  else
    *analog_axis = analog_x;

  *analog_axis = BSPF::clamp(*analog_axis, -0x7fff, 0x7fff);
}

static void draw_crosshair(int16_t x, int16_t y, uint16_t color)
{
   int i;
   int size       = 3;
   int width      = stella.getVideoWidthMax();
   int viewport_w = stella.getVideoWidth();
   int viewport_h = stella.getVideoHeight();
   uint8_t zoom   = stella.getVideoZoom();

   /* crosshair center position */
   uint32_t *ptr = (uint32_t *)stella.getVideoBuffer() + (y * width) + x;

   /* default crosshair dimension */
   int x_start = x - size * zoom;
   int x_end   = x + size * zoom;
   int y_start = y - size;
   int y_end   = y + size;

   if (zoom > 1)
     x_end++;

   /* off-screen */
   if (x <= 0 || y <= 0)
      return;

   /* framebuffer limits */
   if (x_start < 0) x_start = 0;
   if (x_end > viewport_w) x_end = viewport_w;
   if (y_start < 0) y_start = 0;
   if (y_end > viewport_h) y_end = viewport_h;

   /* draw crosshair */
   for (i = (x_start - x); i <= (x_end - x); i++)
   {
      ptr[i] = (i & zoom) ? color : 0xffffff;
   }
   for (i = (y_start - y); i <= (y_end - y); i++)
   {
      ptr[i * width] = (i & 1) ? color : 0xffffff;
      if (zoom > 1)
        ptr[(i * width) + 1] = (i & 1) ? color : 0xffffff;
   }
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
static void update_input()
{
  if(!input_poll_cb) return;
  input_poll_cb();

#define EVENT stella.setInputEvent
  int32_t input_bitmask[4];
#define GET_BITMASK(pad) \
    if (libretro_supports_bitmasks) \
      input_bitmask[(pad)] = input_state_cb((pad), RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_MASK); \
    else \
    { \
        input_bitmask[(pad)] = 0; \
        for (int i = 0; i <= RETRO_DEVICE_ID_JOYPAD_R3; i++) \
          input_bitmask[(pad)] |= input_state_cb((pad), RETRO_DEVICE_JOYPAD, 0, i) ? (1 << i) : 0; \
    }
#define MASK_EVENT(evt, pad, id) stella.setInputEvent((evt), (input_bitmask[(pad)] & (1 << id)) ? 1 : 0)

  input_crosshair[0] = input_crosshair[1] = 0;

  int pad = 0;
  GET_BITMASK(pad)
  switch(input_type[0])
  {
    case Controller::Type::Joy2BPlus:
    case Controller::Type::BoosterGrip:
      MASK_EVENT(Event::LeftJoystickFire9, pad, RETRO_DEVICE_ID_JOYPAD_Y);
      [[fallthrough]];
    case Controller::Type::Genesis:
      MASK_EVENT(Event::LeftJoystickFire5, pad, RETRO_DEVICE_ID_JOYPAD_A);
      [[fallthrough]];
    case Controller::Type::Joystick:
      MASK_EVENT(Event::LeftJoystickLeft,  pad, RETRO_DEVICE_ID_JOYPAD_LEFT);
      MASK_EVENT(Event::LeftJoystickRight, pad, RETRO_DEVICE_ID_JOYPAD_RIGHT);
      MASK_EVENT(Event::LeftJoystickUp,    pad, RETRO_DEVICE_ID_JOYPAD_UP);
      MASK_EVENT(Event::LeftJoystickDown,  pad, RETRO_DEVICE_ID_JOYPAD_DOWN);
      MASK_EVENT(Event::LeftJoystickFire,  pad, RETRO_DEVICE_ID_JOYPAD_B);
      break;

    case Controller::Type::Driving:
    {
      int32_t wheel = 0;

      retro_analog_wheel(pad, &wheel, &input_bitmask[pad]);
      EVENT(Event::LeftDrivingAnalog, wheel);
      MASK_EVENT(Event::LeftDrivingCCW,  pad, RETRO_DEVICE_ID_JOYPAD_LEFT);
      MASK_EVENT(Event::LeftDrivingCW,   pad, RETRO_DEVICE_ID_JOYPAD_RIGHT);
      MASK_EVENT(Event::LeftDrivingFire, pad, RETRO_DEVICE_ID_JOYPAD_B);
      break;
    }

    case Controller::Type::Paddles:
    {
      static int32_t paddle_a = 0;
      static int32_t paddle_b = 0;

      retro_analog_paddle(pad, &paddle_a, &input_bitmask[pad]);
      EVENT(Event::LeftPaddleAAnalog, paddle_a);
      MASK_EVENT(Event::LeftPaddleAIncrease, pad, RETRO_DEVICE_ID_JOYPAD_LEFT);
      MASK_EVENT(Event::LeftPaddleADecrease, pad, RETRO_DEVICE_ID_JOYPAD_RIGHT);
      MASK_EVENT(Event::LeftPaddleAFire,     pad, RETRO_DEVICE_ID_JOYPAD_B);

      pad++;
      GET_BITMASK(pad)

      retro_analog_paddle(pad, &paddle_b, &input_bitmask[pad]);
      EVENT(Event::LeftPaddleBAnalog, paddle_b);
      MASK_EVENT(Event::LeftPaddleBIncrease, pad, RETRO_DEVICE_ID_JOYPAD_LEFT);
      MASK_EVENT(Event::LeftPaddleBDecrease, pad, RETRO_DEVICE_ID_JOYPAD_RIGHT);
      MASK_EVENT(Event::LeftPaddleBFire,     pad, RETRO_DEVICE_ID_JOYPAD_B);
      break;
    }

    case Controller::Type::Lightgun:
    {
      // scale from -0x8000..0x7fff to image rect
      const Common::Rect& rect = stella.getImageRect();
      const int32_t x = (input_state_cb(pad, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_SCREEN_X) + 0x7fff) * rect.w() / 0xffff;
      const int32_t y = (input_state_cb(pad, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_SCREEN_Y) + 0x7fff) * rect.h() / 0xffff;

      input_crosshair[0] = x > 0 && x < 0x7fff ? x * stella.getVideoWidth() / rect.w() : 0;
      input_crosshair[1] = y > 0 && y < 0x7fff ? y * stella.getVideoHeight() / rect.h() : 0;

      EVENT(Event::MouseAxisXValue, x);
      EVENT(Event::MouseAxisYValue, y);
      EVENT(Event::MouseButtonLeftValue,  input_state_cb(pad, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_TRIGGER));
      EVENT(Event::MouseButtonRightValue, input_state_cb(pad, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_TRIGGER));
      break;
    }

    case Controller::Type::AmigaMouse:
    case Controller::Type::AtariMouse:
    case Controller::Type::TrakBall:
    {
      bool mouse_l     = input_state_cb(pad, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_LEFT);
      bool mouse_r     = input_state_cb(pad, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_RIGHT);
      int32_t mouse_x  = input_state_cb(pad, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_X);
      int32_t mouse_y  = input_state_cb(pad, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_Y);
      int32_t analog_x = input_state_cb(pad, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X);
      int32_t analog_y = input_state_cb(pad, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y);
      float analog_mag = sqrt((analog_x * analog_x) + (analog_y * analog_y));

      if (stella_paddle_analog_deadzone && analog_mag <= stella_paddle_analog_deadzone * 0x7fff / 100)
        analog_x = analog_y = 0;

      mouse_x += analog_x / (80000 / stella_paddle_analog_sensitivity);
      mouse_y += analog_y / (80000 / stella_paddle_analog_sensitivity);

      if (input_bitmask[pad] & (1 << RETRO_DEVICE_ID_JOYPAD_LEFT))
        mouse_x -= stella_paddle_joypad_sensitivity;
      else if (input_bitmask[pad] & (1 << RETRO_DEVICE_ID_JOYPAD_RIGHT))
        mouse_x += stella_paddle_joypad_sensitivity;

      if (input_bitmask[pad] & (1 << RETRO_DEVICE_ID_JOYPAD_UP))
        mouse_y -= stella_paddle_joypad_sensitivity;
      else if (input_bitmask[pad] & (1 << RETRO_DEVICE_ID_JOYPAD_DOWN))
        mouse_y += stella_paddle_joypad_sensitivity;

      if (input_bitmask[pad] & (1 << RETRO_DEVICE_ID_JOYPAD_B))
        mouse_l = true;
      if (input_bitmask[pad] & (1 << RETRO_DEVICE_ID_JOYPAD_A))
        mouse_r = true;

      EVENT(Event::MouseAxisXMove, mouse_x);
      EVENT(Event::MouseAxisYMove, mouse_y);
      EVENT(Event::MouseButtonLeftValue,  mouse_l);
      EVENT(Event::MouseButtonRightValue, mouse_r);
      break;
    }

    default:
      break;
  }
  pad++;
  GET_BITMASK(pad)

  switch(input_type[1])
  {
    case Controller::Type::Joy2BPlus:
    case Controller::Type::BoosterGrip:
      MASK_EVENT(Event::RightJoystickFire9, pad, RETRO_DEVICE_ID_JOYPAD_Y);
      [[fallthrough]];
    case Controller::Type::Genesis:
      MASK_EVENT(Event::RightJoystickFire5, pad, RETRO_DEVICE_ID_JOYPAD_A);
      [[fallthrough]];
    case Controller::Type::Joystick:
      MASK_EVENT(Event::RightJoystickLeft,  pad, RETRO_DEVICE_ID_JOYPAD_LEFT);
      MASK_EVENT(Event::RightJoystickRight, pad, RETRO_DEVICE_ID_JOYPAD_RIGHT);
      MASK_EVENT(Event::RightJoystickUp,    pad, RETRO_DEVICE_ID_JOYPAD_UP);
      MASK_EVENT(Event::RightJoystickDown,  pad, RETRO_DEVICE_ID_JOYPAD_DOWN);
      MASK_EVENT(Event::RightJoystickFire,  pad, RETRO_DEVICE_ID_JOYPAD_B);
      break;

    case Controller::Type::Driving:
    {
      int32_t wheel = 0;

      retro_analog_wheel(pad, &wheel, &input_bitmask[pad]);
      EVENT(Event::RightDrivingAnalog, wheel);
      MASK_EVENT(Event::RightDrivingCCW,  pad, RETRO_DEVICE_ID_JOYPAD_LEFT);
      MASK_EVENT(Event::RightDrivingCW,   pad, RETRO_DEVICE_ID_JOYPAD_RIGHT);
      MASK_EVENT(Event::RightDrivingFire, pad, RETRO_DEVICE_ID_JOYPAD_B);
      break;
    }

    case Controller::Type::Paddles:
    {
      static int32_t paddle_a = 0;
      static int32_t paddle_b = 0;

      retro_analog_paddle(pad, &paddle_a, &input_bitmask[pad]);
      EVENT(Event::RightPaddleAAnalog, paddle_a);
      MASK_EVENT(Event::RightPaddleAIncrease, pad, RETRO_DEVICE_ID_JOYPAD_LEFT);
      MASK_EVENT(Event::RightPaddleADecrease, pad, RETRO_DEVICE_ID_JOYPAD_RIGHT);
      MASK_EVENT(Event::RightPaddleAFire,     pad, RETRO_DEVICE_ID_JOYPAD_B);

      pad++;
      GET_BITMASK(pad)

      retro_analog_paddle(pad, &paddle_b, &input_bitmask[pad]);
      EVENT(Event::RightPaddleBAnalog, paddle_b);
      MASK_EVENT(Event::RightPaddleBIncrease, pad, RETRO_DEVICE_ID_JOYPAD_LEFT);
      MASK_EVENT(Event::RightPaddleBDecrease, pad, RETRO_DEVICE_ID_JOYPAD_RIGHT);
      MASK_EVENT(Event::RightPaddleBFire,     pad, RETRO_DEVICE_ID_JOYPAD_B);
      break;
    }

    default:
      break;
  }

  // Notes:
  // - Each event can only be assigned once, in case of conflicts ususally the latest assignment will be active
  // - The follwing events can also be used by analog devices
  MASK_EVENT(Event::ConsoleLeftDiffA,  0, RETRO_DEVICE_ID_JOYPAD_L);
  MASK_EVENT(Event::ConsoleLeftDiffB,  0, RETRO_DEVICE_ID_JOYPAD_L2);
  MASK_EVENT(Event::ConsoleColor,      0, RETRO_DEVICE_ID_JOYPAD_L3);
  MASK_EVENT(Event::ConsoleRightDiffA, 0, RETRO_DEVICE_ID_JOYPAD_R);
  MASK_EVENT(Event::ConsoleRightDiffB, 0, RETRO_DEVICE_ID_JOYPAD_R2);
  MASK_EVENT(Event::ConsoleBlackWhite, 0, RETRO_DEVICE_ID_JOYPAD_R3);
  MASK_EVENT(Event::ConsoleSelect,     0, RETRO_DEVICE_ID_JOYPAD_SELECT);
  MASK_EVENT(Event::ConsoleReset,      0, RETRO_DEVICE_ID_JOYPAD_START);

#undef EVENT
#undef MASK_EVENT
#undef GET_BITMASK
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
static void update_geometry()
{
  struct retro_system_av_info av_info;

  retro_get_system_av_info(&av_info);

  environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &av_info);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
static void update_system_av()
{
  struct retro_system_av_info av_info;

  retro_get_system_av_info(&av_info);

  environ_cb(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO, &av_info);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
static void update_variables(bool init = false)
{
  bool geometry_update = false;

  struct retro_variable var;

#define RETRO_GET(x) \
  var.key = x; \
  var.value = NULL; \
  if(environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)

  RETRO_GET("stella_filter")
  {
    NTSCFilter::Preset value = NTSCFilter::Preset::OFF;

    if(!strcmp(var.value, "disabled"))            value = NTSCFilter::Preset::OFF;
    else if(!strcmp(var.value, "composite"))      value = NTSCFilter::Preset::COMPOSITE;
    else if(!strcmp(var.value, "s-video"))        value = NTSCFilter::Preset::SVIDEO;
    else if(!strcmp(var.value, "rgb"))            value = NTSCFilter::Preset::RGB;
    else if(!strcmp(var.value, "badly adjusted")) value = NTSCFilter::Preset::BAD;

    if(setting_filter != value)
    {
      stella.setVideoFilter(value);

      geometry_update = true;
      setting_filter = value;
    }
  }

  RETRO_GET("stella_crop_hoverscan")
  {
    setting_crop_hoverscan = !strcmp(var.value, "enabled");

    geometry_update = true;
  }

  RETRO_GET("stella_crop_voverscan")
  {
    setting_crop_voverscan = atoi(var.value);

    geometry_update = true;
  }

  RETRO_GET("stella_ntsc_aspect")
  {
    int value = 0;

    if(!strcmp(var.value, "par")) value = 0;
    else value = atoi(var.value);

    if(setting_ntsc != value)
    {
      stella.setVideoAspectNTSC(value);

      geometry_update = true;
      setting_ntsc = value;
    }
  }

  RETRO_GET("stella_pal_aspect")
  {
    int value = 0;

    if(!strcmp(var.value, "par")) value = 0;
    else value = atoi(var.value);

    if(setting_pal != value)
    {
      stella.setVideoAspectPAL(value);

      setting_pal = value;
      geometry_update = true;
    }
  }

  RETRO_GET("stella_palette")
  {
    if(setting_palette != var.value)
    {
      stella.setVideoPalette(var.value);

      setting_palette = var.value;
    }
  }

  RETRO_GET("stella_console")
  {
    int value = 0;

    if(!strcmp(var.value, "auto")) value = 0;
    else if(!strcmp(var.value, "ntsc")) value = 1;
    else if(!strcmp(var.value, "pal")) value = 2;
    else if(!strcmp(var.value, "secam")) value = 3;
    else if(!strcmp(var.value, "ntsc50")) value = 4;
    else if(!strcmp(var.value, "pal60")) value = 5;
    else if(!strcmp(var.value, "secam60")) value = 6;

    if(setting_console != value)
    {
      stella.setConsoleFormat(value);

      setting_console = value;
      system_reset = true;
    }
  }

  RETRO_GET("stella_stereo")
  {
    int value = 0;

    if(!strcmp(var.value, "auto")) value = 0;
    else if(!strcmp(var.value, "off")) value = 1;
    else if(!strcmp(var.value, "on")) value = 2;

    if(setting_stereo != value)
    {
      stella.setAudioStereo(value);

      setting_stereo = value;
    }
  }

  RETRO_GET("stella_phosphor")
  {
    int value = 0;

    if(!strcmp(var.value, "auto")) value = 0;
    else if(!strcmp(var.value, "off")) value = 1;
    else if(!strcmp(var.value, "on")) value = 2;

    if(setting_phosphor != value)
    {
      stella.setVideoPhosphor(value, setting_phosphor_blend);

      setting_phosphor = value;
    }
  }

  RETRO_GET("stella_phosphor_blend")
  {
    int value = 0;

    value = atoi(var.value);

    if(setting_phosphor_blend != value)
    {
      stella.setVideoPhosphor(setting_phosphor, value);

      setting_phosphor_blend = value;
    }
  }

  RETRO_GET("stella_paddle_joypad_sensitivity")
  {
    int value = 0;

    value = atoi(var.value);

    if(stella_paddle_joypad_sensitivity != value)
    {
      if(!init) stella.setPaddleJoypadSensitivity(value);

      stella_paddle_joypad_sensitivity = value;
    }
  }

  RETRO_GET("stella_paddle_analog_sensitivity")
  {
    int value = 0;

    value = atoi(var.value);

    if(stella_paddle_analog_sensitivity != value)
    {
      if(!init) stella.setPaddleAnalogSensitivity(value);

      stella_paddle_analog_sensitivity = value;
    }
  }

  RETRO_GET("stella_paddle_mouse_sensitivity")
  {
    stella_paddle_mouse_sensitivity = atoi(var.value);
  }

  RETRO_GET("stella_paddle_analog_deadzone")
  {
    stella_paddle_analog_deadzone = atoi(var.value);
  }

  RETRO_GET("stella_paddle_analog_absolute")
  {
    stella_paddle_analog_absolute = false;

    if(!strcmp(var.value, "enabled"))
      stella_paddle_analog_absolute = true;
  }

  RETRO_GET("stella_lightgun_crosshair")
  {
    stella_lightgun_crosshair = false;

    if(!strcmp(var.value, "enabled"))
      stella_lightgun_crosshair = true;
  }

  if(!init && !system_reset)
  {
    crop_left = setting_crop_hoverscan ? (stella.getVideoZoom() == 2 ? 32 : 8) : 0;
    crop_top  = setting_crop_voverscan;

    if(geometry_update) update_geometry();
  }

#undef RETRO_GET
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
/* retrodebug (arret-debugger) console lifecycle hooks (defined in the glue
   section below). */
static void rd_on_console_created();
static void rd_on_console_destroyed();

static bool reset_system()
{
  // clean restart
  stella.destroy();
  rd_on_console_destroyed();

  // apply pre-boot settings first
  update_variables(true);

  // start system
  if(!stella.create(log_cb ? true : false)) return false;

  // retrodebug: console now exists — expose CPU/memory regions
  rd_on_console_created();

  // get auto-detect controllers
  input_type[0] = stella.getLeftControllerType();
  input_type[1] = stella.getRightControllerType();
  stella.setPaddleJoypadSensitivity(stella_paddle_joypad_sensitivity);
  stella.setPaddleAnalogSensitivity(stella_paddle_analog_sensitivity);

  system_reset = false;

  // reset libretro window, apply post-boot settings
  update_variables(false);

  return true;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
unsigned retro_api_version()
{
  return RETRO_API_VERSION;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
unsigned retro_get_region()
{
  return stella.getVideoNTSC() ? RETRO_REGION_NTSC : RETRO_REGION_PAL;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
void retro_set_audio_sample(retro_audio_sample_t cb) { audio_cb = cb; }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
void retro_set_input_poll(retro_input_poll_t cb) { input_poll_cb = cb; }
void retro_set_input_state(retro_input_state_t cb) { input_state_cb = cb; }

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void retro_get_system_info(struct retro_system_info *info)
{
  *info = retro_system_info{};  // reset to defaults

  info->library_name = stella.getCoreName();
#ifndef GIT_VERSION
#define GIT_VERSION ""
#endif
  info->library_version = STELLA_VERSION GIT_VERSION;
  info->valid_extensions = stella.getROMExtensions();
  info->need_fullpath = false;
  info->block_extract = false;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void retro_get_system_av_info(struct retro_system_av_info *info)
{
  *info = retro_system_av_info{};  // reset to defaults
  unsigned crop_width         = crop_left ? 8 : 0;

  info->timing.fps            = stella.getVideoRate();
  info->timing.sample_rate    = stella.getAudioRate();

  info->geometry.base_width   = stella.getRenderWidth();
  info->geometry.base_height  = stella.getRenderHeight();

  info->geometry.max_width    = stella.getVideoWidthMax();
  info->geometry.max_height   = stella.getVideoHeightMax();

  info->geometry.aspect_ratio = stella.getVideoAspectPar() *
      (float)(160 - crop_width) * 2 / (float)(stella.getVideoHeight() - (crop_top * 2));
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void retro_set_controller_port_device(unsigned port, unsigned device)
{
  if(port < 4)
  {
    switch(device)
    {
      case RETRO_DEVICE_NONE:
      case RETRO_DEVICE_JOYPAD:
        input_devices[port] = device;
        break;

      default:
        input_devices[port] = RETRO_DEVICE_JOYPAD;
        break;
    }
  }
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
/* ====================================================================== */
/* retrodebug glue (arret-debugger)                                       */
/*                                                                        */
/* The Atari 2600's 6507 is a full MOS 6502 (decimal mode intact, unlike  */
/* the NES 2A03) with only 13 address pins, so its 8KB space mirrors       */
/* through the 16-bit bus.  We expose the whole 64KB CPU address space and */
/* let Stella's System resolve mirroring/bankswitching.                    */
/*                                                                        */
/* Halting uses Stella's native clean-stop: the execution hook returns     */
/* true, M6502::_execute() stops with a debugger-status DispatchResult,    */
/* and the frame loop unwinds.  No fceumm-style "phase resume" is needed   */
/* because Stella keeps the TIA/RIOT in lockstep with the CPU.             */
/* ====================================================================== */

static rd_DebuggerIf *debugger_if;
static bool rd_game_loaded = false;

/* --- live system/CPU access (valid only while a game is loaded) --- */

static System *rd_sys() {
  return rd_game_loaded ? &stella.osystem().console().system() : nullptr;
}
static M6502 *rd_m6502() {
  return rd_game_loaded ? &stella.osystem().console().system().m6502() : nullptr;
}
static M6532 *rd_riot() {
  return rd_game_loaded ? &stella.osystem().console().system().m6532() : nullptr;
}
static TIA *rd_tia() {
  return rd_game_loaded ? &stella.osystem().console().system().tia() : nullptr;
}
static Cartridge *rd_cart() {
  return rd_game_loaded ? &stella.osystem().console().system().cart() : nullptr;
}

/* --- register get / set --- */

static uint64_t rd_get_register(rd_Cpu const *self, unsigned reg) {
  (void)self;
  M6502 *cpu = rd_m6502();
  if(!cpu) return 0;
  switch(reg) {
  case RD_6502_A:  return cpu->rdRegA();
  case RD_6502_X:  return cpu->rdRegX();
  case RD_6502_Y:  return cpu->rdRegY();
  case RD_6502_S:  return cpu->rdRegSP();
  case RD_6502_PC: return cpu->rdRegPC();
  case RD_6502_P:  return cpu->rdRegPS();
  default:         return 0;
  }
}

static int rd_set_register(rd_Cpu const *self, unsigned reg, uint64_t value) {
  (void)self;
  M6502 *cpu = rd_m6502();
  if(!cpu) return 0;
  switch(reg) {
  case RD_6502_A:  cpu->rdSetRegA((uInt8)value);   return 1;
  case RD_6502_X:  cpu->rdSetRegX((uInt8)value);   return 1;
  case RD_6502_Y:  cpu->rdSetRegY((uInt8)value);   return 1;
  case RD_6502_S:  cpu->rdSetRegSP((uInt8)value);  return 1;
  case RD_6502_PC: cpu->rdSetRegPC((uInt16)value); return 1;
  case RD_6502_P:  cpu->rdSetRegPS((uInt8)value);  return 1;
  default:         return 0;
  }
}

/* --- CPU address space (64KB, mirroring resolved by System) --- */

static uint8_t rd_bus_read(System *sys, uint16_t addr, bool side_effects) {
  /* RAM/ROM pages expose a direct pointer — read it straight out.  This is
     genuinely side-effect-free and safe to call concurrently with the running
     core thread (at worst a stale byte during a RAM write; no hang).
     I/O pages (TIA/RIOT) have a null directPeekBase: reading them via
     device->peek() has side effects (e.g. latching the RIOT timer) that would
     disrupt the live emulation, so for a side-effect-free read we return 0 as
     the retrodebug contract allows.  Note: lockDataBus() is a no-op here — it
     only takes effect under DEBUGGER_SUPPORT, which the libretro build omits. */
  const System::PageAccess& pa = sys->getPageAccess(addr);
  if(pa.directPeekBase)
    return pa.directPeekBase[addr & System::PAGE_MASK];
  // RIOT zero-page RAM ($80-$FF and mirrors) is serviced by M6532::peek (no
  // direct pointer) but is side-effect-free to read straight from the RAM
  // array.  A9=0 selects RAM; A9=1 selects the RIOT I/O registers.
  if(pa.device == &sys->m6532() && (addr & 0x0200) == 0)
    return sys->m6532().getRAM()[addr & 0x007f];
  // Everything else is read-sensitive I/O (TIA / RIOT registers): reading via
  // device->peek() has side effects (e.g. latching the RIOT timer) that would
  // disrupt the live core, so a side-effect-free read returns 0 (contract).
  if(!side_effects)
    return 0;
  return sys->peek(addr, Device::NONE);
}

static void rd_bus_write(System *sys, uint16_t addr, uint8_t val) {
  /* Prefer the direct poke pointer (RAM) to avoid triggering I/O side effects
     on the live core; fall back to the device poke for everything else. */
  const System::PageAccess& pa = sys->getPageAccess(addr);
  if(pa.directPokeBase)
    pa.directPokeBase[addr & System::PAGE_MASK] = val;
  else
    sys->poke(addr, val, Device::NONE);
}

static uint64_t rd_cpu_peek(rd_Memory const *self, uint64_t address, uint64_t size,
                            uint8_t *outbuff, bool side_effects) {
  (void)self;
  System *sys = rd_sys();
  for(uint64_t i = 0; i < size; i++) {
    const uint64_t addr = address + i;
    if(!sys || addr > 0xFFFF) { outbuff[i] = 0; continue; }
    outbuff[i] = rd_bus_read(sys, (uint16_t)addr, side_effects);
  }
  return size;
}

static uint64_t rd_cpu_poke(rd_Memory const *self, uint64_t address, uint64_t size,
                            uint8_t const *buff) {
  (void)self;
  System *sys = rd_sys();
  if(!sys) return 0;
  uint64_t count = 0;
  for(uint64_t i = 0; i < size; i++) {
    const uint64_t addr = address + i;
    if(addr > 0xFFFF) continue;
    rd_bus_write(sys, (uint16_t)addr, buff[i]);
    count++;
  }
  return count;
}

static rd_Memory rd_cpu_mem = {
  /* .v1 = */ {
    /* .id          */ "cpu",
    /* .description */ "CPU Address Space ($0000-$FFFF)",
    /* .alignment   */ 1,
    /* .size        */ 0x10000,
    /* .break_points*/ nullptr,
    /* .peek        */ rd_cpu_peek,
    /* .poke        */ rd_cpu_poke,
    /* rest zero-initialized */
  }
};

static const rd_Cpu rd_cpu = {
  /* .v1 = */ {
    /* .id            */ "6507",
    /* .description   */ "MOS 6507",
    /* .type          */ RD_CPU_6502,
    /* .config        */ 0,   /* full 6502: decimal mode intact */
    /* .memory_region */ &rd_cpu_mem,
    /* .break_points  */ nullptr,
    /* .get_register  */ rd_get_register,
    /* .set_register  */ rd_set_register,
    /* rest zero */
  }
};

static const rd_Cpu *rd_cpus[] = { &rd_cpu, nullptr };

/* ---- wram: 128-byte RIOT zero-page RAM ($80-$FF) ---- */

static uint64_t rd_wram_peek(rd_Memory const *self, uint64_t address, uint64_t size,
                             uint8_t *outbuff, bool side_effects) {
  (void)self; (void)side_effects;
  M6532 *riot = rd_riot();
  const uint8_t *ram = riot ? riot->getRAM() : nullptr;
  for(uint64_t i = 0; i < size; i++) {
    const uint64_t a = address + i;
    outbuff[i] = (ram && a < 128) ? ram[a] : 0;
  }
  return size;
}

static uint64_t rd_wram_poke(rd_Memory const *self, uint64_t address, uint64_t size,
                             uint8_t const *buff) {
  (void)self;
  M6532 *riot = rd_riot();
  uint8_t *ram = riot ? riot->getRAM() : nullptr;
  if(!ram) return 0;
  uint64_t count = 0;
  for(uint64_t i = 0; i < size; i++) {
    const uint64_t a = address + i;
    if(a >= 128) continue;
    ram[a] = buff[i];
    count++;
  }
  return count;
}

static rd_Memory rd_wram_mem = {
  /* .v1 = */ {
    /* .id          */ "wram",
    /* .description */ "RIOT RAM (zero page $80-$FF)",
    /* .alignment   */ 1,
    /* .size        */ 128,
    /* .break_points*/ nullptr,
    /* .peek        */ rd_wram_peek,
    /* .poke        */ rd_wram_poke,
  }
};

/* ---- io: TIA write registers ($00-$3F), via side-effect-free shadow ---- */

static uint64_t rd_io_peek(rd_Memory const *self, uint64_t address, uint64_t size,
                           uint8_t *outbuff, bool side_effects) {
  (void)self; (void)side_effects;
  TIA *tia = rd_tia();
  for(uint64_t i = 0; i < size; i++) {
    const uint64_t a = address + i;
    /* registerValue() returns the last-written shadow value for regs 0-63
       without touching live TIA state. */
    outbuff[i] = (tia && a < 0x40) ? tia->registerValue((uint8_t)a) : 0;
  }
  return size;
}

static uint64_t rd_io_poke(rd_Memory const *self, uint64_t address, uint64_t size,
                           uint8_t const *buff) {
  (void)self;
  System *sys = rd_sys();
  if(!sys) return 0;
  uint64_t count = 0;
  for(uint64_t i = 0; i < size; i++) {
    const uint64_t a = address + i;
    if(a >= 0x40) continue;
    sys->poke((uint16_t)a, buff[i], Device::NONE);  /* write to the live TIA */
    count++;
  }
  return count;
}

static rd_Memory rd_io_mem = {
  /* .v1 = */ {
    /* .id          */ "io",
    /* .description */ "TIA registers ($00-$3F, write shadow)",
    /* .alignment   */ 1,
    /* .size        */ 0x40,
    /* .break_points*/ nullptr,
    /* .peek        */ rd_io_peek,
    /* .poke        */ rd_io_poke,
  }
};

/* ---- rom: cartridge ROM image (writable for patching) ---- */

static uint64_t rd_rom_peek(rd_Memory const *self, uint64_t address, uint64_t size,
                            uint8_t *outbuff, bool side_effects) {
  (void)self; (void)side_effects;
  Cartridge *cart = rd_cart();
  size_t romsz = 0;
  const uint8_t *img = cart ? cart->getImage(romsz).get() : nullptr;
  for(uint64_t i = 0; i < size; i++) {
    const uint64_t a = address + i;
    outbuff[i] = (img && a < romsz) ? img[a] : 0;
  }
  return size;
}

static uint64_t rd_rom_poke(rd_Memory const *self, uint64_t address, uint64_t size,
                            uint8_t const *buff) {
  (void)self;
  Cartridge *cart = rd_cart();
  size_t romsz = 0;
  /* The bank pointers index into this image, so patching it affects execution. */
  uint8_t *img = cart ? const_cast<uint8_t*>(cart->getImage(romsz).get()) : nullptr;
  if(!img) return 0;
  uint64_t count = 0;
  for(uint64_t i = 0; i < size; i++) {
    const uint64_t a = address + i;
    if(a >= romsz) continue;
    img[a] = buff[i];
    count++;
  }
  return count;
}

static rd_Memory rd_rom_mem = {
  /* .v1 = */ {
    /* .id          */ "rom",
    /* .description */ "Cartridge ROM",
    /* .alignment   */ 1,
    /* .size        */ 0,   /* set at load from the cart image */
    /* .break_points*/ nullptr,
    /* .peek        */ rd_rom_peek,
    /* .poke        */ rd_rom_poke,
  }
};

/* ---- cartram: cartridge internal RAM (e.g. CommaVid CV's 1KB) ----
   Read-only here (no generic cart-RAM setter); writes go through the cpu
   region's direct-poke window. */

static uint64_t rd_cartram_peek(rd_Memory const *self, uint64_t address, uint64_t size,
                                uint8_t *outbuff, bool side_effects) {
  (void)self; (void)side_effects;
  Cartridge *cart = rd_cart();
  const uint32_t sz = cart ? cart->internalRamSize() : 0;
  for(uint64_t i = 0; i < size; i++) {
    const uint64_t a = address + i;
    outbuff[i] = (cart && a < sz) ? cart->internalRamGetValue((uint16_t)a) : 0;
  }
  return size;
}

static rd_Memory rd_cartram_mem = {
  /* .v1 = */ {
    /* .id          */ "cartram",
    /* .description */ "Cartridge RAM",
    /* .alignment   */ 1,
    /* .size        */ 0,   /* set at load; region omitted if the cart has none */
    /* .break_points*/ nullptr,
    /* .peek        */ rd_cartram_peek,
    /* .poke        */ nullptr,   /* read-only (patch via the cpu region) */
  }
};

/* Region list, populated at load by rd_setup_regions().  Must be a valid,
   null-terminated array — much of the frontend iterates it without first
   null-checking the array pointer. */
static rd_Memory const *rd_mem_regions[8] = { nullptr };
static rd_Filesystem const *rd_filesystems[] = { nullptr };
static rd_MiscBreakpoint const *rd_break_points[] = { nullptr };
static char const *rd_schemata[] = { nullptr };

static rd_System rd_system = {
  /* .v1 = */ {
    /* .id              */ "a2600",
    /* .cpus            */ rd_cpus,
    /* .memory_regions  */ rd_mem_regions,
    /* .filesystems     */ rd_filesystems,
    /* .break_points    */ rd_break_points,
    /* .schemata        */ rd_schemata,
    /* .get_content_info*/ nullptr,
  }
};

/* Build the memory-region list from the loaded cart.  Called after the
   console is (re)created, while the frontend has not yet cached topology. */
static void rd_setup_regions() {
  Cartridge &cart = stella.osystem().console().system().cart();

  size_t romsz = 0;
  cart.getImage(romsz);
  rd_rom_mem.v1.size = romsz;
  const uint32_t cramsz = cart.internalRamSize();
  rd_cartram_mem.v1.size = cramsz;

  unsigned n = 0;
  rd_mem_regions[n++] = &rd_wram_mem;
  rd_mem_regions[n++] = &rd_io_mem;
  if(romsz)  rd_mem_regions[n++] = &rd_rom_mem;
  if(cramsz) rd_mem_regions[n++] = &rd_cartram_mem;
  rd_mem_regions[n] = nullptr;
}

/* --- subscription system (breakpoints + stepping) ---
   The 6507 has no IRQ/NMI pins, so interrupt and memory-watch subscriptions
   are not implemented here. */

static bool rd_execution_hook_impl();

#define RD_MAX_SUBS 16

typedef struct {
  bool active;
  rd_Subscription sub;
  rd_SubscriptionID id;
  int call_depth;
} rd_SubSlot;

static rd_SubSlot rd_subs[RD_MAX_SUBS];
static rd_SubscriptionID rd_next_id = 1;

static bool rd_has_exec_sub = false;   /* any breakpoint or step sub active */
static bool rd_has_step_sub = false;   /* any step sub active */
static uint8_t rd_sub_bitset[8192];    /* 64K bits for point breakpoints */

static uint16_t rd_prev_pc;
static bool rd_prev_valid;

static void rd_recompute_sub_state() {
  rd_has_exec_sub = false;
  rd_has_step_sub = false;
  memset(rd_sub_bitset, 0, sizeof(rd_sub_bitset));

  for(int i = 0; i < RD_MAX_SUBS; i++) {
    rd_SubSlot *s = &rd_subs[i];
    if(!s->active) continue;
    switch(s->sub.type) {
    case RD_EVENT_BREAKPOINT: {
      rd_has_exec_sub = true;
      const uint16_t addr = (uint16_t)s->sub.breakpoint.address;
      rd_sub_bitset[addr >> 3] |= (1 << (addr & 7));
      break;
    }
    case RD_EVENT_STEP:
      rd_has_exec_sub = true;
      rd_has_step_sub = true;
      break;
    default:
      break;
    }
  }

  if(rd_has_exec_sub) {
    rd_execution_hook = rd_execution_hook_impl;
  } else {
    rd_execution_hook = nullptr;
    rd_prev_valid = false;
  }
}

/* Track JSR/RTS/RTI to maintain call depth for STEP_OVER / STEP_OUT. */
static void rd_track_calls(uint16_t old_pc) {
  if(!rd_has_step_sub) return;
  System *sys = rd_sys();
  if(!sys) return;
  const uint8_t opcode = rd_bus_read(sys, old_pc, false);
  int delta = 0;
  if(opcode == 0x20)       delta = +1;  /* JSR */
  else if(opcode == 0x60)  delta = -1;  /* RTS */
  else if(opcode == 0x40)  delta = -1;  /* RTI */
  if(delta)
    for(int i = 0; i < RD_MAX_SUBS; i++)
      if(rd_subs[i].active && rd_subs[i].sub.type == RD_EVENT_STEP)
        rd_subs[i].call_depth += delta;
}

static bool rd_fire_execution(uint16_t pc) {
  if(!rd_has_exec_sub || !debugger_if) return false;

  const bool pc_in_bitset = (rd_sub_bitset[pc >> 3] & (1 << (pc & 7))) != 0;
  if(!rd_has_step_sub && !pc_in_bitset) return false;

  for(int i = 0; i < RD_MAX_SUBS; i++) {
    rd_SubSlot *s = &rd_subs[i];
    if(!s->active) continue;

    if(s->sub.type == RD_EVENT_BREAKPOINT) {
      if((uint16_t)s->sub.breakpoint.address != pc) continue;
      rd_Event ev;
      memset(&ev, 0, sizeof(ev));
      ev.type = RD_EVENT_BREAKPOINT;
      ev.can_halt = true;
      ev.breakpoint.cpu = &rd_cpu;
      ev.breakpoint.address = pc;
      if(debugger_if->v1.handle_event(debugger_if->v1.user_data, s->id, &ev))
        return true;
    } else if(s->sub.type == RD_EVENT_STEP) {
      bool fire = false;
      switch(s->sub.step.mode) {
      case RD_STEP_INTO:           fire = true; break;
      case RD_STEP_INTO_SKIP_IRQ:  fire = true; break;  /* no IRQs on 2600 */
      case RD_STEP_OVER:           if(s->call_depth <= 0) fire = true; break;
      case RD_STEP_OUT:            if(s->call_depth <  0) fire = true; break;
      }
      if(fire) {
        rd_Event ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = RD_EVENT_STEP;
        ev.can_halt = true;
        ev.step.cpu = &rd_cpu;
        ev.step.address = pc;
        if(debugger_if->v1.handle_event(debugger_if->v1.user_data, s->id, &ev))
          return true;
      }
    }
  }
  return false;
}

static bool rd_execution_hook_impl() {
  /* Already halting — don't re-fire for the rest of this phase. */
  if(rd_halt_flag) return false;

  M6502 *cpu = rd_m6502();
  if(!cpu) return false;
  const uint16_t pc = cpu->rdRegPC();

  if(rd_prev_valid) rd_track_calls(rd_prev_pc);
  rd_prev_pc = pc;
  rd_prev_valid = true;

  return rd_fire_execution(pc);
}

static rd_SubscriptionID rd_subscribe(rd_Subscription const *sub) {
  if(!sub) return -1;
  if(sub->type != RD_EVENT_BREAKPOINT && sub->type != RD_EVENT_STEP)
    return -1;  /* interrupt/memory watchpoints unsupported on 2600 */

  for(int i = 0; i < RD_MAX_SUBS; i++) {
    if(!rd_subs[i].active) {
      rd_subs[i].active = true;
      rd_subs[i].sub = *sub;
      rd_subs[i].id = rd_next_id++;
      rd_subs[i].call_depth = 0;
      rd_recompute_sub_state();
      return rd_subs[i].id;
    }
  }
  return -1;
}

static void rd_unsubscribe(rd_SubscriptionID id) {
  for(int i = 0; i < RD_MAX_SUBS; i++) {
    if(rd_subs[i].active && rd_subs[i].id == id) {
      rd_subs[i].active = false;
      rd_recompute_sub_state();
      return;
    }
  }
}

/* --- console lifecycle (called from reset_system) --- */

static void rd_on_console_destroyed() {
  rd_game_loaded = false;
  rd_execution_hook = nullptr;
  rd_prev_valid = false;
}

static void rd_on_console_created() {
  rd_game_loaded = true;
  rd_prev_valid = false;
  rd_halt_flag = false;
  rd_setup_regions();
}

extern "C" RETRO_API void rd_set_debugger(rd_DebuggerIf *const dbg_if) {
  debugger_if = dbg_if;
  if(debugger_if) {
    debugger_if->core_api_version = RD_API_VERSION;
    debugger_if->v1.system = &rd_system;
    debugger_if->v1.subscribe = rd_subscribe;
    debugger_if->v1.unsubscribe = rd_unsubscribe;
  }
}

static retro_proc_address_t core_get_proc_address(const char *sym) {
  if(strcmp(sym, "rd_set_debugger") == 0)
    return (retro_proc_address_t)rd_set_debugger;
  return nullptr;
}

/* ====================================================================== */

void retro_set_environment(retro_environment_t cb)
{
  environ_cb = cb;

  {
    struct retro_get_proc_address_interface proc_iface = { core_get_proc_address };
    environ_cb(RETRO_ENVIRONMENT_SET_PROC_ADDRESS_CALLBACK, &proc_iface);
  }

  static struct retro_variable variables[] = {
    // Adding more variables and rearranging them is safe.
    { "stella_console", "Console display; auto|ntsc|pal|secam|ntsc50|pal60|secam60" },
    { "stella_palette", "Palette colors; standard|z26|user|custom" },
    { "stella_filter", "TV effects; disabled|composite|s-video|rgb|badly adjusted" },
    { "stella_crop_hoverscan", "Crop horizontal overscan; disabled|enabled" },
    { "stella_crop_voverscan", "Crop vertical overscan; 0|1|2|3|4|5|6|7|8|9|10|11|12|13|14|15|16|17|18|19|20|21|22|23|24" },
    { "stella_ntsc_aspect", "NTSC aspect %; par|100|101|102|103|104|105|106|107|108|109|110|111|112|113|114|115|116|117|118|119|120|121|122|123|124|125|75|76|77|78|79|80|81|82|83|84|85|86|87|88|89|90|91|92|93|94|95|96|97|98|99" },
    { "stella_pal_aspect", "PAL aspect %; par|100|101|102|103|104|105|106|107|108|109|110|111|112|113|114|115|116|117|118|119|120|121|122|123|124|125|75|76|77|78|79|80|81|82|83|84|85|86|87|88|89|90|91|92|93|94|95|96|97|98|99" },
    { "stella_stereo", "Stereo sound; auto|off|on" },
    { "stella_phosphor", "Phosphor mode; auto|off|on" },
    { "stella_phosphor_blend", "Phosphor blend %; 60|65|70|75|80|85|90|95|100|0|5|10|15|20|25|30|35|40|45|50|55" },
    { "stella_paddle_mouse_sensitivity", "Paddle mouse sensitivity; 20|21|22|23|24|25|26|27|28|29|30|31|32|33|34|35|36|37|38|39|40|41|42|43|44|45|46|47|48|49|50|1|2|3|4|5|6|7|8|9|10|11|12|13|14|15|16|17|18|19" },
    { "stella_paddle_joypad_sensitivity", "Paddle joypad sensitivity; 3|4|5|6|7|8|9|10|11|12|13|14|15|16|17|18|19|20|1|2" },
    { "stella_paddle_analog_sensitivity", "Paddle analog sensitivity; 20|21|22|23|24|25|26|27|28|29|30|0|1|2|3|4|5|6|7|8|9|10|11|12|13|14|15|16|17|18|19" },
    { "stella_paddle_analog_deadzone", "Paddle analog deadzone; 15|16|17|18|19|20|21|22|23|24|25|26|27|28|29|30|0|1|2|3|4|5|6|7|8|9|10|11|12|13|14" },
    { "stella_paddle_analog_absolute", "Paddle analog absolute; disabled|enabled" },
    { "stella_lightgun_crosshair", "Lightgun crosshair; disabled|enabled" },
    { NULL, NULL },
  };

  environ_cb(RETRO_ENVIRONMENT_SET_VARIABLES, variables);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
static void fallback_log(enum retro_log_level level, const char *fmt, ...)
{
  (void)level;
  va_list va;
  va_start(va, fmt);
  vfprintf(stderr, fmt, va);
  va_end(va);
}

void retro_init()
{
  struct retro_log_callback log;
  unsigned level = 4;

  log_cb = fallback_log;
  if(environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log))
    log_cb = log.log;

  environ_cb(RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL, &level);
  libretro_supports_bitmasks = environ_cb(RETRO_ENVIRONMENT_GET_INPUT_BITMASKS, NULL);
}

static const struct retro_controller_description controllers[] = {
    { "Automatic", RETRO_DEVICE_JOYPAD },
    { "None", RETRO_DEVICE_NONE },
    { NULL, 0 }
};

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
bool retro_load_game(const struct retro_game_info *info)
{
  enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;

  static const struct retro_controller_info controller_info[] = {
    { controllers, sizeof(controllers) / sizeof(controllers[0]) },
    { controllers, sizeof(controllers) / sizeof(controllers[0]) },
    { controllers, sizeof(controllers) / sizeof(controllers[0]) },
    { controllers, sizeof(controllers) / sizeof(controllers[0]) },
    { NULL, 0 }
  };

  #define RETRO_DESCRIPTOR_BLOCK(_user) \
  { _user, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,     "Up" }, \
  { _user, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,   "Down" }, \
  { _user, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,   "Left" }, \
  { _user, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT,  "Right" }, \
  { _user, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,      "Fire" }, \
  { _user, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,      "Trigger" }, \
  { _user, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,      "Booster" }, \
  { _user, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Select" }, \
  { _user, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,  "Reset" }, \
  { _user, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,      "Left Difficulty A" }, \
  { _user, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,      "Right Difficulty A" }, \
  { _user, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2,     "Left Difficulty B" }, \
  { _user, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,     "Right Difficulty B" }, \
  { _user, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3,     "Color" }, \
  { _user, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3,     "Black/White" }, \
  { _user, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,   RETRO_DEVICE_ID_ANALOG_X, "Axis" } \

  #define RETRO_DESCRIPTOR_EXTRA_BLOCK(_user) \
  { _user, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,   "Left" }, \
  { _user, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT,  "Right" }, \
  { _user, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,      "Fire" }, \
  { _user, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,   RETRO_DEVICE_ID_ANALOG_X, "Axis" } \

  static struct retro_input_descriptor input_descriptors[] =
  {
    RETRO_DESCRIPTOR_BLOCK(0),
    RETRO_DESCRIPTOR_BLOCK(1),
    RETRO_DESCRIPTOR_EXTRA_BLOCK(2),
    RETRO_DESCRIPTOR_EXTRA_BLOCK(3),
    {0, 0, 0, 0, NULL},
  };
  #undef RETRO_DESCRIPTOR_BLOCK
  #undef RETRO_DESCRIPTOR_EXTRA_BLOCK

  if(!info || info->size > stella.getROMMax()) return false;

  // Send controller infos to libretro
  environ_cb(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO, (void*)controller_info);
  // Send controller input descriptions to libretro
  environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, (void*)input_descriptors);

  if(!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
  {
    if(log_cb) log_cb(RETRO_LOG_INFO, "XRGB8888 is not supported.\n");
    return false;
  }

  stella.setROM(info->path, info->data, info->size);

  /* reset_system() creates the console and wires up retrodebug regions. */
  return reset_system();
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
bool retro_load_game_special(unsigned game_type, const struct retro_game_info *info, size_t num_info)
{
  return false;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void retro_reset()
{
  stella.reset();
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void retro_run()
{
  /* retrodebug: clear any halt from the previous breakpoint so this frame can
     run.  The backend's skip map steps the CPU past the just-hit address. */
  rd_halt_flag = false;

  bool updated = false;

  if(environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
    update_variables();

  if(system_reset)
  {
    reset_system();
    update_system_av();
    return;
  }

  update_input();
  stella.runFrame();

  if(stella_lightgun_crosshair && input_crosshair[0] && input_crosshair[1])
    draw_crosshair(input_crosshair[0], input_crosshair[1], 0x0000ff);

  if(stella.getVideoResize())
    update_geometry();

  if(stella.getVideoReady())
    video_cb(reinterpret_cast<uInt32*>(stella.getVideoBuffer()) + crop_left + (crop_top * stella.getVideoWidthMax()),
        stella.getVideoWidth() - crop_left,
        stella.getVideoHeight() - crop_top * 2,
        stella.getVideoPitch());

  if(stella.getAudioReady())
    audio_batch_cb(stella.getAudioBuffer(), stella.getAudioSize());
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void retro_unload_game()
{
  rd_game_loaded = false;
  rd_execution_hook = nullptr;
  stella.destroy();
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void retro_deinit()
{
  stella.destroy();
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
size_t retro_serialize_size()
{
  int runahead = -1;
  if(environ_cb(RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE, &runahead))
  {
    // maximum state size possible
    if(runahead & 4)
      return 0x100000;
  }

  return stella.getStateSize();
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
bool retro_serialize(void *data, size_t size)
{
  return stella.saveState(data, size);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
bool retro_unserialize(const void *data, size_t size)
{
  return stella.loadState(data, size);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void *retro_get_memory_data(unsigned id)
{
  switch (id)
  {
    case RETRO_MEMORY_SYSTEM_RAM:
      return stella.getRAM();

    default:
      return NULL;
  }
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
size_t retro_get_memory_size(unsigned id)
{
  switch (id)
  {
    case RETRO_MEMORY_SYSTEM_RAM:
      return stella.getRAMSize();

    default:
      return 0;
  }
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void retro_cheat_reset()
{
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void retro_cheat_set(unsigned index, bool enabled, const char *code)
{
}
