#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <Wire.h>               //permite la comunicación a través del bus I2C
#include <LiquidCrystal_I2C.h>  //La librería para controlar la pantalla lcd
#include <SPI.h>
#include <MFRC522.h>

#define STACK_SIZE 128
#define DELAY_ACCESS (100 / portTICK_PERIOD_MS)
#define DELAY_REGISTRAR_ACCESS (1000 / portTICK_PERIOD_MS)
#define DELAY_RFID (100 / portTICK_PERIOD_MS)

#define SS_PIN 4    //MFRC522 SDA
#define RST_PIN 17  //MFRC522 RST
#define CS_PIN 13   //Micro-SD CS        hay que ver donde inicializar este
#define RELAY_PIN 5
#define BUZZER_PIN 15
#define LEDR_PIN 14
#define LEDG_PIN 25

MFRC522 rfid(SS_PIN, RST_PIN);     // Creamos el objeto rfid definiendo los pines se utilizará: 5 y 17
LiquidCrystal_I2C lcd(0x27, 16, 2);  //Creamos el objeto `lcd`, 32 es la direccion del modulo, 16 cantidad de columnas y 2 cantidad de filas del display

SemaphoreHandle_t semaforo = NULL;
QueueHandle_t colaUIDs;// Cola para pasar UID entre tareas

TickType_t timeout = (TickType_t)5;// tengo que ver donde implementar
TaskHandle_t rfidHandle;
TaskHandle_t accesoHandle;
TaskHandle_t registroHandle;

// UIDs autorizados
byte managerKeyUID[4] = { 0x40, 0x41, 0xD5, 0x19 };
//byte secretaryKeyUID[4] = { 0x23, 0x7F, 0x38, 0xD9 }; // ESTE ES EL CORRECTO
byte secretaryKeyUID[4] = {0x30, 0x01, 0x8B, 0x15}; //ESTE ES EL DE PRUEBA 

void crearSemaforo();
void crearTareas();
void sonarBuzzer(int duracionMS);

void registrarAcceso(const char* usuario);
void leerRFID(void *parametro);
void controlAcceso(void *parametro);

void setup() {

  crearSemaforo();
  colaUIDs = xQueueCreate(5, sizeof(byte[4])); // Crear cola con espacio para 5 UIDs de 4 bytes

  Serial.begin(115200);  
  SPI.begin();        //  Inicializar bus SPI, es para el lector RFID
  rfid.PCD_Init();    //  Inicializar módulo MFRC522
  lcd.init();       //Inicializa la pantalla lcd
  lcd.backlight();  //Enciende la luz de fondo (backlight) del LCD para que sea visible.

  pinMode(RELAY_PIN, INPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);             // Comenzar con la cerradura bloqueada
  digitalWrite(BUZZER_PIN, LOW);             // Buzzer apagado
  pinMode(LEDR_PIN, OUTPUT);
  pinMode(LEDG_PIN, OUTPUT);

  //digitalWrite(LEDG_PIN, HIGH);

  crearTareas();

  Serial.println("Sistema listo. Escanee una tarjeta...");
}

void loop() {}

void crearTareas() {
  xTaskCreate(
    leerRFID,
    "Tarea de leer el RFID",
    2048,
    NULL,
    1,
    &rfidHandle
  );

  xTaskCreate(
    controlAcceso,
    "Tarea de control de acceso",
    4096,
    NULL,
    2,
    &accesoHandle
  );
}

void crearSemaforo() {
  if (semaforo == NULL) {
    //Crear el semaforo
    semaforo = xSemaphoreCreateMutex();

    //Liberamos el semaforo
    if (semaforo != NULL) {
      xSemaphoreGive(semaforo);
    }
  }
}

void sonarBuzzer(int duracionMS) {
  digitalWrite(BUZZER_PIN, HIGH);
  vTaskDelay(duracionMS / portTICK_PERIOD_MS);
  digitalWrite(BUZZER_PIN, LOW);
}


void registrarAcceso(const char* usuario) {
  xTaskCreate(
    [](void* param) {
      const char* usuario = (const char*)param;
      int sem_obtenido = xSemaphoreTake(semaforo, 0);//llamamos al semaforo que queremos y ponemos el tiempo que vamos a esperar para recibir el semaforo
      if (sem_obtenido == pdTRUE) {

        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("Acceso: ");
        lcd.print(usuario);
        vTaskDelay(2000);

        lcd.clear();
        xSemaphoreGive(semaforo);
      }
      vTaskDelete(NULL);
    },
    "Registrar LCD",
    2048,
    (void*)usuario,
    1,
    &registroHandle
  );
}

void leerRFID(void *parametro) {

  while (1) {
    if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
      byte uid[4];
      for (int i = 0; i < 4; i++) uid[i] = rfid.uid.uidByte[i];  // Copiar el UID leído

      xQueueSend(colaUIDs, &uid, portMAX_DELAY);  // Enviar el UID a la cola para que lo lea otra tarea

      rfid.PICC_HaltA();       // Detener la tarjeta (halt)
      rfid.PCD_StopCrypto1();  // Detener la encriptación (modo seguro del lector)
    }
    vTaskDelay(200);
  }
}

void controlAcceso(void *parametro) {
  static int i = 0;
  while (1) {
    byte uid[4];  // Arreglo local para recibir UID desde la cola

    if (xQueueReceive(colaUIDs, &uid, portMAX_DELAY)) {  // Espera bloqueante hasta que haya un UID en la cola
      bool isManager = true;                             // Bandera: ¿es el gerente?
      bool isSecretary = true;                           // Bandera: ¿es el secretario?

      for (int i = 0; i < 4; i++) {
        if (uid[i] != managerKeyUID[i]) isManager = false;      // Comparar byte por byte con UID del gerente
        if (uid[i] != secretaryKeyUID[i]) isSecretary = false;  // Comparar con UID del secretario
      }

      if (isManager) {
        Serial.println("Access granted to manager");  // Mostrar mensaje por serial
        digitalWrite(LEDG_PIN, HIGH);                 // Encender el led verde
        pinMode(RELAY_PIN, OUTPUT);                   // Activar relé para desbloquear cerradura
        sonarBuzzer(200);                             // Sonar buzzer por 200ms
        vTaskDelay(1000);
        pinMode(RELAY_PIN, INPUT);                    // Volver a bloquear la cerradura 
        registrarAcceso("Manager");
        digitalWrite(LEDG_PIN, LOW);                  // Apagar el led verde
      } else {
        Serial.print("Access denied. UID: ");  // UID no reconocido: denegar acceso
        for (int i = 0; i < 4; i++) {
          Serial.print(uid[i] < 0x10 ? "0" : "");  // Formatear UID en hexadecimal
          Serial.print(uid[i], HEX);
          Serial.print(" ");
        }
        Serial.println();

        int sem_obtenido = xSemaphoreTake(semaforo, 0);//llamamos al semaforo que queremos y ponemos el tiempo que vamos a esperar para recibir el semaforo
        if (sem_obtenido == pdTRUE) {
          vTaskPrioritySet(NULL, 3); // subir prioridad

          digitalWrite(LEDR_PIN, HIGH);
          lcd.clear();
          lcd.setCursor(0, 0);
          lcd.print("Acceso denegado");

          pinMode(RELAY_PIN,INPUT);
          registrarAcceso("Secretary");
          sonarBuzzer(200);
          vTaskDelay(1000);
          digitalWrite(LEDR_PIN, LOW);

          lcd.clear();

          xSemaphoreGive(semaforo);
          vTaskPrioritySet(NULL, 2); // restaurar prioridad
        }
      }
    }
  }
}

