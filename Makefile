#
# SDCC Makefile for mcs51
# ------------------------------------------------------
# PATH
INC_DIR  = -I./inc  -I./src

INCDIR  = ./src
SRCDIR  = ./src
OBJDIR  = ./obj
HEXDIR  = ./hex
LIBDIR  = ./lib
LIB_HW  = ./src
TOOLDIR = ./tools

# ------------------------------------------------------
# Target and Source

TARGET = $(HEXDIR)/out

C_SRC := $(wildcard $(SRCDIR)/*.c $(LIBDIR)/*.c $(LIB_HW)/*.c)
ASM_SRC = $(wildcard $(SRCDIR)/*.asm)

C_SRC_FILE = $(notdir $(C_SRC))
C_OBJ_FILE = $(C_SRC_FILE:%.c=%.c.rel)

ASM_SRC_FILE = $(notdir $(ASM_SRC))
ASM_OBJ_FILE = $(ASM_SRC_FILE:%.asm=%.asm.rel)

OBJ = $(addprefix $(OBJDIR)/, $(C_OBJ_FILE)) $(addprefix $(OBJDIR)/, $(ASM_OBJ_FILE)) 

# ------------------------------------------------------
# Usually SDCC's small memory model is the best choice.  If
# you run out of internal RAM, you will need to declare
# variables as "xdata", or switch to larger model

# Memory Model (small, medium, large, huge)
MODEL  = small

# ------------------------------------------------------
# Memory Layout
# PRG Size = 64K Bytes (Максимум для STC8H8K64U)
CODE_SIZE = --code-size 64000

# INT-MEM Size = 256 Bytes
IRAM_SIZE = --iram-size 256

# Ставим 8192 байта (8 КБ), которые физически есть в кристалле.
# EXT-MEM Size = 8K Bytes
XRAM_SIZE = --xram-size 6000

# ------------------------------------------------------
# SDCC

CC = sdcc
AS = sdas8051

MCU_MODEL = mcs51

AFLAGS =  -l -s 
#CFLAGS = $(INC_DIR) -m$(MCU_MODEL) --model-$(MODEL) --out-fmt-ihx --no-xinit-opt $(DEBUG) --peep-file $(TOOLDIR)/peep.def
CFLAGS = $(INC_DIR) -m$(MCU_MODEL) --model-$(MODEL) --out-fmt-ihx --no-xinit-opt $(DEBUG)
LFLAGS = -m$(MCU_MODEL) --model-$(MODEL) $(CODE_SIZE) $(IRAM_SIZE) $(XRAM_SIZE) --out-fmt-ihx $(DEBUG) 

# ------------------------------------------------------
#S = @

.PHONY: cl bn fl

# ИСПРАВЛЕНО: Теперь команда по умолчанию собирает и .hex, и .bin за один раз
all: $(TARGET).hex bn

	@echo "================================================="
	@echo " СБОРКА ЗАВЕРШЕНА УСПЕШНО! Файлы в $(HEXDIR)/"
	@echo "================================================="
	@echo " СТАТИСТИКА ПАМЯТИ ЧИПА:"
	@if [ -f $(OBJDIR)/out.mem ]; then \
		echo "-----------------------------------------------------------------"; \
		echo " Тип памяти      | Старт  | Конец  | Занято (Байт) | Лимит (Байт)"; \
		echo "-----------------------------------------------------------------"; \
		grep -E "RAM|FLASH|DATA|Stack|Page|Internal" $(OBJDIR)/out.mem | \
		grep -E "0x[0-9a-fA-F]+" | \
		sed -E 's/[[:space:]]+/ /g' | \
		awk '{ \
			if ($$2 ~ /^[0-9]/ || $$2 ~ /^0x/) {name=$$1; s=$$2; e=$$3; u=$$4; l=$$5} \
			else {name=$$1" "$$2; s=$$3; e=$$4; u=$$5; l=$$6} \
			printf " %-15s | %-6s | %-6s | %-13s | %-12s\n", name, s, e, u, l \
		}'; \
		echo "-----------------------------------------------------------------"; \
	else \
		echo " Файл статистики out.mem не найден в $(OBJDIR)/"; \
	fi

$(HEXDIR)/%.hex: $(OBJDIR)/%.ihx
	@mkdir -p $(@D)
	$(S) packihx $^ > $@

$(OBJDIR)/%.ihx: $(OBJ)
	@mkdir -p $(@D)
	$(S) $(CC) -o $@ $(LFLAGS) $^

$(OBJDIR)/%.c.rel: $(LIBDIR)/%.c
	@mkdir -p $(@D)
	$(S) $(CC) -o $@ $(CFLAGS) -c $^

$(OBJDIR)/%.c.rel: $(LIB_HW)/%.c
	@mkdir -p $(@D)
	$(S) $(CC) -o $@ $(CFLAGS) -c $^

$(OBJDIR)/%.c.rel: $(SRCDIR)/%.c
	@mkdir -p $(@D)
	$(S) $(CC) -o $@ $(CFLAGS) -c $^

$(OBJDIR)/%.asm.rel: $(SRCDIR)/%.asm
	@mkdir -p $(@D)
	$(S) $(AS) $(AFLAGS) -o $@ $^ 

# ТОТАЛЬНАЯ ОЧИСТКА: Сносим папки obj и hex целиком вместе со всем скрытым мусором
cl:
	$(S) rm -rf $(OBJDIR)
	$(S) rm -rf $(HEXDIR)


# Модифицировано: Теперь собирается автоматически как часть "all"
bn: 
	makebin -p $(TARGET).hex > $(TARGET).bin

# команда для прошивки одной кнопкой
fl:
	@echo "================================================="
	@echo "Запуск прошивки... Передерните питание STC8H!"
	@echo "================================================="
	./stc8prog -p /dev/ttyUSB0 -e -f $(TARGET).hex

