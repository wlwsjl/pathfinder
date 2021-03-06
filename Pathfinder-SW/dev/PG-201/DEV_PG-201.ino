/*
Code/project by Neil Movva.

Documentation in progress.

Major thanks to Jeff Rowberg for his I2C libraries and
pioneering work with the MPU-6050. All related code is
his, or at least draws heavily from it.
*/

//#define DEBUG_PRINT_YPR
//#define DEBUG_PRINT_MM
#define ARM_MOTOR
#define MOTOR_NO_H_PFET
//#define MPU_ENABLE
//#define MPU_ONLY_DEBUG

#define SET(x,y) (x |= (1<<y))
#define CLR(x,y) (x &= (~(1<<y)))

#define PORTl PORTC
#define DDRl DDRC
#define L0 0
#define L1 1

#define PORTm PORTD
#define DDRm DDRD
#define MP 6    //PD6
#define MN 5    //PD5

#define PORTus PORTB
#define TRIG 0    //PB0, also pin 8
#define ECHO 7    //PD7, also pin 7

#define L0a A0
#define L1a A1

#define MPa 6
#define MNa 5
#define FWD 1
#define REV -1
#define STOP 0
#define COAST -1
#define BRAKE 1
#define PULSE_LENGTH 25

#define TRIGa 8
#define ECHOa 7

#define EE_CALIBRATED_ADDR  0x01
#define EE_CALIBRATED_TEST  0xAA
#define EE_GYRO_X_ADDR      0x10
#define EE_GYRO_Y_ADDR      0x20
#define EE_GYRO_Z_ADDR      0x30
#define EE_ACCEL_X_ADDR     0x40
#define EE_ACCEL_Y_ADDR     0x50
#define EE_ACCEL_Z_ADDR     0x60

#define V_SOUND 0.34        //represented in mm/uS
#define MAX_RANGE 3000      //in mm
#define PING_TIMEOUT 20000  // 2 * MAX_RANGE/V_SOUND
#define MAX_HAPTIC 2000
#define DROPOUT 50
#define minPingPeriod 200

uint32_t lastFlash = 0;
uint32_t lastPing = 0;
uint32_t lastPulse = 0;
uint32_t timeElapsed = 0;

int16_t pulsePeriod = 250;
int16_t distance = MAX_RANGE;

bool faceDown = false;
bool ledState = false;


#ifdef MPU_ENABLE

#include "EEPROM.h"
#include "Wire.h"
#include "I2Cdev.h"
#include "helper_3dmath.h"
#define HOST_DMP_READ_RATE 7    // 1khz / (1 + READ_RATE) = 125 Hz
#include "libs/Pathfinder_MPU6050_6Axis_MotionApps20.h"

MPU6050 mpu;
bool dmpReady = false;  // set true if DMP init was successful
uint8_t mpuIntStatus;   // holds actual interrupt status byte from MPU
uint8_t devStatus;      // return status after each device operation (0 = success)
uint16_t packetSize;    // expected DMP packet size (default is 42 bytes)
uint16_t fifoCount;     // count of all bytes currently in FIFO
uint8_t fifoBuffer[64]; // FIFO storage buffer
Quaternion q;           // [w, x, y, z]         quaternion container
VectorInt16 aa;         // [x, y, z]            accel sensor measurements
VectorInt16 aaReal;     // [x, y, z]            gravity-free accel sensor 
VectorInt16 aaWorld;    // [x, y, z]            world-frame accel sensor 
VectorFloat gravity;    // [x, y, z]            gravity vector
float euler[3];         // [psi, theta, phi]    Euler angle container
float ypr[3];           // [yaw, pitch, roll]   yaw/pitch/roll container gravity
int xAngle, yAngle, zAngle;
int16_t ax, ay, az;
int16_t gx, gy, gz;
int32_t base_x_gyro, base_y_gyro, base_z_gyro, base_x_accel, base_y_accel, base_z_accel;

volatile bool mpuInterrupt = false;     // indicates whether MPU interrupt pin
void dmpDataReady() {
  mpuInterrupt = true;
}

void setup_mpu(){
  mpu.initialize();
  devStatus = mpu.dmpInitialize();

  //data from G2-200 (how specific?)
  mpu.setXGyroOffset(-183);
  mpu.setYGyroOffset(82);
  mpu.setZGyroOffset(23);
  mpu.setXAccelOffset(-4504);
  mpu.setYAccelOffset(-959);
  mpu.setZAccelOffset(1383);
  
  if (devStatus == 0) {
    // turn on the DMP, now that it's ready
    Serial.println(F("Enabling DMP..."));
    mpu.setDMPEnabled(true);

    // enable Arduino interrupt detection
    Serial.println(
      F("Enabling interrupt detection (Arduino external interrupt 0)..."));
    attachInterrupt(0, dmpDataReady, RISING);
    mpuIntStatus = mpu.getIntStatus();

    // set our DMP Ready flag so the main loop() function knows it's okay to use it
    Serial.println(F("DMP ready! Waiting for first interrupt..."));
    dmpReady = true;

    // get expected DMP packet size for later comparison
    packetSize = mpu.dmpGetFIFOPacketSize();
  } else {
    // ERROR!
    // 1 = initial memory load failed
    // 2 = DMP configuration updates failed
    // (if it's going to break, usually the code will be 1)
    Serial.print(F("DMP Initialization failed (code "));
      Serial.print(devStatus);
      Serial.println(F(")"));
  }
}

void processMPU() {
  // reset interrupt flag and get INT_STATUS byte
  mpuInterrupt = false;
  mpuIntStatus = mpu.getIntStatus();

  // get current FIFO count
  fifoCount = mpu.getFIFOCount();

  // check for overflow (this should never happen unless our code is too inefficient)
  if ((mpuIntStatus & 0x10) || fifoCount == 1024) {
    // reset so we can continue cleanly
    mpu.resetFIFO();
    Serial.println(F("FIFO overflow!"));
    // otherwise, check for DMP data ready interrupt (this should happen frequently)
  } else if (mpuIntStatus & 0x02) {
    // wait for correct available data length, should be a VERY short wait
    while (fifoCount < packetSize) fifoCount = mpu.getFIFOCount();

    // read a packet from FIFO
    mpu.getFIFOBytes(fifoBuffer, packetSize);

    // track FIFO count here in case there is > 1 packet available
    // (this lets us immediately read more without waiting for an interrupt)
    fifoCount -= packetSize;

    // display Euler angles in degrees
    mpu.dmpGetQuaternion(&q, fifoBuffer);
    mpu.dmpGetGravity(&gravity, &q);
    mpu.dmpGetYawPitchRoll(ypr, &q, &gravity);
    yAngle = ypr[1] * 180 / M_PI;

    if(yAngle < -55){
      faceDown = true;
      //Serial.println("faceDown!");
    } else {
      faceDown = false;
    }

    #ifdef DEBUG_PRINT_YPR
    xAngle = ypr[0] * 180 / M_PI;
    zAngle = ypr[2] * 180 / M_PI;
    Serial.print("ypr      ");
    Serial.print(xAngle);
    Serial.print("   ");
    Serial.print(yAngle);
    Serial.print("   ");
    Serial.println(zAngle);
    #endif
  }
}
//TODO: bring in calibration routines, but in a modular way
void calibrateMPU(){
  byte isCalibrated = EEPROM.read(EE_CALIBRATED_ADDR);
  if(isCalibrated == EE_CALIBRATED_TEST){

    } else {

    }

}

#endif

void drive(int cmd){
  #ifdef ARM_MOTOR
    #ifdef MOTOR_NO_H_PFET
      switch(cmd){
        case 1:   //drive motor in the (arbitrarily) forward direction
        SET(PORTm, MP);
        break;

        default:   //cut-off motor (floating)
        SET(PORTm, MP);
        break;
      }
    #else
      switch(cmd){
        case 1:   //drive motor in the (arbitrarily) forward direction
        digitalWrite(MPa, HIGH);
        digitalWrite(MNa, LOW);
        break;

        case -1:  //drive motor in the (arbitrarily) reverse direction
        digitalWrite(MPa, LOW);
        digitalWrite(MNa, HIGH);
        break;

        default:   //short motor to ground, stalling motion
        digitalWrite(MPa, HIGH);
        digitalWrite(MNa, HIGH);
        break;
      }
    #endif
  #endif
}

void drive(int cmd, int duration){
  drive(cmd);
  delay(duration);
  drive(STOP);
}

void drive(int cmd, int duration, bool coastMode){
  drive(cmd);
  if(coastMode != COAST){
    drive(STOP);
  }
}

int16_t getDistance(){
  if((millis() - lastPing) < minPingPeriod){
    return distance;
  }

  SET(PORTl, L0);

  lastPing = millis();
  int16_t mm;
  
  CLR(PORTus, TRIG);
  delayMicroseconds(5);
  SET(PORTus, TRIG);
  delayMicroseconds(10);
  CLR(PORTus, TRIG);

  mm = pulseIn(ECHOa, HIGH, PING_TIMEOUT); //MAX_RANGE roundtrip timeout

  if (mm == 0){
    return MAX_RANGE;
  }

  mm *= V_SOUND;
  mm /= 2;

  if (mm <= DROPOUT){
    return -1;
  }

  distance = mm;
  #ifdef DEBUG_PRINT_MM
  Serial.print("distance:\t");
  Serial.println(mm);
  #endif
  CLR(PORTl, L0);
  return mm;
}

void pulse(){
  drive(FWD, PULSE_LENGTH);   //drive ERM
  drive(REV, PULSE_LENGTH/4); //active braking (regen?) with default braking at end
  lastPulse = millis();
}

void translate(){
  if(distance == -1){
    while(distance == -1){
      distance = getDistance();
      drive(FWD);
    }
    drive(STOP);
    return;
  }
  pulsePeriod = map(distance, DROPOUT, MAX_HAPTIC, 50, 750);
}

void setup() {
  pinMode(MPa, OUTPUT);
  pinMode(MNa, OUTPUT);

  SET(PORTm, MP);
  SET(PORTm, MN);

  pinMode(TRIGa, OUTPUT);
  pinMode(ECHOa, INPUT);

  pinMode(L0a, OUTPUT); // the activity led (blinks when sensors are read)
  pinMode(L1a, OUTPUT); // the system status led (if off, system off)

  digitalWrite(L1a, HIGH);

  Serial.begin(115200);

  #ifdef MPU_ENABLE
  Wire.begin();
  TWBR = 24;
  setup_mpu();
  #endif

}

void loop() {
  #ifdef MPU_ENABLE
  if(mpuInterrupt || fifoCount > packetSize){
      processMPU();
  }
  #endif
  
  #ifdef MPU_ONLY_DEBUG
  return;
  #endif

  if(faceDown){
    SET(PORTl, L0);
    CLR(PORTl, L1);
    return;
  } else {
    CLR(PORTl, L0);
    SET(PORTl, L1);
  }

  getDistance();          //getDistance will limit itself if we're going too fast
  translate();            //expensive math op to generate pulse period value
  timeElapsed = millis() - lastPulse;  //timestamping for pulse schedule
  if(timeElapsed > pulsePeriod){  //watchdog for pulse frequency
    pulse();
  }
}