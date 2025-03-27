#include <Arduino.h>
#include <DHT.h>
#include <Wire.h>
#include "RTClib.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"
#include "esp_sleep.h"

#define DHTPIN 4
#define LDRPIN 34
#define LED_PIN 5
#define DHTTYPE DHT11
#define BUTTON_PIN_1 18
#define BUTTON_PIN_2 19

DHT dht(DHTPIN, DHTTYPE);
RTC_DS3231 rtc;

QueueHandle_t sensorQueue;
QueueHandle_t rtcQueue;
QueueHandle_t tramaQueue;
SemaphoreHandle_t ledSemaphore;

RTC_DATA_ATTR int contador = 0; 
RTC_DATA_ATTR int wakeCounter = 0; 

struct SensorData {
  float temperature;
  float humidity;
  int light;
};

struct RTCData {
  int hour;
  int minute;
  int second;
  int day;
  int month;
  int year;
};

void tareaDHT(void *pvParameters) {
  while (1) {
    float temp = dht.readTemperature();
    float hum = dht.readHumidity();

    if (!isnan(temp) && !isnan(hum)) {  
      SensorData data = {temp, hum, -1};
      xQueueSend(sensorQueue, &data, portMAX_DELAY);
    } else {
      Serial.println("Error al leer el sensor DHT11");
    }

    vTaskDelay(pdMS_TO_TICKS(2000));
  }
}

void tareaLDR(void *pvParameters) {
  while (1) {
    int lightValue = analogRead(LDRPIN);
    SensorData data = {-1, -1, lightValue};
    xQueueSend(sensorQueue, &data, portMAX_DELAY);
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

void tareaRTC(void *pvParameters) {
  while (1) {
    DateTime now = rtc.now();
    RTCData rtcData = {now.hour(), now.minute(), now.second(), now.day(), now.month(), now.year()};
    xQueueSend(rtcQueue, &rtcData, portMAX_DELAY);
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

void tareaMostrar(void *pvParameters) {
  SensorData receivedData;
  RTCData rtcData;

  while (1) {
    if (xQueueReceive(sensorQueue, &receivedData, pdMS_TO_TICKS(100)) == pdPASS) {
      if (receivedData.temperature != -1 && receivedData.humidity != -1) {
        Serial.print("Temp: "); Serial.print(receivedData.temperature);
        Serial.print(" C - Hum: "); Serial.print(receivedData.humidity);
        Serial.println("%");
      }
      if (receivedData.light != -1) {
        Serial.print("Luz: "); Serial.println(receivedData.light);
      }

      if ((receivedData.temperature > 24 && receivedData.humidity > 70) || receivedData.light > 5000) {
        xSemaphoreGive(ledSemaphore);
      }
    }

    if (xQueueReceive(rtcQueue, &rtcData, pdMS_TO_TICKS(100)) == pdPASS) {
      Serial.printf("Fecha: %02d/%02d/%04d - Hora: %02d:%02d:%02d\n",
                    rtcData.day, rtcData.month, rtcData.year,
                    rtcData.hour, rtcData.minute, rtcData.second);
    }
  }
}

void tareaAlarma(void *pvParameters) {
  while (1) {
    if (xSemaphoreTake(ledSemaphore, portMAX_DELAY) == pdPASS) {
      digitalWrite(LED_PIN, HIGH);
      vTaskDelay(pdMS_TO_TICKS(500));
      digitalWrite(LED_PIN, LOW);
    }
  }
}

void tareaCrearTrama(void *pvParameters) {
  SensorData sensorData;
  RTCData rtcData;
  char trama[100];
  
  static float lastTemperature = -1;
  static float lastHumidity = -1;

  while (1) {

    if (xQueueReceive(sensorQueue, &sensorData, pdMS_TO_TICKS(1000)) == pdPASS) {
     
      if (sensorData.temperature != -1) {
        lastTemperature = sensorData.temperature;
      }
      if (sensorData.humidity != -1) {
        lastHumidity = sensorData.humidity;
      }
    }

    if (xQueueReceive(rtcQueue, &rtcData, pdMS_TO_TICKS(1000)) == pdPASS) {
      // Crear trama con los últimos valores válidos
      snprintf(trama, sizeof(trama), "%02d/%02d/%04d %02d:%02d:%02d, Temp: %.2f C, Hum: %.2f%%, Luz: %d",
               rtcData.day, rtcData.month, rtcData.year,
               rtcData.hour, rtcData.minute, rtcData.second,
               lastTemperature, lastHumidity, sensorData.light);

      xQueueSend(tramaQueue, &trama, portMAX_DELAY);
    }
    
    vTaskDelay(pdMS_TO_TICKS(5000));
  }
}

void tareaMostrarTrama(void *pvParameters) {
  char trama[100];

  while (1) {
    if (xQueueReceive(tramaQueue, &trama, portMAX_DELAY) == pdPASS) {
      Serial.println(trama);
    }
    vTaskDelay(pdMS_TO_TICKS(5000));
  }
}
void IRAM_ATTR buttonISR() {
  if (digitalRead(BUTTON_PIN_1) == LOW && digitalRead(BUTTON_PIN_2) == LOW) {
    contador++;
  }
}

void tareaMostrarContador(void *pvParameters) {
  while (1) {
    Serial.print("Contador: ");
    Serial.println(contador);
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

void enterDeepSleep() {
    Serial.println("Entrando en Deep Sleep...");
    esp_sleep_enable_timer_wakeup(30 * 1000000);
    esp_deep_sleep_start();
}

void setup() {
  Serial.begin(115200);
  dht.begin();
  Wire.begin();

  if (!rtc.begin()) {
    Serial.println("No se encontró RTC");
    while (1);
  }

  if (rtc.lostPower()) {
    Serial.println("RTC perdió la hora, estableciendo nueva hora...");
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }

  pinMode(LED_PIN, OUTPUT);
  pinMode(BUTTON_PIN_1, INPUT_PULLUP);
  pinMode(BUTTON_PIN_2, INPUT_PULLUP);

  attachInterrupt(digitalPinToInterrupt(BUTTON_PIN_1), buttonISR, FALLING);
  attachInterrupt(digitalPinToInterrupt(BUTTON_PIN_2), buttonISR, FALLING);

  sensorQueue = xQueueCreate(10, sizeof(SensorData));
  rtcQueue = xQueueCreate(5, sizeof(RTCData));
  tramaQueue = xQueueCreate(5, sizeof(char[100]));
  ledSemaphore = xSemaphoreCreateBinary();

  xTaskCreate(tareaMostrarContador, "MostrarContador", 1024, NULL, 1, NULL);
  xTaskCreate(tareaDHT, "DHT11", 1024, NULL, 1, NULL);
  xTaskCreate(tareaLDR, "LDR", 1024, NULL, 1, NULL);
  xTaskCreate(tareaRTC, "RTC", 2048, NULL, 1, NULL);
  xTaskCreate(tareaMostrar, "Mostrar", 2048, NULL, 1, NULL);
  xTaskCreate(tareaAlarma, "Alarma", 1024, NULL, 2, NULL);
  xTaskCreate(tareaCrearTrama, "CrearTrama", 2048, NULL, 1, NULL);
  xTaskCreate(tareaMostrarTrama, "MostrarTrama", 2048, NULL, 1, NULL);

  wakeCounter++;
  Serial.print("Reinicio número: ");
  Serial.println(wakeCounter);

  xTaskCreate([](void *pvParameters) {
    while (1) {
      Serial.println("Sistema en ejecución...");
      vTaskDelay(pdMS_TO_TICKS(10000));
      enterDeepSleep();
    }
  }, "GestionSleep", 1024, NULL, 1, NULL);
}

void loop() {
  vTaskDelete(NULL);
}