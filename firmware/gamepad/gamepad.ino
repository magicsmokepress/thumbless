/*
 * ============================================================================
 *  Agent BLE Gamepad Simulator  —  ESP32-C3
 * ============================================================================
 *
 *  Turns an ESP32-C3 into a *simulated* Bluetooth LE HID gamepad that any
 *  BLE-gamepad-capable host (Windows, Linux, macOS, Android, Raspberry Pi,
 *  Steam, RetroArch/emulators, browser Gamepad-API games...) can pair with
 *  and use like a normal controller.
 *
 *  Instead of physical buttons and sticks, the inputs are driven over the
 *  ESP32-C3's built-in USB serial link by "Agent" (a program / agent) using
 *  a simple line-based ASCII command protocol (see PROTOCOL below and README).
 *
 *      [ Agent ] --USB serial--> [ ESP32-C3 ] --BLE HID--> [ PC / phone ]
 *
 *  NOTE ON BLUETOOTH: the ESP32-C3 only has Bluetooth *LE*, not Classic BT.
 *  So this presents a standard BLE HID gamepad (great for PCs/phones). It
 *  cannot impersonate a real Xbox/PlayStation/Switch pad (those use Classic
 *  BT). BLE HID gamepads are recognised by Windows joy.cpl, Steam Input,
 *  Android, Linux (evdev/js), etc.
 *
 * ----------------------------------------------------------------------------
 *  LIBRARY DEPENDENCY
 * ----------------------------------------------------------------------------
 *  ESP32-BLE-Gamepad by lemmingDev (v0.7.x) + its NimBLE-Arduino dependency.
 *  Install both via Arduino Library Manager. API used here is verified against
 *  BleGamepad.h / BleGamepadConfiguration.h.
 *
 * ----------------------------------------------------------------------------
 *  ARDUINO IDE BOARD SETTINGS (important!)
 * ----------------------------------------------------------------------------
 *   Board:              "ESP32C3 Dev Module" (or your specific C3 board)
 *   USB CDC On Boot:    "Enabled"     <-- required if Agent talks to the
 *                                          chip's NATIVE USB port (GPIO18/19).
 *                                          Then `Serial` == the USB link.
 *                       If instead you use a board whose USB goes through a
 *                       CH340/CP210x UART bridge, leave it as you like; `Serial`
 *                       then maps to UART0. Either way this sketch just uses
 *                       `Serial`, so match the setting to your cabling.
 *   Partition Scheme:   any with >= ~1.4 MB app (default is fine)
 *   Upload Speed:       921600 (or 115200 if flaky)
 *
 * ----------------------------------------------------------------------------
 *  PROTOCOL (quick reference — full docs in README.md)
 * ----------------------------------------------------------------------------
 *  - One command per line, terminated by '\n' (CR is ignored).
 *  - Tokens are space-separated; command keywords are case-insensitive.
 *  - Put several sub-commands on one line with ';' to apply them atomically
 *    (a single HID report is sent after the whole line):
 *        PRESS A ; LX -32000 ; DPAD UP
 *  - Lines beginning with '#' are treated as comments (ignored).
 *  - Every line yields exactly one terminal reply: "OK" or "ERR <reason>".
 *    Info output (STATUS/HELP + async events) is printed on '#'-prefixed lines
 *    *before* the terminal reply.
 *
 *  Buttons (numbers 1..16, or aliases): A B X Y LB RB LT RT BACK START L3 R3
 *      GUIDE  (aliases are just labels for button indices — the host decides
 *      what each index does; remap freely on the host side.)
 *
 *  Axes (signed, -32767..32767, 0 = centre):
 *      LX v | LY v            left stick X / Y      (y: -up / +down)
 *      RX v | RY v            right stick X / Y
 *      LSTICK x y             set left stick X and Y at once
 *      RSTICK x y             set right stick X and Y at once
 *      LT v | RT v            analog triggers (rest 0 .. full 32767)
 *
 *  D-pad / hat:
 *      DPAD dir               dir = C U UR R DR D DL L UL  (or CENTER/UP/...)
 *      HAT n                  raw hat 0..8
 *
 *  Actions:
 *      PRESS  <btns>          hold button(s) down          e.g. PRESS A  |  PRESS A,B
 *      RELEASE <btns>|ALL     release button(s) / all
 *      TAP <btns> [ms]        press, wait ms (default 60), release
 *      CENTER                 sticks+triggers to rest, hat centre (buttons kept)
 *      RESET                  release everything and centre all axes
 *      WAIT ms                blocking delay (handy inside scripted lines)
 *
 *  Meta:
 *      PING                   -> OK   (liveness check)
 *      STATUS                 -> current state on '#'-lines, then OK
 *      HELP                   -> command list on '#'-lines, then OK
 *      ECHO ON|OFF            enable/disable the "OK"/"ERR" replies
 *
 * ============================================================================
 */

#include <Arduino.h>
#include <BleGamepad.h>

// ------------------------------- Configuration ------------------------------
static const char*    FW_VERSION    = "1.0.0";
static const char*    DEVICE_NAME   = "Agent Pad";
static const char*    DEVICE_MFR    = "Agent";
static const uint8_t  BATTERY_LEVEL = 100;

static const uint8_t  BUTTON_COUNT  = 16;         // exposed buttons (1..16)
static const int16_t  AXIS_MIN      = -32767;     // symmetric range -> centre 0
static const int16_t  AXIS_MAX      =  32767;

static const uint32_t SERIAL_BAUD   = 115200;     // baud is ignored on native USB-CDC
static const uint16_t DEFAULT_TAP_MS = 60;
static const size_t   LINE_BUF      = 160;        // max chars per command line

// Optional status LED. Set to a GPIO to blink on activity / show link state.
// Left at -1 (disabled) to stay safe across C3 boards (many use an addressable
// RGB LED on GPIO8 which a plain digitalWrite would not drive correctly).
static const int      STATUS_LED_PIN = -1;

// ------------------------------- Globals ------------------------------------
BleGamepad bleGamepad(DEVICE_NAME, DEVICE_MFR, BATTERY_LEVEL);
// Kept at file scope (not on setup()'s stack) in case begin() retains the ptr.
BleGamepadConfiguration cfg;

// Authoritative shadow state (mirrors what we push into the HID report).
int16_t  s_leftX = 0,  s_leftY = 0;      // left stick   -> HID X / Y
int16_t  s_rightX = 0, s_rightY = 0;     // right stick  -> HID Z / RZ
int16_t  s_trigL = 0,  s_trigR = 0;      // triggers     -> HID RX / RY
int8_t   s_hat = 0;                      // 0=centre, 1..8 clockwise from up
uint32_t s_buttons = 0;                  // bitmask of pressed buttons (bit0 = btn1)

bool     s_echo = true;                  // print OK/ERR replies
bool     s_wasConnected = false;

// --------------------------- Small helpers ----------------------------------
static inline int16_t clampAxis(long v) {
  if (v < AXIS_MIN) return AXIS_MIN;
  if (v > AXIS_MAX) return AXIS_MAX;
  return (int16_t)v;
}
static inline int16_t clampTrig(long v) {
  if (v < 0) return 0;
  if (v > AXIS_MAX) return AXIS_MAX;
  return (int16_t)v;
}

// Push the full shadow state into the library and emit one HID report.
static void applyAndSend() {
  bleGamepad.setLeftThumb(s_leftX, s_leftY);   // X, Y
  bleGamepad.setRightThumb(s_rightX, s_rightY); // Z, RZ
  bleGamepad.setLeftTrigger(s_trigL);          // RX
  bleGamepad.setRightTrigger(s_trigR);         // RY
  bleGamepad.setHat((signed char)s_hat);
  bleGamepad.sendReport();                     // buttons already set via press()/release()
}

static void reply(bool ok, const char* err = nullptr) {
  if (!s_echo) return;
  if (ok) Serial.println("OK");
  else    { Serial.print("ERR "); Serial.println(err ? err : "bad-command"); }
}

// Map a button token (alias or number) to a button index 1..BUTTON_COUNT, or 0.
static uint8_t parseButton(const char* t) {
  // numeric?
  if (t[0] >= '0' && t[0] <= '9') {
    long n = strtol(t, nullptr, 10);
    if (n >= 1 && n <= BUTTON_COUNT) return (uint8_t)n;
    return 0;
  }
  // aliases (Xbox-style labels -> indices). Purely a convenience mapping.
  struct { const char* name; uint8_t idx; } kMap[] = {
    {"A",1},{"B",2},{"X",3},{"Y",4},
    {"LB",5},{"RB",6},{"LT",7},{"RT",8},
    {"BACK",9},{"SELECT",9},{"START",10},{"MENU",10},
    {"L3",11},{"LS",11},{"R3",12},{"RS",12},
    {"GUIDE",13},{"HOME",13},
  };
  for (auto& e : kMap) if (strcasecmp(t, e.name) == 0) return e.idx;
  return 0;
}

// Parse a comma-separated button list "A,B,3" into up to 8 indices.
// Returns count found; writes indices into out[]. Any bad token -> returns -1.
static int parseButtonList(char* list, uint8_t* out, int maxOut) {
  int n = 0;
  char* save = nullptr;
  for (char* tok = strtok_r(list, ",", &save); tok; tok = strtok_r(nullptr, ",", &save)) {
    if (n >= maxOut) return -1;
    uint8_t b = parseButton(tok);
    if (b == 0) return -1;
    out[n++] = b;
  }
  return n == 0 ? -1 : n;
}

// D-pad direction word/number -> hat value 0..8.
static int parseHat(const char* t) {
  if (t[0] >= '0' && t[0] <= '9') {
    long n = strtol(t, nullptr, 10);
    return (n >= 0 && n <= 8) ? (int)n : -1;
  }
  struct { const char* name; int v; } kDir[] = {
    {"C",0},{"CENTER",0},{"CENTRE",0},{"NONE",0},
    {"U",1},{"UP",1},{"N",1},
    {"UR",2},{"UPRIGHT",2},{"NE",2},
    {"R",3},{"RIGHT",3},{"E",3},
    {"DR",4},{"DOWNRIGHT",4},{"SE",4},
    {"D",5},{"DOWN",5},{"S",5},
    {"DL",6},{"DOWNLEFT",6},{"SW",6},
    {"L",7},{"LEFT",7},{"W",7},
    {"UL",8},{"UPLEFT",8},{"NW",8},
  };
  for (auto& e : kDir) if (strcasecmp(t, e.name) == 0) return e.v;
  return -1;
}

static void doPressList(uint8_t* b, int n)   { for (int i=0;i<n;i++){ bleGamepad.press(b[i]);   s_buttons |=  (1UL << (b[i]-1)); } }
static void doReleaseList(uint8_t* b, int n) { for (int i=0;i<n;i++){ bleGamepad.release(b[i]); s_buttons &= ~(1UL << (b[i]-1)); } }

static void releaseAllButtons() {
  for (uint8_t i = 1; i <= BUTTON_COUNT; i++) bleGamepad.release(i);
  s_buttons = 0;
}

static void centerAxes() {
  s_leftX = s_leftY = s_rightX = s_rightY = 0;
  s_trigL = s_trigR = 0;
  s_hat = 0;
}

static void printStatus() {
  Serial.print("# name=");        Serial.print(DEVICE_NAME);
  Serial.print(" fw=");           Serial.print(FW_VERSION);
  Serial.print(" connected=");    Serial.print(bleGamepad.isConnected() ? 1 : 0);
  Serial.print(" buttons=");      Serial.print(BUTTON_COUNT);
  Serial.print(" axis=[");        Serial.print(AXIS_MIN); Serial.print(","); Serial.print(AXIS_MAX); Serial.print("]");
  Serial.println();
  Serial.print("# pressed=0x");   Serial.print(s_buttons, HEX);
  Serial.print(" LX=");  Serial.print(s_leftX);
  Serial.print(" LY=");  Serial.print(s_leftY);
  Serial.print(" RX=");  Serial.print(s_rightX);
  Serial.print(" RY=");  Serial.print(s_rightY);
  Serial.print(" LT=");  Serial.print(s_trigL);
  Serial.print(" RT=");  Serial.print(s_trigR);
  Serial.print(" HAT="); Serial.print(s_hat);
  Serial.println();
}

static void printHelp() {
  Serial.println(F("# Commands:"));
  Serial.println(F("#   PRESS <btns> | RELEASE <btns>|ALL | TAP <btns> [ms]"));
  Serial.println(F("#   LX/LY/RX/RY <v>  (-32767..32767)  | LSTICK x y | RSTICK x y"));
  Serial.println(F("#   LT/RT <v>  (0..32767)             | DPAD <dir> | HAT <0..8>"));
  Serial.println(F("#   CENTER | RESET | WAIT <ms>"));
  Serial.println(F("#   PING | STATUS | HELP | ECHO ON|OFF"));
  Serial.println(F("#   btns: 1..16 or A B X Y LB RB LT RT BACK START L3 R3 GUIDE (comma-list ok)"));
  Serial.println(F("#   dir : C U UR R DR D DL L UL"));
  Serial.println(F("#   Chain with ';' for one atomic report:  PRESS A ; LX -32000 ; DPAD UP"));
}

// ---------------------- Single sub-command executor -------------------------
// Executes one sub-command (already trimmed). Returns:
//   1 = ok & state changed (caller will send a report at end of line)
//   0 = ok & no report needed (query/meta)
//  -1 = error (err set)
static int execOne(char* cmd, const char** err) {
  // Tokenise into argv (max 4 tokens is enough for our commands).
  char* argv[5]; int argc = 0;
  char* save = nullptr;
  for (char* t = strtok_r(cmd, " \t", &save); t && argc < 5; t = strtok_r(nullptr, " \t", &save))
    argv[argc++] = t;
  if (argc == 0) return 0;                 // empty sub-command -> nothing

  char* op = argv[0];
  for (char* p = op; *p; ++p) *p = toupper(*p);

  // ---- meta / query ----
  if (!strcmp(op, "PING"))   { return 0; }
  if (!strcmp(op, "STATUS")) { printStatus(); return 0; }
  if (!strcmp(op, "HELP"))   { printHelp();   return 0; }
  if (!strcmp(op, "ECHO")) {
    if (argc < 2) { *err = "echo-needs-arg"; return -1; }
    if (!strcasecmp(argv[1], "ON"))  { s_echo = true;  return 0; }
    if (!strcasecmp(argv[1], "OFF")) { s_echo = false; return 0; }
    *err = "echo-on-or-off"; return -1;
  }

  // ---- buttons ----
  if (!strcmp(op, "PRESS") || !strcmp(op, "RELEASE")) {
    bool press = (op[0] == 'P');
    if (argc < 2) { *err = "need-button"; return -1; }
    if (!press && !strcasecmp(argv[1], "ALL")) { releaseAllButtons(); return 1; }
    uint8_t list[8]; int n = parseButtonList(argv[1], list, 8);
    if (n < 0) { *err = "bad-button"; return -1; }
    if (press) doPressList(list, n); else doReleaseList(list, n);
    return 1;
  }
  if (!strcmp(op, "TAP")) {
    if (argc < 2) { *err = "need-button"; return -1; }
    uint8_t list[8]; int n = parseButtonList(argv[1], list, 8);
    if (n < 0) { *err = "bad-button"; return -1; }
    long ms = (argc >= 3) ? strtol(argv[2], nullptr, 10) : DEFAULT_TAP_MS;
    if (ms < 0) ms = 0; if (ms > 5000) ms = 5000;
    doPressList(list, n);   applyAndSend();
    delay((uint32_t)ms);
    doReleaseList(list, n); applyAndSend();
    return 0;               // TAP already emitted its own reports
  }

  // ---- axes ----
  if (!strcmp(op, "LX")) { if (argc<2){*err="need-value";return -1;} s_leftX  = clampAxis(strtol(argv[1],0,10)); return 1; }
  if (!strcmp(op, "LY")) { if (argc<2){*err="need-value";return -1;} s_leftY  = clampAxis(strtol(argv[1],0,10)); return 1; }
  if (!strcmp(op, "RX")) { if (argc<2){*err="need-value";return -1;} s_rightX = clampAxis(strtol(argv[1],0,10)); return 1; }
  if (!strcmp(op, "RY")) { if (argc<2){*err="need-value";return -1;} s_rightY = clampAxis(strtol(argv[1],0,10)); return 1; }
  if (!strcmp(op, "LT")) { if (argc<2){*err="need-value";return -1;} s_trigL  = clampTrig(strtol(argv[1],0,10)); return 1; }
  if (!strcmp(op, "RT")) { if (argc<2){*err="need-value";return -1;} s_trigR  = clampTrig(strtol(argv[1],0,10)); return 1; }
  if (!strcmp(op, "LSTICK")) {
    if (argc < 3) { *err = "need-x-y"; return -1; }
    s_leftX  = clampAxis(strtol(argv[1],0,10));
    s_leftY  = clampAxis(strtol(argv[2],0,10));
    return 1;
  }
  if (!strcmp(op, "RSTICK")) {
    if (argc < 3) { *err = "need-x-y"; return -1; }
    s_rightX = clampAxis(strtol(argv[1],0,10));
    s_rightY = clampAxis(strtol(argv[2],0,10));
    return 1;
  }

  // ---- hat / dpad ----
  if (!strcmp(op, "DPAD") || !strcmp(op, "HAT")) {
    if (argc < 2) { *err = "need-dir"; return -1; }
    int h = parseHat(argv[1]);
    if (h < 0) { *err = "bad-dir"; return -1; }
    s_hat = (int8_t)h;
    return 1;
  }

  // ---- bulk ----
  if (!strcmp(op, "CENTER") || !strcmp(op, "CENTRE")) { centerAxes(); return 1; }
  if (!strcmp(op, "RESET")) { releaseAllButtons(); centerAxes(); return 1; }
  if (!strcmp(op, "WAIT")) {
    if (argc < 2) { *err = "need-ms"; return -1; }
    long ms = strtol(argv[1], nullptr, 10);
    if (ms < 0) ms = 0; if (ms > 10000) ms = 10000;
    delay((uint32_t)ms);
    return 0;
  }

  *err = "unknown-command";
  return -1;
}

// ------------------------ Whole-line processor ------------------------------
static void processLine(char* line) {
  // strip comments / blanks
  // (a leading '#' means the whole line is a comment)
  // trim leading spaces
  while (*line == ' ' || *line == '\t') line++;
  if (*line == '\0' || *line == '#') { reply(true); return; }

  bool needSend = false;
  bool anyErr = false;
  const char* err = "bad-command";

  // split on ';' into sub-commands, execute in order
  char* save = nullptr;
  for (char* sub = strtok_r(line, ";", &save); sub; sub = strtok_r(nullptr, ";", &save)) {
    int r = execOne(sub, &err);
    if (r < 0) { anyErr = true; break; }
    if (r > 0) needSend = true;
  }

  if (anyErr) { reply(false, err); return; }
  if (needSend) applyAndSend();
  reply(true);
}

// --------------------------------- Arduino ----------------------------------
char   g_line[LINE_BUF];
size_t g_len = 0;
bool   g_overflow = false;   // true while discarding an over-long line

void setup() {
  Serial.begin(SERIAL_BAUD);

  if (STATUS_LED_PIN >= 0) { pinMode(STATUS_LED_PIN, OUTPUT); digitalWrite(STATUS_LED_PIN, LOW); }

  cfg.setControllerType(CONTROLLER_TYPE_GAMEPAD);   // 0x05
  cfg.setAutoReport(false);                         // we call sendReport() ourselves
  cfg.setButtonCount(BUTTON_COUNT);
  cfg.setHatSwitchCount(1);
  // enable X, Y, Z, RX, RY, RZ  (sticks + triggers); sliders off
  cfg.setWhichAxes(true, true, true, true, true, true, false, false);
  cfg.setAxesMin(AXIS_MIN);                          // -32767  -> centre 0
  cfg.setAxesMax(AXIS_MAX);                          //  32767

  bleGamepad.begin(&cfg);

  // Greeting (dropped harmlessly if the host port isn't open yet).
  Serial.print("# Agent BLE Gamepad Simulator v"); Serial.print(FW_VERSION);
  Serial.println(" ready. Type HELP.");
  Serial.println("# Advertising as BLE HID gamepad; pair from your host now.");
}

void loop() {
  // ---- report BLE connection state changes as async '#' events ----
  bool conn = bleGamepad.isConnected();
  if (conn != s_wasConnected) {
    s_wasConnected = conn;
    Serial.println(conn ? "# CONNECTED" : "# DISCONNECTED");
    if (STATUS_LED_PIN >= 0) digitalWrite(STATUS_LED_PIN, conn ? HIGH : LOW);
  }

  // ---- read + dispatch complete lines from the USB serial link ----
  while (Serial.available() > 0) {
    char c = (char)Serial.read();
    if (c == '\r') continue;                 // ignore CR
    if (c == '\n') {
      if (g_overflow) {                      // finished discarding a long line
        g_overflow = false;
        g_len = 0;
        reply(false, "line-too-long");
      } else {
        g_line[g_len] = '\0';
        processLine(g_line);
        g_len = 0;
      }
    } else if (!g_overflow) {
      if (g_len < LINE_BUF - 1) g_line[g_len++] = c;
      else g_overflow = true;                // too long: discard until newline
    }
  }
}
