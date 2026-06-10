#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"

#include "pico/stdlib.h"
#include "hardware/uart.h"
#include "hardware/irq.h"
#include "hardware/i2c.h"
#include "hardware/pwm.h"

#include "pins.h"
#include "hc06.h"
#include "mpu6050.h"

// Edge Impulse — reconhecimento de gesto da MPU
#include "edge-impulse-sdk/classifier/ei_run_classifier.h"
#include "edge-impulse-sdk/dsp/numpy.hpp"
#include "model-parameters/model_metadata.h"
using namespace ei;

// ---------------------------------------------------------------------------
// Protocolo Pico → PC (via HC-06)
//   0xFF | TYPE | val_lo | val_hi
//
//   TYPE 0x00 : movimento X do mouse  (int16_t LE)
//   TYPE 0x01 : movimento Y do mouse  (int16_t LE)
//   TYPE 0x02 : centralizar mouse     val = 0x0001  (botão G — calibrar)
//   TYPE 0x10 : botão R (atirar)      val = 0x0001
//   TYPE 0x11 : botão G (lanterna)    val = 0x0001  (OBSOLETO — não enviado)
//   TYPE 0x12 : botão B (pausar)      val = 0x0001
//   TYPE 0x13 : gesto de chacoalhar   val = 0x0001
// ---------------------------------------------------------------------------

#define USE_BLUETOOTH       0       // 1 = HC-06, 0 = USB serial (teste com cabo)
#define DEBUG_MPU           0       // 1 = imprime valores da MPU em texto (não envia protocolo)

#define SAMPLE_PERIOD       0.01f   // 10 ms
#define PWM_WRAP            1000
#define PWM_FADE_STEP       10
#define PWM_FADE_DELAY_MS   10

// Reconhecimento de gesto (Edge Impulse)
#define GESTURE_LABEL       "Recharge"  // nome EXATO do label no seu export
#define GESTURE_THRESHOLD   0.7f
#define GESTURE_COOLDOWN_MS 800

typedef struct { int16_t accel[3]; int16_t gyro[3]; } mpu_data_t;
typedef struct { int axis; int16_t value; }             pos_t;
typedef struct { uint8_t type; int16_t value; }         bt_event_t;

static QueueHandle_t xQueueMPU;
static QueueHandle_t xQueueTX;
static QueueHandle_t xQueueBTN;
static QueueHandle_t xQueueGesture;        // amostras da MPU para a inferência

static TaskHandle_t xFusionHandle = NULL;  // notificado para recalibrar a MPU
static TaskHandle_t xMotorHandle  = NULL;  // notificado para pulsar o motor

// ---------------------------------------------------------------------------
// UART ISR — drena RX do HC-06 (ignoramos respostas em modo de jogo)
// ---------------------------------------------------------------------------
static void uart_rx_handler(void) {
    while (uart_is_readable(HC06_UART_ID))
        uart_getc(HC06_UART_ID);
}

static void init_uart_hc06(void) {
    uart_init(HC06_UART_ID, HC06_BAUD_RATE);
    gpio_set_function(HC06_TX_PIN, UART_FUNCSEL_NUM(HC06_UART_ID, HC06_TX_PIN));
    gpio_set_function(HC06_RX_PIN, UART_FUNCSEL_NUM(HC06_UART_ID, HC06_RX_PIN));
    uart_set_hw_flow(HC06_UART_ID, false, false);
    uart_set_format(HC06_UART_ID, 8, 1, UART_PARITY_NONE);
}

static void init_uart_irq(void) {
    uart_set_fifo_enabled(HC06_UART_ID, false);
    int irq = HC06_UART_ID == uart0 ? UART0_IRQ : UART1_IRQ;
    irq_set_exclusive_handler(irq, uart_rx_handler);
    irq_set_enabled(irq, true);
    uart_set_irq_enables(HC06_UART_ID, true, false);
}

// ---------------------------------------------------------------------------
// ISR GPIO — botões com borda de descida
// ---------------------------------------------------------------------------
static void gpio_btn_isr(uint gpio, uint32_t events) {
    uint8_t btn = (uint8_t)gpio;
    xQueueSendFromISR(xQueueBTN, &btn, NULL);
}

// ---------------------------------------------------------------------------
// Helper: enfileira pacote 4 bytes em xQueueTX
// ---------------------------------------------------------------------------
static void bt_send(uint8_t type, int16_t value) {
    uint8_t pkt[4] = {
        0xFF,
        type,
        (uint8_t)(value & 0xFF),
        (uint8_t)((value >> 8) & 0xFF)
    };
#if USE_BLUETOOTH
    for (int i = 0; i < 4; i++)
        xQueueSend(xQueueTX, &pkt[i], 0);
#else
    for (int i = 0; i < 4; i++)
        putchar_raw(pkt[i]);
#endif
}

// ---------------------------------------------------------------------------
// Helper PWM
// ---------------------------------------------------------------------------
static void pwm_init_pin(uint gpio) {
    gpio_set_function(gpio, GPIO_FUNC_PWM);
    uint slice = pwm_gpio_to_slice_num(gpio);
    pwm_set_wrap(slice, PWM_WRAP);
    pwm_set_gpio_level(gpio, 0);
    pwm_set_enabled(slice, true);
}

// ---------------------------------------------------------------------------
// task_mpu — lê MPU-6050 via I2C1 a cada 10 ms
// ---------------------------------------------------------------------------
static void task_mpu(void *p) {
#if DEBUG_MPU
    // Teste de nível das linhas ANTES de ligar o I2C: com pull-up interno,
    // uma linha saudável deve ler 1 (HIGH). Se ler 0, está presa em GND.
    gpio_init(MPU_SDA_PIN); gpio_set_dir(MPU_SDA_PIN, GPIO_IN); gpio_pull_up(MPU_SDA_PIN);
    gpio_init(MPU_SCL_PIN); gpio_set_dir(MPU_SCL_PIN, GPIO_IN); gpio_pull_up(MPU_SCL_PIN);
    sleep_ms(5);
    printf("Nivel das linhas (1=ok, 0=presa em GND):  SDA(GP%d)=%d  SCL(GP%d)=%d\r\n",
           MPU_SDA_PIN, gpio_get(MPU_SDA_PIN), MPU_SCL_PIN, gpio_get(MPU_SCL_PIN));
#endif

    i2c_init(i2c1, 400 * 1000);
    gpio_set_function(MPU_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(MPU_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(MPU_SDA_PIN);
    gpio_pull_up(MPU_SCL_PIN);

#if DEBUG_MPU
    // Varre o barramento I2C1 (SDA=GP2, SCL=GP3) para ver quem responde.
    printf("\r\n--- Scan I2C1 (SDA=%d SCL=%d) ---\r\n", MPU_SDA_PIN, MPU_SCL_PIN);
    int achados = 0, timeouts = 0;
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        uint8_t dummy;
        int rc = i2c_read_timeout_us(i2c1, addr, &dummy, 1, false, 2000);
        if (rc >= 0) {
            printf("  dispositivo no endereco 0x%02X\r\n", addr);
            achados++;
        } else if (rc == PICO_ERROR_TIMEOUT) {
            timeouts++;
        }
    }
    printf("--- fim do scan: %d dispositivo(s), %d timeouts ---\r\n", achados, timeouts);
    if (timeouts > 50)
        printf("!!! Barramento preso (SCL/SDA em nivel baixo): cheque fios/curto/GND\r\n");
#endif

    // Wake up MPU-6050
    uint8_t buf[] = {0x6B, 0x00};
    i2c_write_timeout_us(i2c1, MPU6050_I2C_DEFAULT, buf, 2, false, 2000);

    mpu_data_t data;
    for (;;) {
        uint8_t buffer[14];
        uint8_t reg = 0x3B;
        int w = i2c_write_timeout_us(i2c1, MPU6050_I2C_DEFAULT, &reg, 1, true, 2000);
        int r = (w >= 0) ? i2c_read_timeout_us(i2c1, MPU6050_I2C_DEFAULT, buffer, 14, false, 2000) : -1;
        if (w >= 0 && r >= 0) {
            for (int i = 0; i < 3; i++) {
                data.accel[i] = (int16_t)((buffer[i * 2] << 8) | buffer[i * 2 + 1]);
                data.gyro[i]  = (int16_t)((buffer[8 + i * 2] << 8) | buffer[8 + i * 2 + 1]);
            }
#if DEBUG_MPU
            printf("MPU OK  ACC[%6d %6d %6d]  GYR[%6d %6d %6d]\r\n",
                   data.accel[0], data.accel[1], data.accel[2],
                   data.gyro[0],  data.gyro[1],  data.gyro[2]);
#endif
            xQueueSend(xQueueMPU, &data, 0);
            xQueueSend(xQueueGesture, &data, 0);  // alimenta a inferência de gesto
        }
#if DEBUG_MPU
        else {
            printf("MPU I2C FALHOU  (write=%d read=%d)\r\n", w, r);
        }
#endif
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// ---------------------------------------------------------------------------
// task_fusion — AHRS, mira e detecção de chacoalhar
// ---------------------------------------------------------------------------
#define GYRO_SENSITIVITY  0.8f   // pixels por grau/s — ajuste conforme preferência
#define GYRO_DEAD_ZONE    1.5f   // graus/s — abaixo disso ignora (elimina drift)
#define CALIB_SAMPLES     100    // amostras para calibrar offset do giroscópio

// Mede o offset (bias) do giroscópio com o sensor parado (~1 s @ 100 amostras)
static void calibrate_gyro(float *bias_x, float *bias_y) {
    mpu_data_t d;
    float sx = 0.0f, sy = 0.0f;
    for (int i = 0; i < CALIB_SAMPLES; i++) {
        xQueueReceive(xQueueMPU, &d, portMAX_DELAY);
        sx += d.gyro[0] / 131.0f;
        sy += d.gyro[2] / 131.0f;
    }
    *bias_x = sx / CALIB_SAMPLES;
    *bias_y = sy / CALIB_SAMPLES;
}

static void task_fusion(void *p) {
    mpu_data_t data;
    float bias_x = 0.0f, bias_y = 0.0f;
    calibrate_gyro(&bias_x, &bias_y);

    for (;;) {
        // Pedido de recalibração (botão de calibração) — refaz o offset
        if (ulTaskNotifyTake(pdTRUE, 0) > 0)
            calibrate_gyro(&bias_x, &bias_y);

        if (xQueueReceive(xQueueMPU, &data, portMAX_DELAY) != pdTRUE)
            continue;

        float gx = -(data.gyro[0] / 131.0f - bias_x);  // negado: corrige inversão esq/dir
        float gy =   data.gyro[2] / 131.0f - bias_y;   // eixo Z: cima/baixo com sensor de lado

#if DEBUG_MPU
        // gx/gy já calibrados (°/s) — os valores que viram movimento do mouse.
        // Não envia o protocolo binário para não misturar texto com bytes crus.
        printf("FUSION  gx=%7.2f gy=%7.2f\r\n", gx, gy);
        continue;
#endif

        // Movimento de mira — velocidade angular direta, sem acúmulo de ângulo
        if (gx > GYRO_DEAD_ZONE || gx < -GYRO_DEAD_ZONE) {
            int16_t vx = (int16_t)(gx * GYRO_SENSITIVITY);
            bt_send(0x00, vx);
        }
        if (gy > GYRO_DEAD_ZONE || gy < -GYRO_DEAD_ZONE) {
            int16_t vy = (int16_t)(gy * GYRO_SENSITIVITY);
            bt_send(0x01, vy);
        }

        // O gesto de chacoalhar é detectado pela task_gesture (Edge Impulse)
    }
}

// ---------------------------------------------------------------------------
// task_gesture — Edge Impulse: classifica janela da MPU e envia 0x13
// ---------------------------------------------------------------------------
static void task_gesture(void *p) {
    static float features[EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE];
    mpu_data_t d;
    TickType_t last_fire = 0;

    for (;;) {
        // Coleta uma janela completa (não sobreposta) da fila de amostras
        for (size_t ix = 0; ix < EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE;
             ix += EI_CLASSIFIER_RAW_SAMPLES_PER_FRAME) {
            xQueueReceive(xQueueGesture, &d, portMAX_DELAY);
            features[ix + 0] = d.accel[0];
            features[ix + 1] = d.accel[1];
            features[ix + 2] = d.accel[2];
#if EI_CLASSIFIER_RAW_SAMPLES_PER_FRAME == 6   // modelo com accel+gyro
            features[ix + 3] = d.gyro[0];
            features[ix + 4] = d.gyro[1];
            features[ix + 5] = d.gyro[2];
#endif
        }

        signal_t signal;
        if (numpy::signal_from_buffer(features, EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE, &signal) != 0)
            continue;

        ei_impulse_result_t result = {0};
        if (run_classifier(&signal, &result, false) != EI_IMPULSE_OK)
            continue;

        size_t best = 0;
        for (size_t ix = 1; ix < EI_CLASSIFIER_LABEL_COUNT; ix++)
            if (result.classification[ix].value > result.classification[best].value)
                best = ix;

#if DEBUG_MPU
        ei_printf(">>> %s (%.2f)\n",
                  result.classification[best].label,
                  result.classification[best].value);
#endif

        TickType_t now = xTaskGetTickCount();
        if (strcmp(result.classification[best].label, GESTURE_LABEL) == 0 &&
            result.classification[best].value >= GESTURE_THRESHOLD &&
            (now - last_fire) >= pdMS_TO_TICKS(GESTURE_COOLDOWN_MS)) {
            bt_send(0x13, 0x0001);              // recarga via gesto
            last_fire = now;
        }

        xQueueReset(xQueueGesture);             // descarta amostras acumuladas na inferência
    }
}

// ---------------------------------------------------------------------------
// task_btn — debounce e envio de eventos de botão
// ---------------------------------------------------------------------------
static void task_btn(void *p) {
    gpio_init(BTN_PIN_R); gpio_set_dir(BTN_PIN_R, GPIO_IN); gpio_pull_up(BTN_PIN_R);
    gpio_init(BTN_PIN_G); gpio_set_dir(BTN_PIN_G, GPIO_IN); gpio_pull_up(BTN_PIN_G);
    gpio_init(BTN_PIN_B); gpio_set_dir(BTN_PIN_B, GPIO_IN); gpio_pull_up(BTN_PIN_B);

    gpio_set_irq_enabled_with_callback(BTN_PIN_R, GPIO_IRQ_EDGE_FALL, true, gpio_btn_isr);
    gpio_set_irq_enabled(BTN_PIN_G, GPIO_IRQ_EDGE_FALL, true);
    gpio_set_irq_enabled(BTN_PIN_B, GPIO_IRQ_EDGE_FALL, true);

    uint8_t gpio;
    for (;;) {
        if (xQueueReceive(xQueueBTN, &gpio, portMAX_DELAY) != pdTRUE)
            continue;

        vTaskDelay(pdMS_TO_TICKS(50));  // debounce

        if (!gpio_get(gpio)) {
            if (gpio == BTN_PIN_R) {
                bt_send(0x10, 0x0001);                  // atirar (clique)
                xTaskNotifyGive(xMotorHandle);          // vibra o controle
            } else if (gpio == BTN_PIN_G) {
                bt_send(0x02, 0x0001);                  // centraliza mouse no PC
                xTaskNotifyGive(xFusionHandle);         // recalibra a MPU
            } else if (gpio == BTN_PIN_B) {
                bt_send(0x12, 0x0001);                  // pausar
            }
        }
    }
}

// ---------------------------------------------------------------------------
// task_bt_tx — drena xQueueTX para o HC-06
// ---------------------------------------------------------------------------
static void task_bt_tx(void *p) {
    uint8_t ch;
    for (;;) {
        if (xQueueReceive(xQueueTX, &ch, portMAX_DELAY) == pdTRUE)
            uart_putc_raw(HC06_UART_ID, ch);
    }
}

// ---------------------------------------------------------------------------
// task_motor — pulsa o motor de vibração ao ser notificado (cada tiro)
// ---------------------------------------------------------------------------
#define MOTOR_PULSE_MS 150
static void task_motor(void *p) {
    gpio_init(MOTOR_PIN);
    gpio_set_dir(MOTOR_PIN, GPIO_OUT);
    gpio_put(MOTOR_PIN, 0);

    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        gpio_put(MOTOR_PIN, 1);
        vTaskDelay(pdMS_TO_TICKS(MOTOR_PULSE_MS));
        gpio_put(MOTOR_PIN, 0);
    }
}

// ---------------------------------------------------------------------------
// task_led — fade PWM desconectado, sólido conectado
// ---------------------------------------------------------------------------
static void task_led(void *p) {
    gpio_init(HC06_STATE_PIN);
    gpio_set_dir(HC06_STATE_PIN, GPIO_IN);

    pwm_init_pin(LED_PIN_R);
    pwm_init_pin(LED_PIN_G);

    int level = 0, dir = PWM_FADE_STEP;

    for (;;) {
        bool connected = gpio_get(HC06_STATE_PIN);
        if (connected) {
            pwm_set_gpio_level(LED_PIN_G, PWM_WRAP);
            pwm_set_gpio_level(LED_PIN_R, 0);
        } else {
            pwm_set_gpio_level(LED_PIN_R, level);
            pwm_set_gpio_level(LED_PIN_G, 0);
            level += dir;
            if (level >= PWM_WRAP) { level = PWM_WRAP; dir = -PWM_FADE_STEP; }
            if (level <= 0)        { level = 0;        dir =  PWM_FADE_STEP; }
        }
        vTaskDelay(pdMS_TO_TICKS(PWM_FADE_DELAY_MS));
    }
}

extern "C" void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName) {
    (void)xTask; (void)pcTaskName;
    for (;;);
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(void) {
    stdio_init_all();

#if DEBUG_MPU
    sleep_ms(2000);  // dá tempo de abrir o monitor serial antes do banner
    printf("\r\n=== BOOT pico_emb (DEBUG_MPU=1) — stdio via UART0 GP0/GP1 ===\r\n");
#endif

#if USE_BLUETOOTH
    init_uart_hc06();
    hc06_config("HOTD-CTRL", "1234");
    init_uart_irq();
#endif

    xQueueMPU     = xQueueCreate(10,  sizeof(mpu_data_t));
    xQueueTX      = xQueueCreate(256, sizeof(uint8_t));
    xQueueBTN     = xQueueCreate(8,   sizeof(uint8_t));
    xQueueGesture = xQueueCreate(32,  sizeof(mpu_data_t));

    xTaskCreate(task_mpu,     "MPU",    2048,  NULL, 3, NULL);
    xTaskCreate(task_fusion,  "FUSION", 4096,  NULL, 2, &xFusionHandle);
    xTaskCreate(task_gesture, "GEST",   16384, NULL, 2, NULL);
    xTaskCreate(task_btn,     "BTN",    512,   NULL, 2, NULL);
    xTaskCreate(task_motor,   "MOTOR",  256,   NULL, 2, &xMotorHandle);
#if USE_BLUETOOTH
    xTaskCreate(task_bt_tx,  "BTTX",  512,  NULL, 3, NULL);
#endif
    xTaskCreate(task_led,    "LED",    512,  NULL, 1, NULL);

    vTaskStartScheduler();
    for (;;);
}
