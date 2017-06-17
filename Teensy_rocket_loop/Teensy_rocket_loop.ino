#include <time.h>
#include <SPI.h>
#include <Stepper.h>
#include <Servo.h>
#include <Wire.h>
#include <math.h>
#include <string.h>
#include <Adafruit_BMP085.h> // baroSensormeter Library
#include <SD.h>  // Default Arduino Library Supports it
#include <TinyGPS.h> //TinyGPS library

/*****************************************************************/
/*********** USER SETUP AREA! Set your options here! *************/
/*****************************************************************/

// HARDWARE OPTIONS
/*****************************************************************/
//TODO: figure out what these actually are
//All serial communication lines
#define DOWNLINK_SERIAL Serial5
#define GPS_SERIAL Serial2
#define IMU_SERIAL Serial1

//#define BMP_180_ADDRESS 0x77
//Parachute triggers
#define DROGUE_CHUTE_PIN 20

//Sampler pin
#define FAN_PIN 23

//Fan speeds (Do not exceed: max(70) min(40))
#define FAN_ARMED_ZERO_THRUST 40
#define FAN_FULL_THROTTLE 70

//Stepper Motor
#define STEPS_PER_REVOLUTION 200
#define STEPPER_SPEED 100
#define STEPS_PER_FILTER 25

#define STEPPER_PIN_IN1A 34
#define STEPPER_PIN_IN2A 37
#define STEPPER_PIN_IN1B 38
#define STEPPER_PIN_IN2B 39

//SD card
#define SD_CS_PIN BUILTIN_SDCARD

//LED
#define LED_PIN 13

// FLIGHT OPTIONS
/*****************************************************************/
#define NUM_FILTERS 6
#define ALTITUDE_BUFFER_SIZE 10
#define ALTITUDE_TOLERANCE 50 //In meters

//Sensor Constants and defintions
#define SEA_LVL_PRESSURE 101800 //current sea level equivalent pressure for your location. used for calibration. 

//Timing delay
#define TIME_BETWEEN_COMMUNICATIONS 2000 //in ms


// DEBUG OPTIONS
/*****************************************************************/
#define SERIAL_DEBUGGING true
#define SKIP_SAFETY_CHECKS false //************BE VERY CAREFUL TO DISABLE THIS BEFORE WIRING CHARGE************
#define LED_DEBUGGING true

/*****************************************************************/
/****************** END OF USER SETUP AREA!  *********************/
/*****************************************************************/

//Output file
File logFile;

//Peripheral objects
Servo samplerFan;
Stepper samplerStepper(STEPS_PER_REVOLUTION,STEPPER_PIN_IN1A,STEPPER_PIN_IN2A,STEPPER_PIN_IN1B,STEPPER_PIN_IN2B);
Adafruit_BMP085 baroSensor;
TinyGPS gps;

//Sensor variables
float x_accel;
float y_accel;
float z_accel;

float ang_accel_x;
float ang_accel_y;
float ang_accel_z;

float mag_x;
float mag_y;
float mag_z;

float altitude;
float temperature;
float pressure;

float pitch;
float yaw;
float roll;

long longitude;
long latitude;
float altitude_gps;

float altitude_baseline;
float normalised_altitude;

//Keep track of what filter we're on
int filterNumber;

unsigned long lastCommunication;

char dataChar[250];
String dataString;

/*
Setup method. Initiates all variables and files
*/
void setup() {
  if (SERIAL_DEBUGGING)
  {
    Serial.begin(9600);
    while (!Serial) {
    ; // wait for serial port to connect. Needed for native USB port only
    }
  }
  
  if (!baroSensor.begin()) { 
   Serial.println("Could not find a valid BMP085 sensor, check wiring!"); 
  } 

  DOWNLINK_SERIAL.begin(9600);
  GPS_SERIAL.begin(9600);
  IMU_SERIAL.begin(57600);

  filterNumber = 0;
  lastCommunication = 0;

  samplerFan.attach(FAN_PIN);
  samplerFan.write(FAN_ARMED_ZERO_THRUST);

  if (LED_DEBUGGING) {pinMode(LED_PIN,OUTPUT);}

  if (!SD.begin(SD_CS_PIN)) {
    Serial.println("Initialization failed!");
    return;
  }
  
  logFile = SD.open("LOGS.txt", FILE_WRITE);
  logFile.println("# Log file initialised at system time " + String(millis(), DEC));
  logFile.println("Columns in order w/(units): ");
  logFile.flush();

  Serial.println("Initialized. Starting main loop.");
}

/*
main loop
we will use sub loops like what we planned for the c++
*/
void loop() {
  if (SERIAL_DEBUGGING) {Serial.println("Grabbing first data.");}
	updateData();
  altitude_baseline = altitude;

	while(true)
	{
		if(altitude > ALTITUDE_TOLERANCE + altitude_baseline || SKIP_SAFETY_CHECKS) //Compare against initial height
		{
			 logFile.println("Altitude change of 50 meters exceeded at " + String(millis(), DEC));
			 goto loop_launch_started;
		}
		updateData();
	}
	
loop_launch_started:
	//TODO: we need accurate acceleration curves, 15 m/s^2 is the initial guess
	while(true)
	{
		if(pow(pow(x_accel, 2) + pow(y_accel, 2) + pow(z_accel,2), 0.5) <= 15  || SKIP_SAFETY_CHECKS) //Take the magnitude of acceleration and wait until it is smaller than 15 m/s^2
		{
			 logFile.println("Acceleration dropped to normal levels at "+ String(millis(), DEC));
			 goto loop_high_acceleration;
		}
		updateData();
	}


loop_high_acceleration:
  //take data while waiting for max height
	float altitudeBuffer[ALTITUDE_BUFFER_SIZE];
  float averageSecondHalf;
  float averageFirstHalf;
  memset(altitudeBuffer,0,sizeof(altitudeBuffer));
  
	while(true)
	{
    //Update altitude buffer
    for(int i = 1; i < ALTITUDE_BUFFER_SIZE; i++){altitudeBuffer[i-1]=altitudeBuffer[i];}
    altitudeBuffer[ALTITUDE_BUFFER_SIZE-1] = altitude;

    //Average the first half of the buffer
    averageFirstHalf = 0.0;
    for(int i = 0; i < floor(ALTITUDE_BUFFER_SIZE/2.0); i++){averageFirstHalf+= altitudeBuffer[i];}
    averageFirstHalf = averageFirstHalf/floor(ALTITUDE_BUFFER_SIZE/2.0);

    //Average the second half of the buffer
    averageSecondHalf = 0.0;
    for(int i = floor(ALTITUDE_BUFFER_SIZE/2.0); i < ALTITUDE_BUFFER_SIZE; i++){averageSecondHalf += altitudeBuffer[i];}
    averageSecondHalf = averageSecondHalf/(ALTITUDE_BUFFER_SIZE - floor(ALTITUDE_BUFFER_SIZE/2.0));

    //Check for descent by checking if the first half of the buffer shows us being higher than the second half
    if(averageFirstHalf > averageSecondHalf || SKIP_SAFETY_CHECKS){goto loop_begun_descent;}
    else{updateData();}
	}
 
loop_begun_descent:
  deployChute(DROGUE_CHUTE_PIN);
  
	//begins the spin sampler sequence
  samplerFan.write(FAN_FULL_THROTTLE);
	spinSampler();
	while(true) //turn the sampler while we fall until second parachute deployment
	{
		 updateData();

     normalised_altitude = altitude - altitude_baseline;

     //Sampler logic
     if (normalised_altitude < 9144 && normalised_altitude > 6096 && filterNumber != 1) // 10000 ft distance between 30000 ft and 20000 ft
     {
      spinSampler();
     }
     else if (normalised_altitude < 6096 && normalised_altitude > 4114.8 && filterNumber != 2) // 6500 ft distance between 20000 ft and 13500 ft
     {
      spinSampler();
     }
     else if (normalised_altitude < 4114.8 && normalised_altitude > 3352.8 && filterNumber != 3) // 2500 ft distance between 13500 ft and 11000 ft
     {
      spinSampler();
     }
     else if (normalised_altitude < 3352.8 && normalised_altitude > 3048 && filterNumber != 4) // 1000 ft distance between 11000 ft and 10000 ft
     {
      spinSampler();
     }
     else if (normalised_altitude < 3048) //If it's lower than 10000ft, no use in collecting data anymore.
     {
      spinSampler(); // Should spin us to filter 5 if everything went well. Otherwise, will spin to unused filter. Check log for details
      goto loop_final_descent;
     }
	}

loop_final_descent:
  // Kill fan
  samplerFan.write(FAN_ARMED_ZERO_THRUST);
	
	// After both parachutes have deployed
	while(true)
	{
		updateData();
	}
}

/******************************************************************************
  Fetch and update all data. Record data and communicate when appropriate.
******************************************************************************/
void updateData() {

  if (LED_DEBUGGING) {digitalWrite(LED_PIN, HIGH);}
  //memset(dataChar,'*',sizeof(dataChar));
 
  //Updates baroSensormeter variables
  pressure = baroSensor.readPressure();
  altitude = baroSensor.readAltitude(SEA_LVL_PRESSURE);
  temperature = baroSensor.readTemperature();
  
  if (SERIAL_DEBUGGING) {Serial.println("Got barometer data");}
  
  
  //Updates gps variables
  parseGPSStream();
  if (SERIAL_DEBUGGING) {Serial.println("Got gps data");}
  
  //Updates IMU vaiables
  parseIMUStream();
  if (SERIAL_DEBUGGING) {Serial.println("Got IMU data");}
  
  
  // Prepares string for logging and telemetry purposes
  dataString = 
    "$" + 
    String(millis()) + "|" + 
    
    String(temperature) + "|" + 
    String(pressure) + "|" +
    String(altitude) + "|" + 

    String(ang_accel_x) + "|" +
    String(ang_accel_y) + "|" +
    String(ang_accel_z) + "|" +
    
    String(x_accel) + "|" +
    String(y_accel) + "|" +
    String(z_accel) + "|" +
    
    String(mag_x) + "|" + 
    String(mag_y) + "|" + 
    String(mag_z) + "|" +

    String(pitch) + "|" + 
    String(roll) + "|" + 
    String(yaw)  + "|" +
    
    String(latitude) +"|" +
    String(longitude) + "|" +
    String(altitude_gps) +
    "*";


  Serial.println(strlen(dataString.c_str())+1);
  strcpy(dataChar, dataString.c_str());

  char endingcontrolCs[250-sizeof(dataChar)];
  char dataChar[strlen(dataString.c_str())+1];
  char FinalString[250];
  
  memset(endingcontrolCs,'*',(250-sizeof(dataChar)));
  
  strcat(FinalString,dataChar);
  strcat(FinalString,endingcontrolCs);
  
  if (SERIAL_DEBUGGING) {Serial.println(endingcontrolCs);}
  if (SERIAL_DEBUGGING) {Serial.println(dataChar);}
  if (SERIAL_DEBUGGING) {Serial.println(FinalString);}
  
  
  logFile.println(dataChar);
  logFile.flush();

  if (lastCommunication + TIME_BETWEEN_COMMUNICATIONS < millis())
  {
    DOWNLINK_SERIAL.print(dataChar);
    lastCommunication = millis();
    if (SERIAL_DEBUGGING) {Serial.println(dataChar);}
  }
  
  if (LED_DEBUGGING) 
  {
    delay(500);
    digitalWrite(LED_PIN, LOW);
    delay(500);
  }
  
}


/******************************************************************************
  Complete parachute deployment sequence while collecting data
******************************************************************************/
void deployChute(int chutePin)
{
  unsigned long timeNow = millis();
  unsigned long targetTime;

  digitalWrite(chutePin, HIGH); //fire parachute 
  logFile.println("Parachute " + String(chutePin, DEC) + " fired " + String(timeNow, DEC) + " milliseconds after program start and at an altitude of " + String(altitude, DEC));
  
  targetTime = timeNow + 4000; //wait 4 seconds to ensure chute is deployed and only one filter is contaminated at once
  while(timeNow < targetTime)
  {
    updateData();
    timeNow = millis();
  }

  digitalWrite(chutePin, LOW); //Stop sending signal to fire parchutes in case of a short
}

/******************************************************************************
  Spin sampler and keep track of which sample it's on
******************************************************************************/
void spinSampler() {
  if (filterNumber <= NUM_FILTERS){  // So it won't fire if it is activated too many times
    samplerStepper.step(STEPS_PER_FILTER);
    filterNumber++;
    logFile.println("Spun to filter " + String(filterNumber, DEC) + " at " +  String(altitude, DEC) + " meters above sea level and at approximately " + String(millis(), DEC) + " seconds after startup");
  }
}

/******************************************************************************
  Parse IMU and assign global variables.
******************************************************************************/
void parseIMUStream() {
  String input;
  char temp[250];
  char *token;
  bool stringNotRead=true;
  
  while (stringNotRead && IMU_SERIAL.available()){
    if(IMU_SERIAL.read() == '$'){
      input = IMU_SERIAL.readStringUntil('*');
      stringNotRead=false;
    }
  }
  
  strcpy(temp, input.c_str());

  token = strtok(temp, "|");
  yaw = atof(token);
  token = strtok(NULL, "|");
  pitch = atof(token);
  token = strtok(NULL, "|");
  roll = atof(token);
  token = strtok(NULL, "|");
  x_accel = atof(token);
  token = strtok(NULL, "|");
  y_accel = atof(token);
  token = strtok(NULL, "|");
  z_accel = atof(token);
  token = strtok(NULL, "|");
  ang_accel_x = atof(token);
  token = strtok(NULL, "|");
  ang_accel_y = atof(token);
  token = strtok(NULL, "|");
  ang_accel_z = atof(token);
  token = strtok(NULL, "|");
  mag_x = atof(token);
  token = strtok(NULL, "|");
  mag_y = atof(token);
  token = strtok(NULL, "|");
  mag_z = atof(token);
  
}

/******************************************************************************
  Parse GPS and assign glabl variables.
******************************************************************************/
void parseGPSStream() { 
  while (GPS_SERIAL.available()){
    int c = GPS_SERIAL.read();
    if(gps.encode(c)){//if statement is true if a full string has been read
      if(gps.satellites() > 1){ // Is this needed?
        gps.get_position(&latitude, &longitude); //assigns by reference
        altitude_gps = gps.altitude()/100.0;
      }
    }
  }
}
