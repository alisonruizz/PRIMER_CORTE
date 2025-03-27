/**
 * @file main.cpp
 * @brief Sistema de monitoreo ambiental con ESP32 usando FreeRTOS
 */

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

// Definición de pines
#define DHTPIN 4         ///< Pin de datos del sensor DHT11
#define LDRPIN 34        ///< Pin del sensor LDR (fotorresistencia)
#define LED_PIN 5        ///< Pin del LED indicador
#define DHTTYPE DHT11    ///< Tipo de sensor DHT (DHT11)
#define BUTTON_PIN_1 18  ///< Primer botón para interrupción
#define BUTTON_PIN_2 19  ///< Segundo botón para interrupción

DHT dht(DHTPIN, DHTTYPE);  ///< Objeto sensor DHT
RTC_DS3231 rtc;            ///< Objeto RTC DS3231

// Colas y semáforos
QueueHandle_t sensorQueue;  ///< Cola para datos de sensores (temperatura, humedad, luz)
QueueHandle_t rtcQueue;     ///< Cola para datos del RTC (fecha y hora)
QueueHandle_t tramaQueue;   ///< Cola para tramas formateadas completas
SemaphoreHandle_t ledSemaphore; ///< Semáforo binario para controlar el LED de alarma

// Variables persistentes en Deep Sleep para que los datos se conserven despues de estar en este modo
RTC_DATA_ATTR int contador = 0;     ///< Contador de pulsaciones persistente
RTC_DATA_ATTR int wakeCounter = 0;  ///< Contador de reinicios persistente

/**
 * @struct SensorData
 * @brief Estructura para almacenar datos de los sensores ambientales
 * 
 * Esta estructura se usa para enviar datos a través de sensorQueue.
 * Cuando un campo es invalido o no es logico, se establece en -1.
 */
struct SensorData {
  float temperature; 
  float humidity;    
  int light;         
};

/**
 * @struct RTCData
 * @brief Estructura para almacenar datos de fecha y hora
 * 
 * Esta estructura se usa para enviar datos a través de rtcQueue.
 */
struct RTCData {
  int hour;    ///< Hora actual 
  int minute;  ///< Minutos actuales 
  int second;  ///< Segundos actuales 
  int day;     ///< Día del mes 
  int month;   ///< Mes actual 
  int year;    ///< Año actual 
};

/**
 * @brief Tarea para lectura del sensor DHT11
 * 
 * Esta tarea se ejecuta cada 2 segundos y:
 * 1. Lee temperatura y humedad del sensor DHT11
 * 2. Verifica que las lecturas sean válidas
 * 3. Crea una estructura SensorData con los valores leídos
 * 4. Envía los datos a sensorQueue (cola de 10 elementos)
 * 
 * Comunicación:
 * - Productor de la cola sensorQueue (envía datos)
 * - No consume de ninguna cola
 * 
 */
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

/**
 * @brief Tarea para lectura del sensor LDR
 * 
 * Esta tarea se ejecuta cada segundo y:
 * 1. Lee el valor analógico del LDR 
 * 2. Crea una estructura SensorData con el valor de luz
 * 3. Envía los datos a sensorQueue
 * 
 * Comunicación:
 * - Productor de la cola sensorQueue (envía datos)
 * - No consume de ninguna cola
 */
void tareaLDR(void *pvParameters) {
  while (1) {
    int lightValue = analogRead(LDRPIN);
    SensorData data = {-1, -1, lightValue};
    xQueueSend(sensorQueue, &data, portMAX_DELAY);
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

/**
 * @brief Tarea para lectura del RTC
 * 
 * Esta tarea se ejecuta cada segundo y:
 * 1. Obtiene la fecha y hora actual del compilador
 * 2. Crea una estructura RTCData con los valores
 * 3. Envía los datos a rtcQueue (cola de 5 elementos)
 * 
 * Comunicación:
 * - Productor de la cola rtcQueue (envía datos)
 * - No consume de ninguna cola
 */
void tareaRTC(void *pvParameters) {
  while (1) {
    DateTime now = rtc.now();
    RTCData rtcData = {now.hour(), now.minute(), now.second(), 
                       now.day(), now.month(), now.year()};
    xQueueSend(rtcQueue, &rtcData, portMAX_DELAY);
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

/**
 * @brief Tarea para mostrar datos y gestionar alarmas

 * Esta tarea:
 * 1. Recibe datos de sensorQueue 
 *    - Muestra temperatura/humedad o luz por serial
 *    - Activa semáforo si supera umbrales (temp>24 y hum>70 o luz>500)
 * 2. Recibe datos de rtcQueue 
 *    - Muestra fecha y hora formateada por serial
 * 
 * Comunicación:
 * - Consumidor de sensorQueue y rtcQueue (recibe datos)
 * - Productor del semáforo ledSemaphore (para activar alarma)
 * 
 * Sincronización:
 * - Usa semáforo para indicar condición de alarma a tareaAlarma
 */
void tareaMostrar(void *pvParameters) {
  SensorData receivedData;
  RTCData rtcData;

  while (1) {
    // Procesar datos de sensores
    if (xQueueReceive(sensorQueue, &receivedData, pdMS_TO_TICKS(100)) == pdPASS) {
      if (receivedData.temperature != -1 && receivedData.humidity != -1) {
        Serial.print("Temp: "); Serial.print(receivedData.temperature);
        Serial.print(" C - Hum: "); Serial.print(receivedData.humidity);
        Serial.println("%");
      }
      if (receivedData.light != -1) {
        Serial.print("Luz: "); Serial.println(receivedData.light);
      }

      // Lógica de alarma
      if ((receivedData.temperature > 24 && receivedData.humidity > 70) || 
          receivedData.light > 500) {
        xSemaphoreGive(ledSemaphore); // Notificar alarma
      }
    }

    // Procesar datos del RTC
    if (xQueueReceive(rtcQueue, &rtcData, pdMS_TO_TICKS(100)) == pdPASS) {
      Serial.printf("Fecha: %02d/%02d/%04d - Hora: %02d:%02d:%02d\n",
                    rtcData.day, rtcData.month, rtcData.year,
                    rtcData.hour, rtcData.minute, rtcData.second);
    }
  }
}

/**
 * @brief Tarea para controlar el LED de alarma
 * 
 * Esta tarea:
 * 1. Espera indefinidamente al semáforo ledSemaphore
 * 2. Cuando lo recibe:
 *    - Enciende el LED por 500ms
 *    - Lo apaga
 * 
 * Comunicación:
 * - Consumidor del semáforo ledSemaphore(se usa un semanforo binario libre-ocupado)
 * - No produce datos para otras tareas
 * 
 * Sincronización:
 * - Se bloquea hasta que tareaMostrar libera el semáforo
 * - Prioridad más alta para respuesta rápida
 */
void tareaAlarma(void *pvParameters) {
  while (1) {
    if (xSemaphoreTake(ledSemaphore, portMAX_DELAY) == pdPASS) {
      digitalWrite(LED_PIN, HIGH);
      vTaskDelay(pdMS_TO_TICKS(500));
      digitalWrite(LED_PIN, LOW);
    }
  }
}

/**
 * @brief Tarea para crear tramas formateadas
 * 
 * Esta tarea:
 * 1. Mantiene últimos valores válidos de temperatura/humedad
 * 2. Recibe datos de sensores 
 * 3. Recibe datos del RTC 
 * 4. Cuando tiene todos los datos, crea una trama:
 *    "DD/MM/AAAA HH:MM:SS, Temp: X.XX C, Hum: XX.XX%, Luz: XXXX"
 * 5. Envía la trama a tramaQueue
 * 
 * Comunicación:
 * - Consumidor de sensorQueue y rtcQueue
 * - Productor de tramaQueue
 */
void tareaCrearTrama(void *pvParameters) {
  SensorData sensorData;
  RTCData rtcData;
  char trama[100];
  
  static float lastTemperature = -1;
  static float lastHumidity = -1;

  while (1) {
    // Actualizar últimos valores de sensores
    if (xQueueReceive(sensorQueue, &sensorData, pdMS_TO_TICKS(1000)) == pdPASS) {
      if (sensorData.temperature != -1) lastTemperature = sensorData.temperature;
      if (sensorData.humidity != -1) lastHumidity = sensorData.humidity;
    }

    // Cuando hay datos del RTC, crear trama completa
    if (xQueueReceive(rtcQueue, &rtcData, pdMS_TO_TICKS(1000)) == pdPASS) {
      snprintf(trama, sizeof(trama), 
               "%02d/%02d/%04d %02d:%02d:%02d, Temp: %.2f C, Hum: %.2f%%, Luz: %d",
               rtcData.day, rtcData.month, rtcData.year,
               rtcData.hour, rtcData.minute, rtcData.second,
               lastTemperature, lastHumidity, sensorData.light);

      xQueueSend(tramaQueue, &trama, portMAX_DELAY);
    }
    
    vTaskDelay(pdMS_TO_TICKS(5000));
  }
}

/**
 * @brief Tarea para mostrar tramas formateadas
 * 
 * Esta tarea:
 * 1. Espera tramas de tramaQueue
 * 2. Las muestra por el puerto serial
 * 3. Se ejecuta cada 5 segundos
 * 
 * Comunicación:
 * - Consumidor de tramaQueue
 * - No produce datos para otras tareas
 */
void tareaMostrarTrama(void *pvParameters) {
  char trama[100];

  while (1) {
    if (xQueueReceive(tramaQueue, &trama, portMAX_DELAY) == pdPASS) {
      Serial.println(trama);
    }
    vTaskDelay(pdMS_TO_TICKS(5000));
  }
}

/**
 * @brief ISR para manejo de pulsadores
 * 
 * Esta ISR:
 * 1. Se ejecuta cuando hay flanco de bajada en los botones, usando configuración de resistencia pull-up
 * 2. Incrementa el contador global si se presiona cualquiera
 * 
 * Características:
 * - No usa colas ni semáforos directamente
 */
void IRAM_ATTR buttonISR() {
  if (digitalRead(BUTTON_PIN_1) == LOW && digitalRead(BUTTON_PIN_2) == LOW) {
    contador++;
  }
}

/**
 * @brief Tarea para mostrar el contador de pulsaciones
 * 
 * Esta tarea:
 * 1. Muestra el valor del contador por serial cada segundo
 * 2. Usa variable contador marcada como RTC_DATA_ATTR
 * 
 * Comunicación:
 * - Accede a variable global compartida (contador)
 * - No usa colas ni semáforos
 */
void tareaMostrarContador(void *pvParameters) {
  while (1) {
    Serial.print("Contador: ");
    Serial.println(contador);
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

/**
 * @brief Configura y activa el modo Deep Sleep
 * 
 * Esta función:
 * 1. Configura fuente de wakeup:
 *    - timer: Después de 30 segundos
 * 2. Inicia el modo Deep Sleep
 * 
 * Nota:
 * - Las variables marcadas con RTC_DATA_ATTR se preservan
 * - El consumo de energía se reduce
 */
void enterDeepSleep() {
    Serial.println("Entrando en Deep Sleep...");
    esp_sleep_enable_timer_wakeup(30 * 1000000);
    esp_deep_sleep_start();
}

/**
 * @brief Función de configuración inicial
 * 
 * Esta función:
 * 1. Inicializa periféricos (Serial, DHT, I2C, RTC)
 * 2. Configura pines (LED, botones)
 * 3. Configura interrupciones para los botones
 * 4. Crea colas y semáforos
 * 5. Crea todas las tareas de FreeRTOS
 * 6. Inicia el contador de reinicios
 */
void setup() {
  Serial.begin(115200);
  dht.begin();
  Wire.begin();

  // Inicialización RTC
  if (!rtc.begin()) {
    Serial.println("No se encontró RTC");
    while (1);
  }

  if (rtc.lostPower()) {
    Serial.println("RTC perdió la hora, estableciendo nueva hora...");
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }

  // Configuración de pines
  pinMode(LED_PIN, OUTPUT);
  pinMode(BUTTON_PIN_1, INPUT_PULLUP);
  pinMode(BUTTON_PIN_2, INPUT_PULLUP);

  // Configuración de interrupciones
  attachInterrupt(digitalPinToInterrupt(BUTTON_PIN_1), buttonISR, FALLING);
  attachInterrupt(digitalPinToInterrupt(BUTTON_PIN_2), buttonISR, FALLING);

  // Creación de objetos FreeRTOS
  sensorQueue = xQueueCreate(10, sizeof(SensorData));
  rtcQueue = xQueueCreate(5, sizeof(RTCData));
  tramaQueue = xQueueCreate(5, sizeof(char[100]));
  ledSemaphore = xSemaphoreCreateBinary();

      // Creación de tareas
    xTaskCreate(tareaMostrarContador, "MostrarContador", 1024, NULL, 1, NULL);
    xTaskCreate(tareaDHT, "DHT11", 1024, NULL, 1, NULL);
    xTaskCreate(tareaLDR, "LDR", 1024, NULL, 1, NULL);
    xTaskCreate(tareaRTC, "RTC", 2048, NULL, 1, NULL);
    xTaskCreate(tareaMostrar, "Mostrar", 2048, NULL, 1, NULL);
    xTaskCreate(tareaAlarma, "Alarma", 1024, NULL, 2, NULL);
    xTaskCreate(tareaCrearTrama, "CrearTrama", 2048, NULL, 1, NULL);
    xTaskCreate(tareaMostrarTrama, "MostrarTrama", 2048, NULL, 1, NULL);

    // Información de reinicio
    wakeCounter++;
    Serial.print("Reinicio número: ");
    Serial.println(wakeCounter);

    // Tarea para manejar el Deep Sleep
    xTaskCreate([](void* pvParameters) {
        while (1) {
            Serial.println("Sistema en ejecución...");
            vTaskDelay(pdMS_TO_TICKS(10000));
            enterDeepSleep();
        }
        }, "GestionSleep", 1024, NULL, 1, NULL);
}

/**
 * @brief Función loop principal (no utilizada en FreeRTOS)
 *
 * En sistemas con FreeRTOS, el loop principal no se usa. Las tareas se manejan por el scheduler o planificador.
 */
void loop() {
    vTaskDelete(NULL); // Elimina la tarea del loop ya que no se usa
}
