//
//  VoodooI2CACPIResourcesParser.cpp
//  VoodooI2C
//
//  Created by CoolStar on 8/15/17.
//  Copyright © 2017 CoolStar. All rights reserved.
//

// Demo Standalone Program: https://ghostbin.com/paste/tqt73

#include <IOKit/IOLib.h>
#include <string.h>

#include "VoodooI2CACPIResourcesParser.hpp"
#include "linuxirq.hpp"

VoodooI2CACPIResourcesParser::VoodooI2CACPIResourcesParser() {
    found_gpio_interrupts = false;
    found_i2c = false;
    found_gpio_io = false;

    memset(&i2c_info, 0, sizeof(i2c_info));
    memset(&gpio_interrupts, 0, sizeof(gpio_interrupts));
    memset(&gpio_io, 0, sizeof(gpio_io));
}

void VoodooI2CACPIResourcesParser::parseACPISerialBus(uint8_t const* res, uint32_t offset, uint32_t sz) {
    if (found_i2c)
        return;
    if (offset >= sz || offset + 18 > sz)
        return;
    uint8_t opcode = res[offset];
    if (opcode != 0x8e)
        return;
    
    uint16_t len;
    memcpy(&len, res + offset + 1, sizeof(uint16_t));
    
    uint8_t bustype = res[offset + 5];
    if (bustype != 1)
        return; // Only support I2C. Bus type 2 = SPI, 3 = UART
    
    uint8_t flags = res[offset + 6];
    
    uint16_t tflags;
    memcpy(&tflags, res + offset + 7, sizeof(uint16_t));
    
    /*uint16_t datalen;
    memcpy(&datalen, res + offset + 10, sizeof(uint16_t));*/
    
    if (bustype == 1) {
        found_i2c = true;
        
        i2c_info.resource_consumer = (flags >> 1) & 0x1;
        i2c_info.device_initiated = flags & 0x1;
        i2c_info.address_mode_10Bit = tflags & 0x1;
        
        uint32_t busspeed;
        memcpy(&busspeed, res + offset + 12, sizeof(uint32_t));
        i2c_info.bus_speed = busspeed;
        
        uint16_t address;
        memcpy(&address, res + offset + 16, sizeof(uint16_t));
        i2c_info.address = address;
        
        /*uint8_t i2cLen = len - (12 + datalen - 3);
        
        char *i2cBus = (char *)malloc(i2cLen);
        memcpy(i2cBus, res + offset + 12 + datalen, i2cLen);
        IOLog("I2C Bus: %s\n", i2cBus);*/
    }
}

void VoodooI2CACPIResourcesParser::parseACPIGPIO(uint8_t const* res, uint32_t offset, uint32_t sz) {
    if (found_gpio_interrupts)
        return;
    if (offset >= sz || offset + 18 > sz)
        return;
    
    uint8_t opcode = res[offset];
    if (opcode != 0x8c)
        return;
    
    uint16_t len;
    memcpy(&len, res + offset + 1, sizeof(uint16_t));
    
    uint8_t gpio_type = res[offset + 4];
    uint8_t flags = res[offset + 5];
    
    uint8_t gpio_flags = res[offset + 7];
    
    uint8_t pin_config = res[offset + 9];
    
    uint16_t pin_offset;
    memcpy(&pin_offset, res + offset + 14, sizeof(uint16_t));

    // The pin number is stored at the vendor defined offset, make sure the
    // descriptor is large enough before reading it.
    if (pin_offset < 18 || offset + pin_offset + 2 > sz)
        return;

    /*uint16_t resourceOffset;
    memcpy(&resourceOffset, res + offset + 17, sizeof(uint16_t));
    
    uint16_t vendorOffset;
    memcpy(&vendorOffset, res + offset + 19, sizeof(uint16_t));*/
    
    uint16_t pin_number;
    memcpy(&pin_number, res + offset + pin_offset, sizeof(uint16_t));
    
    if (pin_number == 0xFFFF) // pinNumber 0xFFFF is invalid
        return;
    
    /*char *gpioController = (char *)malloc(vendorOffset - resourceOffset);
    memcpy(gpioController, res + offset + resourceOffset, vendorOffset - resourceOffset);
    IOLog("GPIO Controller: %s\n", gpioController);*/
    
    if (gpio_type == 0) {
        // GPIOInt
        
        found_gpio_interrupts = true;
        
        gpio_interrupts.resource_consumer = flags & 0x1;
        gpio_interrupts.level_interrupt = !(gpio_flags & 0x1);
        
        gpio_interrupts.interrupt_polarity = (gpio_flags >> 1) & 0x3;
        
        gpio_interrupts.shared_interrupt = (gpio_flags >> 3) & 0x1;
        gpio_interrupts.wake_interrupt = (gpio_flags >> 4) & 0x1;
        
        gpio_interrupts.pin_config = pin_config;
        gpio_interrupts.pin_number = pin_number;
        
        int irq = 0;
        if (gpio_interrupts.level_interrupt) {
            switch (gpio_interrupts.interrupt_polarity) {
                case 0:
                    irq = IRQ_TYPE_LEVEL_HIGH;
                    break;
                case 1:
                    irq = IRQ_TYPE_LEVEL_LOW;
                    break;
                default:
                    irq = IRQ_TYPE_LEVEL_LOW;
                    break;
            }
        } else {
            switch (gpio_interrupts.interrupt_polarity) {
                case 0:
                    irq = IRQ_TYPE_EDGE_FALLING;
                    break;
                case 1:
                    irq = IRQ_TYPE_EDGE_RISING;
                    break;
                case 2:
                    irq = IRQ_TYPE_EDGE_BOTH;
                    break;
                default:
                    irq = IRQ_TYPE_EDGE_FALLING;
                    break;
            }
        }
        gpio_interrupts.irq_type = irq;
    }
    
    if (gpio_type == 1) {
        // GPIOIo
        
        found_gpio_io = true;
        
        gpio_io.resource_consumer = flags & 0x1;
        gpio_io.io_restriction = gpio_flags & 0x3;
        gpio_io.sharing = (gpio_flags >> 3) & 0x1;
        
        gpio_io.pin_config = pin_config;
        gpio_io.pin_number = pin_number;
    }
}

void VoodooI2CACPIResourcesParser::parseACPIResources(uint8_t const* res, uint32_t offset, uint32_t sz) {
    // Iterate over the resource template instead of recursing: malformed or
    // hostile tables must not be able to overflow the kernel stack.
    while (offset + 3 <= sz) {
        uint8_t opcode = res[offset];

        uint16_t len;
        memcpy(&len, res + offset + 1, sizeof(uint16_t));

        uint32_t next = offset + (uint32_t)len + 3;

        // A zero/overflowing length would loop forever, stop on malformed data.
        if (next <= offset || next > sz)
            break;

        if (opcode == 0x8c)
            parseACPIGPIO(res, offset, next);
        if (opcode == 0x8e)
            parseACPISerialBus(res, offset, next);

        offset = next;
    }
}
