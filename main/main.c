#include <stdio.h>
#include <string.h>
#include <math.h>

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
#include "Fusion.h"

// ---------------------------------------------------------------------------
// Protocolo Pico → PC (via HC-06)
//   0xFF | TYPE | val_lo | val_hi
//
//   TYPE 0x00 : movimento X do mouse  (int16_t LE)
//   TYPE 0x01 : movimento Y do mouse  (int16_t LE)
//   TYPE 0x10 : botão R (atirar)      val = 0x0001
//   TYPE 0x11 : botão G (recarregar)  val = 0x0001
//   TYPE 0x12 : botão B (pausar)      val = 0x0001
//   TYPE 0x13 : gesto de chacoalhar   val = 0x0001
// ---------------------------------------------------------------------------

#define SAMPLE_PERIOD       0.01f   // 10 ms
#define SHAKE_THRESHOLD     1.8f    // g — pico de aceleração para detectar chacoalhar
#define SHAKE_COOLDOWN_MS   600     // ms mínimo entre dois gestos
#define PWM_WRAP            1000
#define PWM_FADE_STEP       10
#define PWM_FADE_DELAY_MS   10

typedef struct { int16_t accel[3]; int16_t gyro[3]; } mpu_data_t;
typedef struct { int axis; int16_t value; }             pos_t;
typedef struct { uint8_t type; int16_t value; }         bt_event_t;

static QueueHandle_t xQueueMPU;
static QueueHandle_t xQueueTX;
static QueueHandle_t xQueueBTN;

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
    for (int i = 0; i < 4; i++)
        xQueueSend(xQueueTX, &pkt[i], 0);
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
    i2c_init(i2c1, 400 * 1000);
    gpio_set_function(MPU_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(MPU_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(MPU_SDA_PIN);
    gpio_pull_up(MPU_SCL_PIN);

    // Wake up MPU-6050
    uint8_t buf[] = {0x6B, 0x00};
    i2c_write_blocking(i2c1, MPU6050_I2C_DEFAULT, buf, 2, false);

    mpu_data_t data;
    for (;;) {
        uint8_t buffer[14];
        uint8_t reg = 0x3B;
        if (i2c_write_blocking(i2c1, MPU6050_I2C_DEFAULT, &reg, 1, true) >= 0 &&
            i2c_read_blocking(i2c1, MPU6050_I2C_DEFAULT, buffer, 14, false) >= 0) {
            for (int i = 0; i < 3; i++) {
                data.accel[i] = (int16_t)((buffer[i * 2] << 8) | buffer[i * 2 + 1]);
                data.gyro[i]  = (int16_t)((buffer[8 + i * 2] << 8) | buffer[8 + i * 2 + 1]);
            }
            xQueueSend(xQueueMPU, &data, 0);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// ---------------------------------------------------------------------------
// task_fusion — AHRS, mira e detecção de chacoalhar
// ---------------------------------------------------------------------------
static void task_fusion(void *p) {
    FusionAhrs ahrs;
    FusionAhrsInitialise(&ahrs);

    mpu_data_t data;
    TickType_t last_shake = 0;

    for (;;) {
        if (xQueueReceive(xQueueMPU, &data, portMAX_DELAY) != pdTRUE)
            continue;

        FusionVector gyroscope = {{
            data.gyro[0] / 131.0f,
            data.gyro[1] / 131.0f,
            data.gyro[2] / 131.0f,
        }};
        FusionVector accelerometer = {{
            data.accel[0] / 16384.0f,
            data.accel[1] / 16384.0f,
            data.accel[2] / 16384.0f,
        }};

        FusionAhrsUpdateNoMagnetometer(&ahrs, gyroscope, accelerometer, SAMPLE_PERIOD);
        FusionEuler euler = FusionQuaternionToEuler(FusionAhrsGetQuaternion(&ahrs));

        // Movimento de mira: pitch → X, roll → Y
        if (euler.angle.pitch > 5.0f || euler.angle.pitch < -5.0f) {
            int16_t vx = (int16_t)(euler.angle.pitch * 0.4f);
            bt_send(0x00, vx);
        }
        if (euler.angle.roll > 5.0f || euler.angle.roll < -5.0f) {
            int16_t vy = (int16_t)(euler.angle.roll * 0.4f);
            bt_send(0x01, vy);
        }

        // Detecção de chacoalhar — magnitude total acima do threshold
        float ax = accelerometer.axis.x;
        float ay = accelerometer.axis.y;
        float az = accelerometer.axis.z;
        float mag = sqrtf(ax * ax + ay * ay + az * az);

        TickType_t now = xTaskGetTickCount();
        if (mag > SHAKE_THRESHOLD &&
            (now - last_shake) > pdMS_TO_TICKS(SHAKE_COOLDOWN_MS)) {
            last_shake = now;
            bt_send(0x13, 0x0001);
        }
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
            if      (gpio == BTN_PIN_R) bt_send(0x10, 0x0001);
            else if (gpio == BTN_PIN_G) bt_send(0x11, 0x0001);
            else if (gpio == BTN_PIN_B) bt_send(0x12, 0x0001);
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
// task_led — fade PWM desconectado, sólido conectado
// ---------------------------------------------------------------------------
static void task_led(void *p) {
    gpio_init(HC06_STATE_PIN);
    gpio_set_dir(HC06_STATE_PIN, GPIO_IN);

    pwm_init_pin(LED_PIN_R);
    pwm_init_pin(LED_PIN_G);
    pwm_init_pin(LED_PIN_B);

    int level = 0, dir = PWM_FADE_STEP;

    for (;;) {
        bool connected = gpio_get(HC06_STATE_PIN);
        if (connected) {
            pwm_set_gpio_level(LED_PIN_G, PWM_WRAP);
            pwm_set_gpio_level(LED_PIN_R, 0);
            pwm_set_gpio_level(LED_PIN_B, 0);
        } else {
            pwm_set_gpio_level(LED_PIN_R, level);
            pwm_set_gpio_level(LED_PIN_G, 0);
            pwm_set_gpio_level(LED_PIN_B, 0);
            level += dir;
            if (level >= PWM_WRAP) { level = PWM_WRAP; dir = -PWM_FADE_STEP; }
            if (level <= 0)        { level = 0;        dir =  PWM_FADE_STEP; }
        }
        vTaskDelay(pdMS_TO_TICKS(PWM_FADE_DELAY_MS));
    }
}

void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName) {
    (void)xTask; (void)pcTaskName;
    for (;;);
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(void) {
    stdio_init_all();

    init_uart_hc06();
    hc06_config("HOTD-CTRL", "1234");
    init_uart_irq();

    xQueueMPU = xQueueCreate(10,  sizeof(mpu_data_t));
    xQueueTX  = xQueueCreate(256, sizeof(uint8_t));
    xQueueBTN = xQueueCreate(8,   sizeof(uint8_t));

    xTaskCreate(task_mpu,    "MPU",    2048, NULL, 3, NULL);
    xTaskCreate(task_fusion, "FUSION", 4096, NULL, 2, NULL);
    xTaskCreate(task_btn,    "BTN",    512,  NULL, 2, NULL);
    xTaskCreate(task_bt_tx,  "BTTX",  512,  NULL, 3, NULL);
    xTaskCreate(task_led,    "LED",    512,  NULL, 1, NULL);

    vTaskStartScheduler();
    for (;;);
}
