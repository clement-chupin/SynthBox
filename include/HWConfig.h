#define NUM_LEDS 36
#define BRIGHTNESS 50

//multiplexer Pin Config
#define MUX_S0      GPIO_NUM_18
#define MUX_S1      GPIO_NUM_33
#define MUX_S2      GPIO_NUM_35
#define MUX_S3      GPIO_NUM_34
#define MUX_COM     GPIO_NUM_4

//soft power-off system Pin Config
#define PWR_ON_EN   GPIO_NUM_6
#define PWR_SENSE   GPIO_NUM_12

//Keyboard Pin Config
#define KBDSDA      GPIO_NUM_42
#define KBDSCL      GPIO_NUM_41
#define LED_DATA_PIN GPIO_NUM_14

//OLED Pin Config
#define OLEDSDA     GPIO_NUM_38
#define OLEDSCL     GPIO_NUM_39

//Joystick Pin Config
#define JOYY GPIO_NUM_1
#define JOYX GPIO_NUM_2
#define JOYSW GPIO_NUM_40

// I2S Audio (PCM5102A DAC + MAX98357A speaker amp)
#define I2S_MCK  GPIO_NUM_10  // Master clock (was incorrectly called SCK)
#define I2S_BCK  GPIO_NUM_9   // Bit clock
#define I2S_DOUT GPIO_NUM_8   // Data out
#define I2S_LRCK GPIO_NUM_7   // Left/Right clock (word select)
#define I2S_XSMT GPIO_NUM_13  // PCM5102A soft mute (HIGH=unmute)
#define SPK_SD   GPIO_NUM_5   // MAX98357A shutdown (HIGH=enabled)

//SD Card SPI Pin Config
#define SD_MOSI GPIO_NUM_36
#define SD_SCLK GPIO_NUM_15
#define SD_MISO GPIO_NUM_16
#define SD_CS   GPIO_NUM_37
