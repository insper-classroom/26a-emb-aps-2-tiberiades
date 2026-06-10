#ifndef PINS_H
#define PINS_H

// Botões do controle
#define BTN_PIN_R  16   // gatilho — atirar (LMB)
#define BTN_PIN_G  21   // calibrar MPU + centralizar mouse
#define BTN_PIN_B  18   // pausar/confirmar (Esc)

// LED RGB (PWM) — feedback visual (apenas R e G são usados)
#define LED_PIN_R  7
#define LED_PIN_G  8

// Motor de vibração — feedback háptico no tiro
#define MOTOR_PIN  9

// MPU-6050 (I2C1)
#define MPU_SDA_PIN  2
#define MPU_SCL_PIN  3

// HC-06 (UART1)
#define HC06_UART_ID    uart1
#define HC06_BAUD_RATE  115200
#define HC06_TX_PIN     4
#define HC06_RX_PIN     5
#define HC06_ENABLE_PIN 6
#define HC06_STATE_PIN  15

#endif // PINS_H
