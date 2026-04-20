#include <stdint.h>

#define LED_REG (*(volatile uint32_t *)0x03000000u)

static void delay(volatile uint32_t count)
{
    while (count--) {
        __asm__ volatile ("nop");
    }
}

int main(void)
{
    uint32_t led = 0x01u;

    while (1) {
        LED_REG = led;
        delay(1500000u);

        led <<= 1;
        if (led == 0u || led > 0x80u)
            led = 0x01u;
    }
}
