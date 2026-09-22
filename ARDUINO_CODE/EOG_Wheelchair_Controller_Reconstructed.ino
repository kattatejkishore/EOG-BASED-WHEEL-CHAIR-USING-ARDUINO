/*
  ================================================================
  EOG-BASED WHEELCHAIR CONTROL - ARDUINO UNO
  ================================================================

  Reconstructed implementation based on the supplied project PDF.

  Documented methodology implemented:
    1. Horizontal EOG -> A0
    2. Vertical EOG   -> A1
    3. Adaptive baseline using 50 resting samples
    4. 5-sample moving-average FIR filter
    5. Primary threshold       = 10
    6. Confirmation threshold  = 8
    7. Stability/history check = previous 3 readings
    8. Orthogonal dominance check
    9. Discrete movement duration = 3000 ms
   10. Motor lockout while a command is executing
   11. TB6612FNG dual H-bridge
   12. Four LEDs for movement feedback
   13. Serial telemetry

  IMPORTANT:
  This is a reconstructed implementation, NOT the original missing
  project code. Pin assignments below are assumed basic connections
  and should be changed if your real circuit is different.

  TEST SAFETY:
  First test with the wheelchair wheels lifted from the ground.
  Do NOT test with a person sitting on the wheelchair until the
  signal polarity, thresholds, motor direction and emergency stop
  behavior have been verified.
*/

// ================================================================
// 1. EOG INPUT PINS
// ================================================================

const int EOG_HORIZONTAL = A0;   // Horizontal EOG amplifier output
const int EOG_VERTICAL   = A1;   // Vertical EOG amplifier output


// ================================================================
// 2. TB6612FNG MOTOR DRIVER
//
// Assumed connection:
//
// TB6612FNG       Arduino UNO
// --------------------------------
// PWMA            D5
// AIN1            D7
// AIN2            D8
// PWMB            D6
// BIN1            D9
// BIN2            D10
// STBY            D4
//
// Motor A = LEFT motor
// Motor B = RIGHT motor
// ================================================================

const int PWMA = 5;
const int AIN1 = 7;
const int AIN2 = 8;

const int PWMB = 6;
const int BIN1 = 9;
const int BIN2 = 10;

const int STBY = 4;


// ================================================================
// 3. LED FEEDBACK
//
// Assumed:
// D2 -> Forward LED
// D3 -> Backward LED
// D11 -> Left LED
// D12 -> Right LED
// ================================================================

const int LED_FORWARD  = 2;
const int LED_BACKWARD = 3;
const int LED_LEFT     = 11;
const int LED_RIGHT    = 12;


// ================================================================
// 4. EOG ALGORITHM PARAMETERS
// ================================================================

const int CALIBRATION_SAMPLES = 50;
const int SAMPLE_SIZE = 5;
const int HISTORY_SIZE = 3;

const float PRIMARY_THRESHOLD = 10.0;
const float CONFIRM_THRESHOLD = 8.0;

// Stability tolerance.
// The PDF specifies comparison with the average of the
// previous 3 readings but does not give an exact tolerance.
// This is therefore an implementation assumption.
const float STABILITY_TOLERANCE = 4.0;

// Movement duration specified by the project document.
const unsigned long MOVEMENT_DURATION = 3000;

// Conservative motor speed for prototype testing.
const int MOTOR_SPEED = 120;


// ================================================================
// 5. EOG POLARITY
//
// Depending on electrode placement and amplifier wiring,
// looking left/right or up/down may produce either polarity.
//
// Start with +1.
// If LEFT and RIGHT are reversed, change H_POLARITY to -1.
// If FORWARD and BACKWARD are reversed, change V_POLARITY to -1.
// ================================================================

const int H_POLARITY = 1;
const int V_POLARITY = 1;


// ================================================================
// 6. MOVEMENT ENUMERATION
// ================================================================

enum Movement {
  NONE,
  FORWARD,
  BACKWARD,
  LEFT,
  RIGHT
};


// ================================================================
// 7. GLOBAL VARIABLES
// ================================================================

// Adaptive baseline
float baselineH = 0.0;
float baselineV = 0.0;

// Moving-average buffers
float hSamples[SAMPLE_SIZE];
float vSamples[SAMPLE_SIZE];

int sampleIndex = 0;
bool filterReady = false;

// History for stability confirmation
float hHistory[HISTORY_SIZE];
float vHistory[HISTORY_SIZE];

int historyIndex = 0;
int historyCount = 0;

// Movement state
bool isMoving = false;
Movement activeMovement = NONE;

unsigned long movementStartTime = 0;

// Telemetry timer
unsigned long lastTelemetry = 0;


// ================================================================
// 8. MOVEMENT NAME
// ================================================================

const char* getMovementName(Movement movement) {

  switch (movement) {

    case FORWARD:
      return "FORWARD";

    case BACKWARD:
      return "BACKWARD";

    case LEFT:
      return "LEFT";

    case RIGHT:
      return "RIGHT";

    default:
      return "NONE";
  }
}


// ================================================================
// 9. LED CONTROL
// ================================================================

void turnOffLEDs() {

  digitalWrite(LED_FORWARD, LOW);
  digitalWrite(LED_BACKWARD, LOW);
  digitalWrite(LED_LEFT, LOW);
  digitalWrite(LED_RIGHT, LOW);
}


void showMovementLED(Movement movement) {

  turnOffLEDs();

  switch (movement) {

    case FORWARD:
      digitalWrite(LED_FORWARD, HIGH);
      break;

    case BACKWARD:
      digitalWrite(LED_BACKWARD, HIGH);
      break;

    case LEFT:
      digitalWrite(LED_LEFT, HIGH);
      break;

    case RIGHT:
      digitalWrite(LED_RIGHT, HIGH);
      break;

    default:
      break;
  }
}


// ================================================================
// 10. MOTOR CONTROL
// ================================================================

void stopMotors() {

  analogWrite(PWMA, 0);
  analogWrite(PWMB, 0);

  digitalWrite(AIN1, LOW);
  digitalWrite(AIN2, LOW);

  digitalWrite(BIN1, LOW);
  digitalWrite(BIN2, LOW);
}


void moveForward() {

  // LEFT MOTOR -> forward
  digitalWrite(AIN1, HIGH);
  digitalWrite(AIN2, LOW);

  // RIGHT MOTOR -> forward
  digitalWrite(BIN1, HIGH);
  digitalWrite(BIN2, LOW);

  analogWrite(PWMA, MOTOR_SPEED);
  analogWrite(PWMB, MOTOR_SPEED);
}


void moveBackward() {

  // LEFT MOTOR -> backward
  digitalWrite(AIN1, LOW);
  digitalWrite(AIN2, HIGH);

  // RIGHT MOTOR -> backward
  digitalWrite(BIN1, LOW);
  digitalWrite(BIN2, HIGH);

  analogWrite(PWMA, MOTOR_SPEED);
  analogWrite(PWMB, MOTOR_SPEED);
}


void moveLeft() {

  // LEFT motor backward
  digitalWrite(AIN1, LOW);
  digitalWrite(AIN2, HIGH);

  // RIGHT motor forward
  digitalWrite(BIN1, HIGH);
  digitalWrite(BIN2, LOW);

  analogWrite(PWMA, MOTOR_SPEED);
  analogWrite(PWMB, MOTOR_SPEED);
}


void moveRight() {

  // LEFT motor forward
  digitalWrite(AIN1, HIGH);
  digitalWrite(AIN2, LOW);

  // RIGHT motor backward
  digitalWrite(BIN1, LOW);
  digitalWrite(BIN2, HIGH);

  analogWrite(PWMA, MOTOR_SPEED);
  analogWrite(PWMB, MOTOR_SPEED);
}


// ================================================================
// 11. ADAPTIVE BASELINE CALIBRATION
//
// The project document specifies 50 resting samples.
//
// Keep the eyes centered and remain still while calibration runs.
// ================================================================

void calibrateEOG() {

  Serial.println();
  Serial.println("========================================");
  Serial.println("       EOG CALIBRATION START");
  Serial.println("========================================");
  Serial.println("Keep eyes centered and remain still.");
  Serial.println("Collecting 50 baseline samples...");
  Serial.println();

  delay(1500);

  long totalH = 0;
  long totalV = 0;

  for (int i = 0; i < CALIBRATION_SAMPLES; i++) {

    int rawH = analogRead(EOG_HORIZONTAL);
    int rawV = analogRead(EOG_VERTICAL);

    totalH += rawH;
    totalV += rawV;

    Serial.print("Calibration sample ");
    Serial.print(i + 1);
    Serial.print("/");
    Serial.println(CALIBRATION_SAMPLES);

    delay(20);
  }

  baselineH = (float)totalH / CALIBRATION_SAMPLES;
  baselineV = (float)totalV / CALIBRATION_SAMPLES;

  Serial.println();
  Serial.println("Calibration complete.");

  Serial.print("Horizontal baseline = ");
  Serial.println(baselineH, 2);

  Serial.print("Vertical baseline   = ");
  Serial.println(baselineV, 2);

  Serial.println();
}


// ================================================================
// 12. MOVING AVERAGE FILTER
//
// Five consecutive samples are averaged.
//
// Filter output = average of the latest 5 EOG samples.
// ================================================================

float readFilteredHorizontal() {

  float current =
    (analogRead(EOG_HORIZONTAL) - baselineH) * H_POLARITY;

  hSamples[sampleIndex] = current;

  float total = 0.0;

  for (int i = 0; i < SAMPLE_SIZE; i++) {
    total += hSamples[i];
  }

  return total / SAMPLE_SIZE;
}


float readFilteredVertical() {

  float current =
    (analogRead(EOG_VERTICAL) - baselineV) * V_POLARITY;

  vSamples[sampleIndex] = current;

  float total = 0.0;

  for (int i = 0; i < SAMPLE_SIZE; i++) {
    total += vSamples[i];
  }

  return total / SAMPLE_SIZE;
}


// ================================================================
// 13. UPDATE SIGNAL HISTORY
// ================================================================

void updateHistory(float h, float v) {

  hHistory[historyIndex] = h;
  vHistory[historyIndex] = v;

  historyIndex++;

  if (historyIndex >= HISTORY_SIZE) {
    historyIndex = 0;
  }

  if (historyCount < HISTORY_SIZE) {
    historyCount++;
  }
}


// ================================================================
// 14. HISTORICAL AVERAGES
// ================================================================

float getAverageH() {

  if (historyCount == 0) {
    return 0.0;
  }

  float total = 0.0;

  for (int i = 0; i < historyCount; i++) {
    total += hHistory[i];
  }

  return total / historyCount;
}


float getAverageV() {

  if (historyCount == 0) {
    return 0.0;
  }

  float total = 0.0;

  for (int i = 0; i < historyCount; i++) {
    total += vHistory[i];
  }

  return total / historyCount;
}


// ================================================================
// 15. STABILITY CHECK
//
// The project specifies that the current filtered signal should
// agree with the average of the previous 3 readings.
//
// A tolerance of 4 ADC-count units is used here because the PDF
// does not specify the exact numerical tolerance.
// ================================================================

bool isStableSignal(float currentH, float currentV) {

  if (historyCount < HISTORY_SIZE) {
    return false;
  }

  float averageH = getAverageH();
  float averageV = getAverageV();

  bool horizontalStable =
    abs(currentH - averageH) <= STABILITY_TOLERANCE;

  bool verticalStable =
    abs(currentV - averageV) <= STABILITY_TOLERANCE;

  return horizontalStable && verticalStable;
}


// ================================================================
// 16. COMMAND DECISION ENGINE
//
// A command requires:
//
//   Instantaneous:
//       |FilteredSignal| > 10
//
//   Historical confirmation:
//       |HistoricalAverage| > 8
//
//   Stability:
//       signal agrees with 3-reading history
//
//   Axis dominance:
//       horizontal command -> |H| > |V|
//       vertical command   -> |V| > |H|
//
// ================================================================

Movement determineMovement(float H, float V) {

  float averageH = getAverageH();
  float averageV = getAverageV();


  // --------------------------------------------------------------
  // HORIZONTAL COMMANDS
  // --------------------------------------------------------------

  if (abs(H) > PRIMARY_THRESHOLD &&
      abs(averageH) > CONFIRM_THRESHOLD) {

    // Reject diagonal/mixed movement
    if (abs(H) > abs(V)) {

      if (!isStableSignal(H, V)) {
        return NONE;
      }

      if (H > 0) {
        return RIGHT;
      }
      else {
        return LEFT;
      }
    }
  }


  // --------------------------------------------------------------
  // VERTICAL COMMANDS
  // --------------------------------------------------------------

  if (abs(V) > PRIMARY_THRESHOLD &&
      abs(averageV) > CONFIRM_THRESHOLD) {

    // Reject diagonal/mixed movement
    if (abs(V) > abs(H)) {

      if (!isStableSignal(H, V)) {
        return NONE;
      }

      if (V > 0) {
        return FORWARD;
      }
      else {
        return BACKWARD;
      }
    }
  }


  return NONE;
}


// ================================================================
// 17. EXECUTE MOVEMENT
// ================================================================

void executeMovement(Movement movement) {

  if (movement == NONE) {
    return;
  }

  // Lock the decision engine while moving.
  isMoving = true;

  activeMovement = movement;

  movementStartTime = millis();

  showMovementLED(movement);

  Serial.println();
  Serial.println("**************************************");
  Serial.print("COMMAND ACCEPTED: ");
  Serial.println(getMovementName(movement));
  Serial.println("Motor lockout ACTIVE");
  Serial.println("**************************************");


  switch (movement) {

    case FORWARD:
      moveForward();
      break;

    case BACKWARD:
      moveBackward();
      break;

    case LEFT:
      moveLeft();
      break;

    case RIGHT:
      moveRight();
      break;

    default:
      stopMotors();
      break;
  }
}


// ================================================================
// 18. RESET SIGNAL HISTORY
//
// Prevents the same eye movement from immediately triggering the
// same command again after the 3-second movement finishes.
// ================================================================

void resetHistory() {

  historyIndex = 0;
  historyCount = 0;

  for (int i = 0; i < HISTORY_SIZE; i++) {
    hHistory[i] = 0.0;
    vHistory[i] = 0.0;
  }
}


// ================================================================
// 19. SETUP
// ================================================================

void setup() {

  Serial.begin(115200);


  // --------------------------------------------------------------
  // EOG
  // --------------------------------------------------------------

  pinMode(EOG_HORIZONTAL, INPUT);
  pinMode(EOG_VERTICAL, INPUT);


  // --------------------------------------------------------------
  // TB6612FNG
  // --------------------------------------------------------------

  pinMode(PWMA, OUTPUT);
  pinMode(AIN1, OUTPUT);
  pinMode(AIN2, OUTPUT);

  pinMode(PWMB, OUTPUT);
  pinMode(BIN1, OUTPUT);
  pinMode(BIN2, OUTPUT);

  pinMode(STBY, OUTPUT);


  // --------------------------------------------------------------
  // LEDs
  // --------------------------------------------------------------

  pinMode(LED_FORWARD, OUTPUT);
  pinMode(LED_BACKWARD, OUTPUT);
  pinMode(LED_LEFT, OUTPUT);
  pinMode(LED_RIGHT, OUTPUT);


  // Enable TB6612FNG
  digitalWrite(STBY, HIGH);


  // Safety startup
  stopMotors();
  turnOffLEDs();


  // --------------------------------------------------------------
  // INITIAL CALIBRATION
  // --------------------------------------------------------------

  calibrateEOG();


  // --------------------------------------------------------------
  // INITIALIZE FILTER BUFFERS
  // --------------------------------------------------------------

  for (int i = 0; i < SAMPLE_SIZE; i++) {
    hSamples[i] = 0.0;
    vSamples[i] = 0.0;
  }

  for (int i = 0; i < HISTORY_SIZE; i++) {
    hHistory[i] = 0.0;
    vHistory[i] = 0.0;
  }

  sampleIndex = 0;
  historyIndex = 0;
  historyCount = 0;
  filterReady = false;


  Serial.println("========================================");
  Serial.println(" EOG WHEELCHAIR CONTROLLER READY");
  Serial.println("========================================");
  Serial.println("H-EOG : A0");
  Serial.println("V-EOG : A1");
  Serial.println("Motor : TB6612FNG");
  Serial.println("Mode  : Discrete EOG control");
  Serial.println();
}


// ================================================================
// 20. LOOP
// ================================================================

void loop() {


  // ==============================================================
  // MOTOR LOCKOUT
  //
  // While moving, NO new EOG command is accepted.
  // ==============================================================

  if (isMoving) {

    if (millis() - movementStartTime >= MOVEMENT_DURATION) {

      stopMotors();
      turnOffLEDs();

      Serial.println();
      Serial.println("--------------------------------------");
      Serial.print("MOVEMENT COMPLETE: ");
      Serial.println(getMovementName(activeMovement));
      Serial.println("Motor lockout RELEASED");
      Serial.println("--------------------------------------");
      Serial.println();


      activeMovement = NONE;

      isMoving = false;


      // Clear command history
      resetHistory();


      // Small dead time
      delay(250);
    }

    return;
  }


  // ==============================================================
  // READ + FILTER EOG
  // ==============================================================

  float filteredH = readFilteredHorizontal();

  float filteredV = readFilteredVertical();


  // Move circular buffer index
  sampleIndex++;

  if (sampleIndex >= SAMPLE_SIZE) {

    sampleIndex = 0;

    filterReady = true;
  }


  // Need five samples before making decisions.
  if (!filterReady) {
    delay(10);
    return;
  }


  // ==============================================================
  // UPDATE HISTORY
  // ==============================================================

  updateHistory(filteredH, filteredV);


  // ==============================================================
  // SERIAL TELEMETRY
  // ==============================================================

  if (millis() - lastTelemetry >= 100) {

    Serial.print("H=");
    Serial.print(filteredH, 2);

    Serial.print(" | V=");
    Serial.print(filteredV, 2);

    Serial.print(" | Havg=");
    Serial.print(getAverageH(), 2);

    Serial.print(" | Vavg=");
    Serial.print(getAverageV(), 2);

    Serial.println();

    lastTelemetry = millis();
  }


  // ==============================================================
  // DECISION ENGINE
  // ==============================================================

  Movement command =
    determineMovement(filteredH, filteredV);


  // ==============================================================
  // EXECUTE VALID COMMAND
  // ==============================================================

  if (command != NONE) {

    executeMovement(command);
  }


  delay(10);
}
