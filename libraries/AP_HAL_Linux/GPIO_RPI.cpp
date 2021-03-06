#include <AP_HAL.h>

#if CONFIG_HAL_BOARD_SUBTYPE == HAL_BOARD_SUBTYPE_LINUX_NAVIO || CONFIG_HAL_BOARD_SUBTYPE == HAL_BOARD_SUBTYPE_LINUX_RASPILOT

#include "GPIO.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/stat.h>

#define MAX_SIZE_LINE 50

using namespace Linux;

static const AP_HAL::HAL& hal = AP_HAL_BOARD_DRIVER;
LinuxGPIO_RPI::LinuxGPIO_RPI()
{}

int LinuxGPIO_RPI::getRaspberryPiVersion() const
{
    char buffer[MAX_SIZE_LINE];
    const char* hardware_description_entry = "Hardware";
    const char* v1 = "BCM2708";
    const char* v2 = "BCM2709";
    char* flag;
    FILE* fd;

    fd = fopen("/proc/cpuinfo", "r");

    while (fgets(buffer, MAX_SIZE_LINE, fd) != NULL) {
        flag = strstr(buffer, hardware_description_entry);

        if (flag != NULL) {
            if (strstr(buffer, v2) != NULL) {
                printf("Raspberry Pi 2 with BCM2709!\n");
                fclose(fd);
                return 2;
            } else if (strstr(buffer, v1) != NULL) {
                printf("Raspberry Pi 1 with BCM2708!\n");
                fclose(fd);
                return 1;
            }
        }
    }

    /* defaults to 1 */
    fprintf(stderr, "Could not detect RPi version, defaulting to 1\n");
    fclose(fd);
    return 1;
}

void LinuxGPIO_RPI::init()
{
    int rpi_version = getRaspberryPiVersion();
    uint32_t gpio_address = rpi_version == 1? GPIO_BASE(BCM2708_PERI_BASE): GPIO_BASE(BCM2709_PERI_BASE);
    uint32_t pwm_address  = rpi_version == 1? PWM_BASE(BCM2708_PERI_BASE): PWM_BASE(BCM2709_PERI_BASE);
    uint32_t clk_address  = rpi_version == 1? CLOCK_BASE(BCM2708_PERI_BASE): CLOCK_BASE(BCM2709_PERI_BASE);
    // open /dev/mem
    if ((mem_fd = open("/dev/mem", O_RDWR|O_SYNC) ) < 0) {
        hal.scheduler->panic("Can't open /dev/mem");
    }

    // mmap GPIO
    gpio_map = mmap(
        NULL,                 // Any adddress in our space will do
        BLOCK_SIZE,           // Map length
        PROT_READ|PROT_WRITE, // Enable reading & writting to mapped memory
        MAP_SHARED,           // Shared with other processes
        mem_fd,               // File to map
        gpio_address    // Offset to GPIO peripheral
    );

    pwm_map = mmap(
        NULL,                 // Any adddress in our space will do
        BLOCK_SIZE,           // Map length
        PROT_READ|PROT_WRITE, // Enable reading & writting to mapped memory
        MAP_SHARED,           // Shared with other processes
        mem_fd,               // File to map
        pwm_address    // Offset to GPIO peripheral
    );

    clk_map = mmap(
        NULL,                 // Any adddress in our space will do
        BLOCK_SIZE,           // Map length
        PROT_READ|PROT_WRITE, // Enable reading & writting to mapped memory
        MAP_SHARED,           // Shared with other processes
        mem_fd,               // File to map
        clk_address    // Offset to GPIO peripheral
    );

    close(mem_fd); // No need to keep mem_fd open after mmap

    if (gpio_map == MAP_FAILED || pwm_map == MAP_FAILED || clk_map == MAP_FAILED) {
        hal.scheduler->panic("Can't open /dev/mem");
    }

    gpio = (volatile uint32_t *)gpio_map; // Always use volatile pointer!
    pwm  = (volatile uint32_t *)pwm_map;
    clk  = (volatile uint32_t *)clk_map;
}

void LinuxGPIO_RPI::pinMode(uint8_t pin, uint8_t output)
{
    if (output == HAL_GPIO_INPUT) {
        GPIO_MODE_IN(pin);
    } else {
        GPIO_MODE_IN(pin);
        GPIO_MODE_OUT(pin);
    }
}

void LinuxGPIO_RPI::pinMode(uint8_t pin, uint8_t output, uint8_t alt)
{
    if (output == HAL_GPIO_INPUT) {
        GPIO_MODE_IN(pin);
    } else if (output == HAL_GPIO_ALT) {
        GPIO_MODE_IN(pin);
        GPIO_MODE_ALT(pin, alt);
    } else {
        GPIO_MODE_IN(pin);
        GPIO_MODE_OUT(pin);
    }
}

int8_t LinuxGPIO_RPI::analogPinToDigitalPin(uint8_t pin)
{
    return -1;
}

uint8_t LinuxGPIO_RPI::read(uint8_t pin)
{
    uint32_t value = GPIO_GET(pin);
    return value ? 1: 0;
}

void LinuxGPIO_RPI::write(uint8_t pin, uint8_t value)
{
    if (value == LOW) {
        GPIO_SET_LOW = 1 << pin;
    } else {
        GPIO_SET_HIGH = 1 << pin;
    }
}

void LinuxGPIO_RPI::toggle(uint8_t pin)
{
    write(pin, !read(pin));
}

void LinuxGPIO_RPI::setPWMPeriod(uint8_t pin, uint32_t time_us)
{
    setPWM0Period(time_us);
}

void LinuxGPIO_RPI::setPWMDuty(uint8_t pin, uint8_t percent)
{
    setPWM0Duty(percent);
}

void LinuxGPIO_RPI::setPWM0Period(uint32_t time_us)
{
    // stop clock and waiting for busy flag doesn't work, so kill clock
    *(clk + PWMCLK_CNTL) = 0x5A000000 | (1 << 5);
    usleep(10);

    // set frequency
    // DIVI is the integer part of the divisor
    // the fractional part (DIVF) drops clock cycles to get the output frequency, bad for servo motors
    // 320 bits for one cycle of 20 milliseconds = 62.5 us per bit = 16 kHz
    int idiv = (int) (19200000.0f / (320000000.0f / time_us));
    if (idiv < 1 || idiv > 0x1000) {
      hal.scheduler->panic("idiv out of range.");
      return;
    }
    *(clk + PWMCLK_DIV)  = 0x5A000000 | (idiv<<12);

    // source=osc and enable clock
    *(clk + PWMCLK_CNTL) = 0x5A000011;

    // disable PWM
    *(pwm + PWM_CTL) = 0;

    // needs some time until the PWM module gets disabled, without the delay the PWM module crashs
    usleep(10);

    // filled with 0 for 20 milliseconds = 320 bits
    *(pwm + PWM_RNG1) = 320;

    // init with 0%
    setPWM0Duty(0);

    // start PWM1 in serializer mode
    *(pwm + PWM_CTL) = 3;
}

void LinuxGPIO_RPI::setPWM0Duty(uint8_t percent)
{
    int bitCount;
    unsigned int bits = 0;

    bitCount = 320 * percent / 100;
    if (bitCount > 320) bitCount = 320;
    if (bitCount < 0) bitCount = 0;
    bits = 0;
    while (bitCount) {
      bits <<= 1;
      bits |= 1;
      bitCount--;
    }
    *(pwm + PWM_DAT1) = bits;
}

/* Alternative interface: */
AP_HAL::DigitalSource* LinuxGPIO_RPI::channel(uint16_t n) {
    return new LinuxDigitalSource(n);
}

/* Interrupt interface: */
bool LinuxGPIO_RPI::attach_interrupt(uint8_t interrupt_num, AP_HAL::Proc p, uint8_t mode)
{
    return true;
}

bool LinuxGPIO_RPI::usb_connected(void)
{
    return false;
}

#endif // CONFIG_HAL_BOARD_SUBTYPE == HAL_BOARD_SUBTYPE_LINUX_NAVIO || CONFIG_HAL_BOARD_SUBTYPE == HAL_BOARD_SUBTYPE_LINUX_RASPILOT
