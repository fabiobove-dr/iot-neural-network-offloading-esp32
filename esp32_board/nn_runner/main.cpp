/* 
 * ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
 * OTHER LIBS
 * ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
 */
#include <WiFi.h>
#include <PubSubClient.h>
#define MQTT_MAX_PACKET_SIZE 2*1024 // Important: Adjust size to correctly send and recieve topic messages
#include <sys/time.h>
#include <UUID.h>
#include <ArduinoJson.h>

/* 
 * ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
 * NN Modles Layers
 * ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
 */
#define MODEL_NAME "test_model"
#include "model_layers/layer_0.h"
#include "model_layers/layer_1.h"
#include "model_layers/layer_2.h"
#include "model_layers/layer_3.h"
#include "model_layers/layer_4.h"
const int MAX_NUM_LAYER = 5;
float imageData[10][10] = {};
const int imageHeight = 10;
const int imageWidth  = 10;

/* 
* ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
* LIBS for TFLITE
* ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
*/
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"
#include "tensorflow/lite/micro/micro_error_reporter.h"
#include "tensorflow/lite/micro/all_ops_resolver.h"
#include "tensorflow/lite/micro/system_setup.h"
/* 
* ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
*  CONFIGURATIONS
* ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
*/
#include "conf.h"
/* 
* ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
*  GLOBAL VARIABLES
* ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
*/
// NN Variables
tflite::MicroErrorReporter micro_error_reporter;
tflite::ErrorReporter* error_reporter = &micro_error_reporter;
const tflite::Model* model            = nullptr;
tflite::MicroInterpreter* interpreter = nullptr;
TfLiteTensor* input                   = nullptr;
TfLiteTensor* output                  = nullptr;
constexpr int kTensorArenaSize        = 2*1024;
uint8_t tensor_arena[kTensorArenaSize];
bool modelLoaded                      = false;
// Communication & Offloading Variables
WiFiClient espClient;
PubSubClient client(espClient);
int computedLayer       = 0;
struct tm timeinfo;
UUID uuid;
String MessageUUID      = "";
const int nonValidLayer = 999;
int offloadingLayer     = nonValidLayer;
bool offloaded          = false;
bool analyticsPublished = false;
bool modelDataLoaded    = false;

/*
 * ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
 * GENERATE MESSAGE UUID
 * ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
*/
void generateMessageUUID(){
  // Generate a UUID
  unsigned long seed = esp_random();
  uuid.seed(seed);
  uuid.setRandomMode();
  uuid.generate();
  MessageUUID = (String)uuid.toCharArray();
  MessageUUID = MessageUUID.substring(0, 4);
 }

/* 
* ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
* LOAD NN LAYER
* ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
*/
void loadNNLayer(String layer_name){
  // Import del layer da eseguire -> Nome nell'header file
  int layer_id = layer_name.substring(6).toInt();
  switch (layer_id) {
      case 0: model = tflite::GetModel(layer_0); break;
      case 1: model = tflite::GetModel(layer_1); break;
      case 2: model = tflite::GetModel(layer_2); break;
      case 3: model = tflite::GetModel(layer_3); break;
      case 4: model = tflite::GetModel(layer_4); break;
  }

  if (model->version() != TFLITE_SCHEMA_VERSION) {
      Serial.print("Model provided is schema version not equal to supported!\n");
      return;
  } else {
      Serial.print("\nModel Layer Loaded! \n");
      modelLoaded = true;
  }
  // Questo richiama tutte le implementazioni delle operazioni di cui abbiamo bisogno
  tflite::AllOpsResolver resolver;
  tflite::MicroInterpreter static_interpreter(model, resolver, tensor_arena, kTensorArenaSize, error_reporter);
  interpreter = &static_interpreter;
  Serial.print("Interpreter ready");

  // Alloco la memoria del tensor_arena per i tensori del modello
  TfLiteStatus allocate_status = interpreter->AllocateTensors();
  if (allocate_status != kTfLiteOk) {
      Serial.println("AllocateTensors() failed");
      return;
  } else {Serial.println("AllocateTensors() done\n");}
}

/* 
* ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
* INFERENCE FOR NN LAYER
* ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
*/
StaticJsonDocument<512> runNNLayer(int offloading_layer_index) {
  // Generate the JSON message
  StaticJsonDocument<512> jsonDoc;
  JsonArray outputArray;  // Declare outputArray outside the loop

  for (int i = 0; i < offloading_layer_index; i++) {
    String layer_name = "layer_" + String(i);
    float inizio = micros();

    while (!modelLoaded){
      Serial.println("Loading Model before Inference...");
      loadNNLayer(layer_name);
      delay(1000);
    }

    // Pass image data to the model
    for (int i = 0; i < imageHeight; ++i) {
        for (int j = 0; j < imageWidth; ++j) {
            input->data.f[i * imageWidth + j] = imageData[i][j];
        }
    }

    // Run inference
    input = interpreter->input(0);
    output = interpreter->output(0);
    interpreter->Invoke();

    // Assuming output is a TfLiteTensor
    const float* outputData = interpreter->typed_output_tensor<float>(0);

    // Initialize layer_output[i] as an array
    outputArray = jsonDoc["layer_output"][i].to<JsonArray>();

    for (int j = 0; j < output->dims->data[0]; ++j) {
      // Add each element to the array
      outputArray.add(outputData[j]);
    }

    jsonDoc["layer_inference_time"][i] = (micros() - inizio);
    Serial.println("Computed layer: " + String(i) + " Inf Time: " + String(micros() - inizio));
  }

  // Now, you can find the last layer output outside the loop
  jsonDoc["last_layer_output"] = outputArray[outputArray.size() - 1];
  return jsonDoc;
}

/* 
* ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
* WIFI CONFIGURATION
* ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
*/
void wifiConfiguration(){
  WiFi.mode(WIFI_STA);
  WiFi.begin(SSID, PWD);
  Serial.println("Connecting to WiFi...");
  while (WiFi.waitForConnectResult() != WL_CONNECTED) {
    Serial.println(".");
    delay(500);
    ESP.restart();
  } 
  Serial.print("Connected to WiFi - IP Address: ");
  Serial.println(WiFi.localIP());
  delay(500);
}

/* 
* ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
* MQTT CONFIGURATION
* ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
*/
void mqttConfiguration(){
  client.setServer(MQTT_SRV, MQTT_PORT);
  while (!client.connect("ESP32Client", "", "")) {
    Serial.println("Connecting to MQTT Broker");
    if (!client.connected()) {
      Serial.print("Failed to connect to MQTT Broker - retrying, rc=");
      Serial.print(client.state());
      delay(500);
    }
  }
  client.setBufferSize(2*1024); // Important: Adjust size to correctly send and recieve topic messages
  Serial.println("Connected to MQTT Broker");
}

/* 
* ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
* TIMER CONFIGURATION & FLOATING-POINT TIMESTAMP
* ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
*/
String getCurrTimeStr(){
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  time_t currentTime = tv.tv_sec;
  int milliseconds = tv.tv_usec / 1000;
  int microseconds = tv.tv_usec % 1000000;
  char currentTimeStr[30];
  snprintf(currentTimeStr, sizeof(currentTimeStr), "%ld.%03d%03d", currentTime, milliseconds, microseconds);
  String currentTimeString = String(currentTimeStr);
  return currentTimeString;
}

void timeConfiguration(){
  // Configure NTP time synchronization
  configTime(NTP_GMT_OFFSET, NTP_DAYLIGHT_OFFSET, NTP_SRV);
  Serial.println("Connecting to NTP Server");
  // Try obtaining the time until successful
  struct tm timeinfo;
  while (!getLocalTime(&timeinfo)) {
    delay(500);
  }

  // Print current time
  Serial.print("NTP Time Configured - Current Time: ");
  Serial.println(getCurrTimeStr());
  return;
}

/* 
* ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
* GET OFFLOADING INFORMATION
* ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
*/
void getOffloadingInformation(DynamicJsonDocument messageData) {
  // Access specific fields in the JSON message
  String nn_layer = messageData["layer"];
  offloadingLayer = nn_layer.toInt();
  // Print some values from the parsed JSON message
  Serial.print("Offloading Layer: "+ String(nn_layer));
}

/* 
* ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
* GET MODEL DATA FOR PREDICTION
* ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
*/
void getModelDataForPrediction(DynamicJsonDocument messageData) {
  // Access specific fields in the JSON message
  String inputData = messageData["input_data"];
  // Convert the inputData string to a 2D array
  for (int i = 0; i < imageHeight; ++i) {
    for (int j = 0; j < imageWidth; ++j) {
      imageData[i][j] = inputData[i * imageWidth + j] - '0'; // Assuming inputData contains numeric characters
    }
  }
  // Print some values from the parsed JSON message
  Serial.println("Model input data recieved");
  modelDataLoaded = true;
}


void dispatchCallbackMessages(){
  client.subscribe("comunication/edge/nn_offloading");
  client.subscribe("comunication/edge/nn_input_data");
  client.setCallback([](char* topic, byte* payload, unsigned int length) {
    // Convert the incoming message to a string
    String message = "";
    for (int i = 0; i < length; i++) {
      message += (char)payload[i];
    }
    // Parse the JSON message and store it in the DynamicJsonDocument
    DynamicJsonDocument doc(2*1024); // Important: Fix size to correctly recieve and store the model input data
    DeserializationError error = deserializeJson(doc, message);
    // Check for parsing errors
    if (error) {
      Serial.println(error.c_str());
      return;
    }
    Serial.println(topic);

    if (strcmp(topic, "comunication/edge/nn_offloading") == 0) { getOffloadingInformation(doc);}
    if (strcmp(topic, "comunication/edge/nn_input_data") == 0) { getModelDataForPrediction(doc);}
  });
}


/*
 * ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
* PUBLISH DEVICE ANALYTICS
* ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
*/
void publishDeviceAnaytics(){
  if (!analyticsPublished){
    // Generate the JSON message
    StaticJsonDocument<512> jsonDoc;
    const int firstRunOffloadingLayer = MAX_NUM_LAYER;
    jsonDoc = runNNLayer(firstRunOffloadingLayer);
    jsonDoc["timestamp"] = getCurrTimeStr();
    jsonDoc["messageUIID"] = MessageUUID;
    jsonDoc["nn_id"] = MODEL_NAME;
    // Serialize the JSON document to a string
    String jsonMessage;
    serializeJson(jsonDoc, jsonMessage);
    // Publish the JSON message to the topic
    client.publish("comunication/device/nn_analytics", jsonMessage.c_str(), 2);
    Serial.println("Published Device Analytics");
    analyticsPublished = true;
  }
}

/*
* ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
* NN MODEL PREDICT 
* ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
*/
void predictAndOffload(){
  if(offloadingLayer != nonValidLayer && !offloaded){
    offloaded = true;

    StaticJsonDocument<512> jsonDocNNOutput;
    StaticJsonDocument<512> jsonDoc;

    jsonDocNNOutput = runNNLayer(offloadingLayer);

    // Generate the JSON message & Fill in the JSON data
    jsonDoc = jsonDocNNOutput;
    jsonDoc["last_computed_layer"] = ""+String(offloadingLayer)+"";
    jsonDoc["timestamp"] = getCurrTimeStr();
    jsonDoc["messageUIID"] = MessageUUID;
    jsonDoc["nn_id"] = MODEL_NAME;

    // Serialize the JSON document to a string and Publish the JSON message to the topic
    String jsonMessage;
    serializeJson(jsonDoc, jsonMessage);
    client.publish("comunication/device/nn_offloading", jsonMessage.c_str(), 2);
    Serial.println("\nPerformed Offloading from layer: " + String(offloadingLayer));
  }
}

/* 
 * ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
 * SETUP 
 * ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
 */
void setup() {
  Serial.begin(115200);
  wifiConfiguration();          // Wi-Fi Connection
  mqttConfiguration();          // MQTT
  timeConfiguration();          // Synchronize Timer - NTP server
  generateMessageUUID();        // Generate an Identifier for the message
  dispatchCallbackMessages();
}

/* 
 * ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
 * LOOP 
 * ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
 */
void loop() {
  client.loop(); 
  if(modelDataLoaded){
    publishDeviceAnaytics();    // Publishes to a topic the inference time of each layer on the device
  }   
  /*        
  predictAndOffload();
  if (offloaded){
    delay(5000);
    ESP.restart();
  }*/
}