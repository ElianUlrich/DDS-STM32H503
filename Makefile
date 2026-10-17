# Generador DDS - STM32H503CBT6
#   make cmsis   -> descarga los headers CMSIS oficiales de ST (una vez)
#   make         -> compila build/dds.elf / .hex / .bin
#   make flash   -> graba con STM32_Programmer_CLI (ST-LINK por SWD)
#   make test    -> prueba el núcleo DDS en la PC (genera datos para el PDF)
#   make tabla   -> regenera inc/wave_arb.h (forma de onda arbitraria)

TARGET     = dds
BUILD      = build
CMSIS_DIR ?= cmsis
CMSIS_DEV  = $(CMSIS_DIR)/cmsis-device-h5
CMSIS_CORE = $(CMSIS_DIR)/cmsis-core/CMSIS/Core/Include

CC      = arm-none-eabi-gcc
OBJCOPY = arm-none-eabi-objcopy
SIZE    = arm-none-eabi-size

MCU     = -mcpu=cortex-m33 -mthumb -mfpu=fpv5-sp-d16 -mfloat-abi=hard
DEFS    = -DSTM32H503xx
INCS    = -Iinc -I$(CMSIS_DEV)/Include -I$(CMSIS_CORE)
CFLAGS  = $(MCU) $(DEFS) $(INCS) -O2 -g3 -std=c11 -Wall -Wextra \
          -ffunction-sections -fdata-sections -fsingle-precision-constant
LDSCRIPT = $(CMSIS_DEV)/Source/Templates/gcc/linker/STM32H503xx_FLASH.ld
LDFLAGS = $(MCU) -T$(LDSCRIPT) --specs=nano.specs \
          -Wl,--gc-sections -Wl,-Map=$(BUILD)/$(TARGET).map -Wl,--print-memory-usage

SRCS = src/main.c src/board.c src/dds_core.c src/dds_hw.c src/encoder.c src/ui.c \
       $(CMSIS_DEV)/Source/Templates/system_stm32h5xx.c
ASMS = $(CMSIS_DEV)/Source/Templates/gcc/startup_stm32h503xx.s

OBJS = $(addprefix $(BUILD)/,$(notdir $(SRCS:.c=.o) $(ASMS:.s=.o)))
vpath %.c $(sort $(dir $(SRCS)))
vpath %.s $(sort $(dir $(ASMS)))

all: $(BUILD)/$(TARGET).elf $(BUILD)/$(TARGET).hex $(BUILD)/$(TARGET).bin

$(BUILD):
	mkdir -p $@

$(BUILD)/%.o: %.c inc/*.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: %.s | $(BUILD)
	$(CC) $(MCU) -c $< -o $@

$(BUILD)/$(TARGET).elf: $(OBJS)
	$(CC) $(OBJS) $(LDFLAGS) -lm -o $@
	$(SIZE) $@

$(BUILD)/%.hex: $(BUILD)/%.elf
	$(OBJCOPY) -O ihex $< $@

$(BUILD)/%.bin: $(BUILD)/%.elf
	$(OBJCOPY) -O binary $< $@

flash: $(BUILD)/$(TARGET).elf
	STM32_Programmer_CLI -c port=SWD -w $< -v -rst

cmsis:
	mkdir -p $(CMSIS_DIR)
	git clone --depth 1 https://github.com/STMicroelectronics/cmsis-device-h5.git $(CMSIS_DEV)
	git clone --depth 1 https://github.com/STMicroelectronics/cmsis-core.git $(CMSIS_DIR)/cmsis-core

test:
	mkdir -p $(BUILD)
	gcc -O2 -Wall -Wextra -Iinc test/test_host.c src/dds_core.c -lm -o $(BUILD)/test_host
	./$(BUILD)/test_host doc

tabla:
	python3 tools/gen_tabla.py

clean:
	rm -rf $(BUILD)

.PHONY: all flash cmsis test tabla clean
