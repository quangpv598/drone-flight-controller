/**
 * The software is provided "as is", without any warranty of any kind.
 * Feel free to edit it if needed.
 * 
 * @author lobodol <grobodol@gmail.com>
 */

// ---------------------------------------------------------------------------
#include <Wire.h>
// ------------------- Define some constants for convenience -----------------
#define CHANNEL1 0
#define CHANNEL2 1
#define CHANNEL3 2
#define CHANNEL4 3

#define YAW      0
#define PITCH    1
#define ROLL     2
#define THROTTLE 3

#define X           0     // X axis
#define Y           1     // Y axis
#define Z           2     // Z axis
#define MPU_ADDRESS 0x68  // I2C address of the MPU-6050
#define FREQ        250   // Sampling frequency
#define SSF_GYRO    65.5  // Sensitivity Scale Factor of the gyro from datasheet
// ---------------- Receiver variables ---------------------------------------
// Received instructions formatted with good units, in that order : [Yaw, Pitch, Roll, Throttle]
float instruction[4];

// Previous state of each channel (HIGH or LOW)
volatile byte previous_state[4];

// Duration of the pulse on each channel of the receiver in µs (must be within 1000µs & 2000µs)
volatile unsigned int pulse_length[4] = {1500, 1500, 1000, 1500};

// Used to calculate pulse duration on each channel.
volatile unsigned long current_time;
volatile unsigned long timer[4]; // Timer of each channel.

// Used to configure which control (yaw, pitch, roll, throttle) is on which channel.
int mode_mapping[4];
// ----------------------- MPU variables -------------------------------------
// The RAW values got from gyro (in °/sec) in that order: X, Y, Z
int gyro_raw[3] = {0,0,0};

// Average gyro offsets of each axis in that order: X, Y, Z
long gyro_offset[3] = {0, 0, 0};

// Calculated angles from gyro's values in that order: X, Y, Z
float gyro_angle[3]  = {0,0,0};

// The RAW values got from accelerometer (in m/sec²) in that order: X, Y, Z
int acc_raw[3] = {0 ,0 ,0};

// Calculated angles from accelerometer's values in that order: X, Y, Z
float acc_angle[3] = {0,0,0};

// Total 3D acceleration vector in m/s²
long acc_total_vector;

// MPU's temperature
int temperature;

// Init flag set to TRUE after first loop
boolean initialized;
// ----------------------- Variables for servo signal generation -------------
unsigned int  period; // Sampling period
unsigned long loop_timer;
unsigned long now, difference;

unsigned long pulse_length_esc1 = 1000,
        pulse_length_esc2 = 1000,
        pulse_length_esc3 = 1000,
        pulse_length_esc4 = 1000;

// ------------- Global variables used for PID automation --------------------
float errors[3];                     // Measured errors (compared to instructions) : [Yaw, Pitch, Roll]
float error_sum[3]      = {0, 0, 0}; // Error sums (used for integral component) : [Yaw, Pitch, Roll]
float previous_error[3] = {0, 0, 0}; // Last errors (used for derivative component) : [Yaw, Pitch, Roll]
float measures[3]       = {0, 0, 0}; // Angular measures : [Yaw, Pitch, Roll]
// ---------------------------------------------------------------------------

/**
 * Setup configuration
 */
void setup() {
    // Turn LED on during setup.
    pinMode(13, OUTPUT);
    digitalWrite(13, HIGH);

    setupMpu6050Registers();

    calibrateMpu6050();

    // Set pins 4 5 6 7 as outputs.
    DDRD |= B11110000;

    // Initialize loop_timer.
    loop_timer = micros();

    configureChannelMapping();

    // Start I2C communication
    Wire.begin();
    TWBR = 24; // 400kHz I2C clock (200kHz if CPU is 8MHz)

    // Configure interrupts for receiver.
    PCICR  |= (1 << PCIE0);  //Set PCIE0 to enable PCMSK0 scan.
    PCMSK0 |= (1 << PCINT0); //Set PCINT0 (digital input 8) to trigger an interrupt on state change.
    PCMSK0 |= (1 << PCINT1); //Set PCINT1 (digital input 9) to trigger an interrupt on state change.
    PCMSK0 |= (1 << PCINT2); //Set PCINT2 (digital input 10)to trigger an interrupt on state change.
    PCMSK0 |= (1 << PCINT3); //Set PCINT3 (digital input 11)to trigger an interrupt on state change.

    period = (1000000/FREQ) ; // Sampling period in µs

    // Turn LED off now setup is done.
    digitalWrite(13, LOW);
}


/**
 * Main program loop
 */
void loop() {
    // 1. First, read raw values from MPU-6050
    readSensor();

    // 2. Calculate angles from gyro & accelerometer's values
    calculateAngles();

    // 3. Translate received data into usable values
    getFlightInstruction();

    // 4. Calculate errors comparing received instruction with measures
    calculateErrors();

    // 5. Calculate motors speed with PID controller
    automation();

    // 6. Apply motors speed
    applyMotorSpeed();
}

/**
 * Generate servo-signal on digital pins #4 #5 #6 #7 with a frequency of 250Hz (4ms period).
 * Direct port manipulation is used for performances.
 *
 * This function might not take more than 2ms to run, which lets 2ms remaining to do other stuff.
 *
 * @see https:// www.arduino.cc/en/Reference/PortManipulation
 *
 * @return void
 */
void applyMotorSpeed() {
    // Refresh rate is 250Hz: send ESC pulses every 4000µs
    while ((now = micros()) - loop_timer < period);

    // Update loop timer
    loop_timer = now;

    // Set pins #4 #5 #6 #7 HIGH
    PORTD |= B11110000;

    // Wait until all pins #4 #5 #6 #7 are LOW
    while (PORTD >= 16) {
        now        = micros();
        difference = now - loop_timer;

        if (difference >= pulse_length_esc1) PORTD &= B11101111; // Set pin #4 LOW
        if (difference >= pulse_length_esc2) PORTD &= B11011111; // Set pin #5 LOW
        if (difference >= pulse_length_esc3) PORTD &= B10111111; // Set pin #6 LOW
        if (difference >= pulse_length_esc4) PORTD &= B01111111; // Set pin #7 LOW
    }
}

/**
 * Request raw values from MPU6050.
 * 
 * @return void
 */
void readSensor() {
    Wire.beginTransmission(MPU_ADDRESS); // Start communicating with the MPU-6050
    Wire.write(0x3B);                    // Send the requested starting register
    Wire.endTransmission();              // End the transmission
    Wire.requestFrom(MPU_ADDRESS,14);    // Request 14 bytes from the MPU-6050

    // Wait until all the bytes are received
    while(Wire.available() < 14);

    acc_raw[X]  = Wire.read() << 8 | Wire.read(); // Add the low and high byte to the acc_raw[X] variable
    acc_raw[Y]  = Wire.read() << 8 | Wire.read(); // Add the low and high byte to the acc_raw[Y] variable
    acc_raw[Z]  = Wire.read() << 8 | Wire.read(); // Add the low and high byte to the acc_raw[Z] variable
    temperature = Wire.read() << 8 | Wire.read(); // Add the low and high byte to the temperature variable
    gyro_raw[X] = Wire.read() << 8 | Wire.read(); // Add the low and high byte to the gyro_raw[X] variable
    gyro_raw[Y] = Wire.read() << 8 | Wire.read(); // Add the low and high byte to the gyro_raw[Y] variable
    gyro_raw[Z] = Wire.read() << 8 | Wire.read(); // Add the low and high byte to the gyro_raw[Z] variable
}

/**
 * Calculate real angles from gyro and accelerometer's values
 */
void calculateAngles()
{
    calculateGyroAngles();
    calculateAccelerometerAngles();

    if (initialized) {
        // Correct the drift of the gyro with the accelerometer
        gyro_angle[X] = gyro_angle[X] * 0.9996 + acc_angle[X] * 0.0004;
        gyro_angle[Y] = gyro_angle[Y] * 0.9996 + acc_angle[Y] * 0.0004;
    } else {
        // At very first start, init gyro angles with accelerometer angles
        gyro_angle[X] = acc_angle[X];
        gyro_angle[Y] = acc_angle[Z];

        initialized = true;
    }

    // To dampen the pitch and roll angles a complementary filter is used.
    measures[ROLL]  = measures[ROLL] * 0.9 + gyro_angle[X] * 0.1;
    measures[PITCH] = measures[PITCH] * 0.9 + gyro_angle[Y] * 0.1;
    measures[YAW]   = gyro_raw[Z]/SSF_GYRO; // Store the angular motion for this axis
}

/**
 * Calculate angles with gyro data
 */
void calculateGyroAngles()
{
    // Substract offsets
    gyro_raw[X] -= gyro_offset[X];
    gyro_raw[Y] -= gyro_offset[Y];
    gyro_raw[Z] -= gyro_offset[Z];

    // Angle calculation
    gyro_angle[X] += gyro_raw[X]   / (FREQ * SSF_GYRO);
    gyro_angle[Y] += (gyro_raw[Y]) / (FREQ * SSF_GYRO);

    // Transfer roll to pitch if IMU has yawed
    gyro_angle[Y] -= gyro_angle[X] * sin(gyro_raw[Z] * (PI / (FREQ * SSF_GYRO * 180)));
    gyro_angle[X] += gyro_angle[Y] * sin(gyro_raw[Z] * (PI / (FREQ * SSF_GYRO * 180)));
}

/**
 * Calculate angles using only the accelerometer
 */
void calculateAccelerometerAngles()
{
    // Calculate total 3D acceleration vector
    acc_total_vector = sqrt(pow(acc_raw[X], 2) + pow(acc_raw[Y], 2) + pow(acc_raw[Z], 2));

    // To prevent asin to produce a NaN, make sure the input value is within [-1;+1]
    if (abs(acc_raw[X]) < acc_total_vector) {
        acc_angle[X] = asin((float)acc_raw[Y] / acc_total_vector) * (180 / PI); // asin gives angle in radian. Convert to degree multiplying by 180/pi
    }

    if (abs(acc_raw[Y]) < acc_total_vector) {
        acc_angle[Y] = asin((float)acc_raw[X] / acc_total_vector) * (180 / PI);
    }
}

/**
 * Calculate motor speed for each motor of an X quadcopter depending on received instructions and measures from sensor
 * by applying PID control.
 *
 * (A) (B)     x
 *   \ /     z ↑
 *    X       \|
 *   / \       +----→ y
 * (C) (D)
 *
 * Motors A & D run clockwise.
 * Motors B & C run counter-clockwise.
 *
 * Each motor output is considered as a servomotor. As a result, value range is about 1000µs to 2000µs
 * 
 * @return void
 */
void automation() {
    float Kp[3]       = {10, 10, 10}; // P coefficients in that order : Yaw, Pitch, Roll //ku = 0.21
    float Ki[3]       = {0.0, 0, 0};  // I coefficients in that order : Yaw, Pitch, Roll
    float Kd[3]       = {0, 0, 0};    // D coefficients in that order : Yaw, Pitch, Roll
    float deltaErr[3] = {0, 0, 0};    // Error deltas in that order :  Yaw, Pitch, Roll
    float yaw         = 0;
    float pitch       = 0;
    float roll        = 0;

    // Initialize motor commands with throttle
    pulse_length_esc1 = instruction[THROTTLE];
    pulse_length_esc2 = instruction[THROTTLE];
    pulse_length_esc3 = instruction[THROTTLE];
    pulse_length_esc4 = instruction[THROTTLE];

    // Do not calculate anything if throttle is 0
    if (instruction[THROTTLE] >= 1012) {
        // Calculate sum of errors : Integral coefficients
        error_sum[YAW] += errors[YAW];
        error_sum[PITCH] += errors[PITCH];
        error_sum[ROLL] += errors[ROLL];

        // Calculate error delta : Derivative coefficients
        deltaErr[YAW] = errors[YAW] - previous_error[YAW];
        deltaErr[PITCH] = errors[PITCH] - previous_error[PITCH];
        deltaErr[ROLL] = errors[ROLL] - previous_error[ROLL];

        // Save current error as previous_error for next time
        previous_error[YAW] = errors[YAW];
        previous_error[PITCH] = errors[PITCH];
        previous_error[ROLL] = errors[ROLL];

        yaw = (errors[YAW] * Kp[YAW]) + (error_sum[YAW] * Ki[YAW]) + (deltaErr[YAW] * Kd[YAW]);
        pitch = (errors[PITCH] * Kp[PITCH]) + (error_sum[PITCH] * Ki[PITCH]) + (deltaErr[PITCH] * Kd[PITCH]);
        roll = (errors[ROLL] * Kp[ROLL]) + (error_sum[ROLL] * Ki[ROLL]) + (deltaErr[ROLL] * Kd[ROLL]);

        // Yaw - Lacet (Z axis)
        pulse_length_esc1 -= yaw;
        pulse_length_esc4 -= yaw;
        pulse_length_esc3 += yaw;
        pulse_length_esc2 += yaw;

        // Pitch - Tangage (Y axis)
        pulse_length_esc1 += pitch;
        pulse_length_esc2 += pitch;
        pulse_length_esc3 -= pitch;
        pulse_length_esc4 -= pitch;

        // Roll - Roulis (X axis)
        pulse_length_esc1 -= roll;
        pulse_length_esc3 -= roll;
        pulse_length_esc2 += roll;
        pulse_length_esc4 += roll;
    }

    pulse_length_esc1 = minMax(pulse_length_esc1, 1000, 2000);
    pulse_length_esc2 = minMax(pulse_length_esc2, 1000, 2000);
    pulse_length_esc3 = minMax(pulse_length_esc3, 1000, 2000);
    pulse_length_esc4 = minMax(pulse_length_esc4, 1000, 2000);
}


/**
 * Calculate errors of Yaw, Pitch & Roll: this is simply the difference between the measure and the command.
 *
 * @return void
 */
void calculateErrors() {
    errors[YAW]   = measures[YAW]   - instruction[YAW];
    errors[PITCH] = measures[PITCH] - instruction[PITCH];
    errors[ROLL]  = measures[ROLL]  - instruction[ROLL];
}

/**
 * Calculate real value of flight instructions from pulses length of each channel.
 *
 * - Roll     : from -33° to 33°
 * - Pitch    : from -33° to 33°
 * - Yaw      : from -180°/sec to 180°/sec
 * - Throttle : from 1000µs to 2000µs
 *
 * @return void
 */
void getFlightInstruction() {
    instruction[YAW]      = map(pulse_length[mode_mapping[YAW]], 1000, 2000, -180, 180);
    instruction[PITCH]    = map(pulse_length[mode_mapping[PITCH]], 1000, 2000, -33, 33);
    instruction[ROLL]     = map(pulse_length[mode_mapping[ROLL]], 1000, 2000, -33, 33);
    instruction[THROTTLE] = pulse_length[mode_mapping[THROTTLE]];
}

/**
 * Customize mapping of controls: set here which command is on which channel and call
 * this function in setup() routine.
 *
 * @return void
 */
void configureChannelMapping() {
    mode_mapping[YAW]      = CHANNEL4;
    mode_mapping[PITCH]    = CHANNEL2;
    mode_mapping[ROLL]     = CHANNEL1;
    mode_mapping[THROTTLE] = CHANNEL3;
}

/**
 * Configure gyro and accelerometer precision as following:
 *  - accelerometer: +/-8g
 *  - gyro: 500dps full scale
 *
 * @return void
 */
void setupMpu6050Registers() {
    //Activate the MPU-6050
    Wire.beginTransmission(MPU_ADDRESS);      // Start communicating with the MPU-6050
    Wire.write(0x6B);                         // Send the requested starting register
    Wire.write(0x00);                         // Set the requested starting register
    Wire.endTransmission();                   // End the transmission
    //Configure the accelerometer (+/-8g)
    Wire.beginTransmission(MPU_ADDRESS);      // Start communicating with the MPU-6050
    Wire.write(0x1C);                         // Send the requested starting register
    Wire.write(0x10);                         // Set the requested starting register
    Wire.endTransmission();                   // End the transmission
    //Configure the gyro (500dps full scale)
    Wire.beginTransmission(MPU_ADDRESS);      // Start communicating with the MPU-6050
    Wire.write(0x1B);                         // Send the requested starting register
    Wire.write(0x08);                         // Set the requested starting register
    Wire.endTransmission();                   // End the transmission
}

/**
 * Calibrate MPU6050: take 2000 samples to calculate average offsets.
 * During this step, the quadcopter needs to be static and on a horizontal surface.
 *
 * This function also sends low throttle signal to each ESC to init and prevent them beeping annoyingly.
 *
 * @return void
 */
void calibrateMpu6050()
{
    int max_samples = 2000;

    for (int i = 0; i < max_samples; i++) {
        readSensor();

        gyro_offset[X] += gyro_raw[X];
        gyro_offset[Y] += gyro_raw[Y];
        gyro_offset[Z] += gyro_raw[Z];

        // Generate low throttle pulse to init ESC and prevent them beeping
        PORTD |= B11110000;                  // Set pins #4 #5 #6 #7 HIGH
        delayMicroseconds(1000); // Wait 1000µs
        PORTD &= B00001111;                  // Then set LOW

        // Just wait a bit before next loop
        delay(3);
    }

    // Calculate average offsets
    gyro_offset[X] /= max_samples;
    gyro_offset[Y] /= max_samples;
    gyro_offset[Z] /= max_samples;
}

/**
 * Make sure that value is not over min_value/max_value.
 *
 * @param float value     : The value to convert
 * @param float min_value : The min value
 * @param float max_value : The max value
 * @return float
 */
float minMax(float value, float min_value, float max_value) {
    if (value > max_value) {
        value = max_value;
    } else if (value < min_value) {
        value = min_value;
    }

    return value;
}

/**
 * This Interrupt Sub Routine is called each time input 8, 9, 10 or 11 changed state.
 * Read the receiver signals in order to get flight instructions.
 *
 * This routine must be as fast as possible to prevent main program to be messed up.
 * The trick here is to use port registers to read pin state.
 * Doing (PINB & B00000001) is the same as digitalRead(8) with the advantage of using less CPU loops.
 * It is less convenient but more efficient, which is the most important here.
 *
 * @see https://www.arduino.cc/en/Reference/PortManipulation
 */
ISR(PCINT0_vect) {
    current_time = micros();

    // Channel 1 -------------------------------------------------
    if (PINB & B00000001) {                                        // Is input 8 high ?
        if (previous_state[CHANNEL1] == LOW) {                     // Input 8 changed from 0 to 1 (rising edge)
            previous_state[CHANNEL1] = HIGH;                       // Save current state
            timer[CHANNEL1] = current_time;                        // Save current time
        }
    } else if (previous_state[CHANNEL1] == HIGH) {                 // Input 8 changed from 1 to 0 (falling edge)
        previous_state[CHANNEL1] = LOW;                            // Save current state
        pulse_length[CHANNEL1] = current_time - timer[CHANNEL1];   // Calculate pulse duration & save it
    }

    // Channel 2 -------------------------------------------------
    if (PINB & B00000010) {                                        // Is input 9 high ?
        if (previous_state[CHANNEL2] == LOW) {                     // Input 9 changed from 0 to 1 (rising edge)
            previous_state[CHANNEL2] = HIGH;                       // Save current state
            timer[CHANNEL2] = current_time;                        // Save current time
        }
    } else if (previous_state[CHANNEL2] == HIGH) {                 // Input 9 changed from 1 to 0 (falling edge)
        previous_state[CHANNEL2] = LOW;                            // Save current state
        pulse_length[CHANNEL2] = current_time - timer[CHANNEL2];   // Calculate pulse duration & save it
    }

    // Channel 3 -------------------------------------------------
    if (PINB & B00000100) {                                        // Is input 10 high ?
        if (previous_state[CHANNEL3] == LOW) {                     // Input 10 changed from 0 to 1 (rising edge)
            previous_state[CHANNEL3] = HIGH;                       // Save current state
            timer[CHANNEL3] = current_time;                        // Save current time
        }
    } else if (previous_state[CHANNEL3] == HIGH) {                 // Input 10 changed from 1 to 0 (falling edge)
        previous_state[CHANNEL3] = LOW;                            // Save current state
        pulse_length[CHANNEL3] = current_time - timer[CHANNEL3];   // Calculate pulse duration & save it
    }

    // Channel 4 -------------------------------------------------
    if (PINB & B00001000) {                                        // Is input 11 high ?
        if (previous_state[CHANNEL4] == LOW) {                     // Input 11 changed from 0 to 1 (rising edge)
            previous_state[CHANNEL4] = HIGH;                       // Save current state
            timer[CHANNEL4] = current_time;                        // Save current time
        }
    } else if (previous_state[CHANNEL4] == HIGH) {                 // Input 11 changed from 1 to 0 (falling edge)
        previous_state[CHANNEL4] = LOW;                            // Save current state
        pulse_length[CHANNEL4] = current_time - timer[CHANNEL4];   // Calculate pulse duration & save it
    }
}
