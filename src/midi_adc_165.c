// ============================================================================
// 02.10.2026  код для sdcc компилятора 
// ============================================================================
// lsusb -v -d 34bf:0001
#include "STC8H.h"

typedef unsigned char  u8;
typedef unsigned int   u16;
typedef unsigned long  u32;

// ============================================================================
#define EPIDLE              0
#define EPSTATUS            1
#define EPDATAIN            2
#define EPDATAOUT           3
#define EPSTALL             0xff

#define GET_STATUS          0x00
#define CLEAR_FEATURE       0x01
#define SET_FEATURE         0x03
#define SET_ADDRESS         0x05
#define GET_DESCRIPTOR      0x06
#define SET_DESCRIPTOR      0x07
#define GET_CONFIG          0x08
#define SET_CONFIG          0x09
#define GET_INTERFACE       0x0A
#define SET_INTERFACE       0x0B
#define SYNCH_FRAME         0x0C

#define GET_REPORT          0x01
#define GET_IDLE            0x02
#define GET_PROTOCOL        0x03
#define SET_REPORT          0x09
#define SET_IDLE            0x0A
#define SET_PROTOCOL        0x0B

#define DESC_DEVICE         0x01
#define DESC_CONFIG         0x02
#define DESC_STRING         0x03
#define DESC_HIDREPORT      0x22

#define STANDARD_REQUEST    0x00
#define CLASS_REQUEST       0x20
#define VENDOR_REQUEST      0x40
#define REQUEST_MASK        0x60
// ============================================================================
#define NUM_POTENSIOMETERS  16
#define ADC_8BIT_THRESHOLD  1   // Порог фильтра на уровне 8 бит (0-255)





// --- Настройки клавиатуры 74HC165 ---
#define NUM_KEYS 32
// Базовая нота для первой клавиши (например, 60 = До 4-й октавы Middle C)
#define BASE_NOTE 60 

// Переменная для хранения текущего и предыдущего состояния 32 кнопок (0 - отпущена, 1 - нажата)

u32  __data current_keys_state ;
u32  __data last_keys_state   ;

u8 Velocity = 0;

int __data octave_offset = 0;
u8 __data btn_down_timer = 0;
u8 __data btn_up_timer = 0;

__idata u8 tx_buffer[64]; //  буфер под EP1

// Быстрые буферы 
u8 __data MidiInput[4];
u8 __data MidiOutput[4];


// Буфер памяти для фильтрации 16 аналоговых ручек
u8 __data last_8bit_values[NUM_POTENSIOMETERS];



// Таблица номеров MIDI CC контроллеров для ручек 1-15 (ручка 0 — это Modulation ручка 1 — это Velocity)
const u8 __code MIDI_CC_TAB1[NUM_POTENSIOMETERS] = {
    0,  1,  10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23
};

// ============================================================================
typedef struct {
    u8 bmRequestType;
    u8 bRequest;
    u8 wValueL;
    u8 wValueH;
    u8 wIndexL;
    u8 wIndexH;
    u8 wLengthL;
    u8 wLengthH;
} SETUP;

typedef struct {
    u8  bStage;
    u16 wResidue;
    u8  *pData;
} EPOSTAGE;

// --- Прототипы функций ---
void UsbInit();
u8   ReadReg(u8 addr);
void WriteReg(u8 addr, u8 dat);
u8   ReadFifo(u8 fifo, u8 *pdat);
void WriteFifo(u8 fifo, u8 *pdat, u8 cnt);
void usb_ctrl_in();
void usb_ctrl_out();
void CONF_SET();
void SendMidiPacket(u8 cable_cin, u8 status, u8 data1, u8 data2);
void delay_ms(u16 ms);
void ProcessMuxPotensiometers();
void AdcInit();
void SetMuxChannel(u8 ch);
u16  GetMuxAdc();

u16 Read165Cascade();
void Process165Keyboard();

// --- Прототипы дескрипторов для SDCC ---
__code u8 DEVICEDESC[18];
__code u8 CONFIGDESC[86];
__code u8 LANGIDDESC[4];
__code u8 MANUFACTDESC[8];
__code u8 PRODUCTDESC[30];

// --- Глобальные переменные ---
SETUP Setup;
EPOSTAGE Ep0Stage;

// ============================================================================
// ОСНОВНОЙ ЦИКЛ MAIN
// ============================================================================
void main()
{
    u8 i;

    // Обнуляем буферы
    for(i = 0; i < NUM_POTENSIOMETERS; i++) {
        last_8bit_values[i] = 0;
    }
    current_keys_state = 0x00000000;
    last_keys_state = 0x00000000;

    // Настройка остальных портов
    P0M0 = 0x00; P0M1 = 0x00;
    P2M0 = 0x00; P2M1 = 0x00;
    P4M0 = 0x00; P4M1 = 0x00;
    P5M0 = 0x00; P5M1 = 0x00;

    // Настраиваем P1.1 как вход с подтяжкой data74hc165 (Quasi)
    P1M0 &= ~0x02; P1M1 &= ~0x02; 
    
    AdcInit(); // Инициализация ацп
    UsbInit(); 
    IE2 = 0x80;
    EA = 1;

    while (1)
    {
        Process165Keyboard();       // 1. Опрашиваем кнопки через ассемблер (P3.6, P3.7, P1.1)
        ProcessMuxPotensiometers(); // 2. Опрашиваем ручки АЦП через мультиплексор (P3.4-P3.7, P1.3)

    }
}

// ============================================================================
u32 Read165Cascade32() __naked 
{
    __asm
        // 1. АППАРАТНОЕ ВОССТАНОВЛЕНИЕ ШИНЫ ПОСЛЕ АЦП
        SETB _P36           // Восстанавливаем PL в 1
        CLR _P37           // Восстанавливаем CP в 0
        NOP                 

        // 2. ЗАЩЁЛКИВАЕМ ДАННЫЕ В 74HC165
        CLR _P36            // PL = 0 (защелкнули все 4 74HC165)
        NOP
        SETB _P36           // PL = 1 

        // ====================================================================
        // СБОР ДАННЫХ ПО ВЫТАЛКИВАЮЩЕМУ АЛГОРИТМУ
        // ====================================================================
        CLR A               // Очищаем Аккумулятор на старте всей функции
        MOV C, _P11         // Читаем самый первый бит (Q7) без такта CP
        RLC A               // Закатываем в Аккумулятор

        MOV R7, #7          // Первый чип: дочитываем оставшиеся 7 бит
        LCALL _shift_loop1  
        MOV R3, A           // Сохраняем Байт 1 во временный R3

        LCALL _shift_loop2  // Читаем Байт 2 (все 8 бит выталкиванием)
        MOV R4, A           // Сохраняем Байт 2 во временный R4

        LCALL _shift_loop2  // Читаем Байт 3 (8 бит)
        MOV R5, A           // Сохраняем Байт 3 во временный R5

        LCALL _shift_loop2  // Читаем Байт 4 (8 бит)
        MOV R6, A           // Сохраняем Байт 4 во временный R6

        // ====================================================================
        // ПЕРЕДАЕМ РЕЗУЛЬТАТ В СИСТЕМНЫЕ РЕГИСТРЫ SDCC (R0..R3 для u32)
        // ====================================================================
        MOV DPL, R3          // Младший байт (0-7)
        MOV DPH, R4          // Второй байт (8-15)
        MOV B, R5          // Третий байт (16-23)
        MOV A, R6          // Старший байт (24-31)
        
        RET   // Уходим на выход, минуя подпрограммы!

        // ====================================================================
        // ОПТИМИЗИРОВАННЫЕ ПОДПРОГРАММЫ БЕЗ ЛИШНИХ ОЧИСТОК
        // ====================================================================
    _read_bits_sub:
    _shift_loop2:
        MOV R7, #8          // Вход для 2, 3 и 4 74HC165: выталкиваем честные 8 бит
        
    _shift_loop1:           // Вход для 1-го 74HC165 (R7 равен 7, первый бит уже в А)
        SETB _P37           // Даем фронт такта CP
        NOP                 
        CLR _P37            // Даем спад такта CP
        MOV C, _P11         // Читаем стабильный бит данных
        RLC A               // Вкатываем в Аккумулятор (выталкивая старый бит наружу)
        DJNZ R7, _shift_loop1 // Повторяем цикл по регистру R7
        
        CPL A               // Инвертируем байт (для 1, 2, 3 и 4 чипов)
        RET                 // Возврат по LCALL

    __endasm;
}


// ============================================================================
// Переменные для хранения смещения и фильтрации дребезга (в самый верх файла)

void Process165Keyboard()
{
    u8 k;
    u32 mask;
    u8 tx_cnt = 0; 

    // Принудительно настраиваем P3.2 и P3.3 как входы с подтяжкой (Quasi-bidirectional)
    // Чтобы в тишине на них железно висела логическая "1"
        //P3M0 &= ~0x0C; P3M1 &= ~0x0C;
    // 3. Настройка P1.5 под Светодиод octave_offset (Push-Pull выход)
    P1M0 |= 0x20; P1M1 &= ~0x20; 

    // 1. Вычитываем карту кнопок  (все 32 клавиши!)
    current_keys_state = Read165Cascade32();

    // 2. СКАНИРУЕМ ВСЕ 32 МУЗЫКАЛЬНЫЕ КЛАВИШИ (Цикл идет честно до 32)
    for (k = 0; k < NUM_KEYS; k++)
    {
        if (tx_cnt >= 60) break; // Защита от переполнения FIFO буфера

        mask = (1UL << k);

        if ((current_keys_state & mask) != (last_keys_state & mask))
        {
            if (current_keys_state & mask)
            {
                // КЛАВИША НАЖАТА -> Применяем текущий octave_offset
                tx_buffer[tx_cnt++] = 0x09;             // CIN: Note On
                tx_buffer[tx_cnt++] = 0x90;             // Статус: Note On, Канал 1
                tx_buffer[tx_cnt++] = BASE_NOTE + k + octave_offset; 
                tx_buffer[tx_cnt++] = Velocity;         // Громкость из АЦП ручки 2
            }
            else
            {
                // КЛАВИША ОТПУЩЕНА -> Отпускаем строго ту же самую ноту
                tx_buffer[tx_cnt++] = 0x08;             // CIN: Note Off
                tx_buffer[tx_cnt++] = 0x80;             // Статус: Note Off, Канал 1
                tx_buffer[tx_cnt++] = BASE_NOTE + k + octave_offset; 
                tx_buffer[tx_cnt++] = 0;                
            }
        }
    }

    // 3. ОТПРАВЛЯЕМ ПАКЕТ  В USB (если были изменения клавиш)
    if (tx_cnt > 0)
    {
        if (tx_cnt > 64) tx_cnt = 64; // защита на лимит 64 байт
        
        WriteReg(INDEX, 1); 
        while (ReadReg(INCSR1) & INIPRDY) { __asm NOP __endasm; }
        
        WriteFifo(FIFO1, tx_buffer, tx_cnt); 
        WriteReg(INCSR1, INIPRDY);
       
    }
    last_keys_state = current_keys_state;

    // ====================================================================
    // 4. ОПРОС ВЫДЕЛЕННЫХ ПИНОВ ОК ТАВ (P3.2 и P3.3) С ФИЛЬТРОМ ДРЕБЕЗГА
    // ====================================================================
    
    // Кнопка Октава Вниз на пине P3.2 (нажата при замыкании на GND -> логический 0)
    if (P32 == 0)
    {
  
            if (BASE_NOTE + octave_offset >= 24) // Ограничение: не уходим ниже субконтроктавы
            {
                octave_offset -= 12;

                P15 = 0; delay_ms(200); P15 = 1; //  светодиод знак смены октавы
            }
        
    }

    // Кнопка Октава Вверх на пине P3.3
    if (P33 == 0)
    {

            if (BASE_NOTE + octave_offset <= 72) // Ограничение: не уходим выше 5-й октавы
            {
                octave_offset += 12;
                P15 = 0; delay_ms(200); P15 = 1; //  светодиод знак смены октавы
            }
   }
}

// ============================================================================
// ИНИЦИАЛИЗАЦИЯ И НАСТРОЙКА АЦП
// ============================================================================
void AdcInit()
{
   
  
    P1M0 &= ~0x01; P1M1 |= 0x01;  // 1. Настраиваем  (P1.0) как вход АЦП (High-Impedance режим)
    
    // 2. Настраиваем адресные пины S0-S3 (P3.4, P3.5, P3.6, P3.7) как выходы (Push-Pull)
    P3M0 |= 0xF0; P3M1 &= ~0xF0;// 2. Настраивае S0-S3 (P3.4, P3.5, P3.6, P3.7) как выходы (Push-Pull)

    // 3. Настройка P1.4 под Светодиод центра Pitch Bend (Push-Pull выход)
    P1M0 |= 0x10; P1M1 &= ~0x10; 
    P14 = 1; // Выключаем светодиод нулем при старте

    P1M0 |= 0x20; P1M1 &= ~0x20; 
   


    // 4. Включаем модуль АЦП контроллера (Питание АЦП)
    ADC_CONTR = 0x80; 
    ADCCFG = 0x2F; 


P_SW2 |= 0x80;
ADCTIM = 0x3f;     // Set ADC internal timing
P_SW2 &=~0x80;


}
// ============================================================================
void SetMuxChannel(u8 ch)
{
    //u8 delay_mux;
    // Выставляем 4 бита адреса канала в верхнюю тетраду P3.4 - P3.7
    P3 = (P3 & 0x0F) | ((ch & 0x0F) << 4);
}

// ============================================================================
u16 GetMuxAdc()
{
    u8 i;
    u32 adc_sum = 0; // Используем u32, чтобы сумма 16 замеров гарантированно не переполнилась

    // 1. ФИКЦИВНОЕ (DUMMY) ПРЕОБРАЗОВАНИЕ 
    // Сбрасываем остаточный заряд емкости АЦП от предыдущего канала мультиплексора
    ADC_CONTR = 0x80 | 0x40 ; // Канал АЦП 3 (P1.0), старт замера
    while (!(ADC_CONTR & 0x20)); 
    ADC_CONTR &= ~0x20; 


    // 2. ОВЕРСЭМПЛИНГ (Накапливаем 16 замеров)
    for (i = 0; i < 16; i++)
    {

        ADC_CONTR = 0x80 | 0x40 ; // Канал АЦП 3 (P1.0), старт замера
        while (!(ADC_CONTR & 0x20)); 
        ADC_CONTR &= ~0x20;


        
        adc_sum += (u16)((ADC_RES << 8) | ADC_RESL );
    }
    
    // 3. МАТЕМАТИЧЕСКОЕ УСРЕДНЕНИЕ (Делим сумму на 16)
    // Сдвиг на 4 бита вправо (>> 4) — это быстрое деление на 16 на уровне процессора
    return (u16)(adc_sum >> 4); 
}

// ============================================================================
void ProcessMuxPotensiometers()
{
    u8 pot;
    u16 raw_val;
    u8 current_8bit_val;
    u8 final_midi_7bit;
    u8 tx_cnt = 0; // Счетчик записанных байт в буфер
    
    for (pot = 0; pot < NUM_POTENSIOMETERS; pot++)
    {
        SetMuxChannel(pot);   
        raw_val = GetMuxAdc(); 
        
        current_8bit_val = (u8)(raw_val >> 4); 
        
       
        if ((current_8bit_val != last_8bit_values[pot]) && 
            (current_8bit_val > last_8bit_values[pot] + ADC_8BIT_THRESHOLD || 
             current_8bit_val < last_8bit_values[pot] - ADC_8BIT_THRESHOLD || 
             current_8bit_val == 0 || current_8bit_val == 255)) 
        {
            last_8bit_values[pot] = current_8bit_val; 
            
            final_midi_7bit = (u8)(current_8bit_val >> 1);

            if (pot == 1) {Velocity = final_midi_7bit; break;}
            
            if (pot == 0)
            {
                if (final_midi_7bit >= 62 && final_midi_7bit <= 66)
                {
                    final_midi_7bit = 64; 
                    P14 = 0;              
                }
                else
                {
                    P14 = 1;              
                }                                                                        
                tx_buffer[tx_cnt++] = 0x0E;             
                tx_buffer[tx_cnt++] = 0xE0;             
                tx_buffer[tx_cnt++] = 0x00;   
                tx_buffer[tx_cnt++] = final_midi_7bit;  


           

            }
            else
            {             
                tx_buffer[tx_cnt++] = 0x0B;             
                tx_buffer[tx_cnt++] = 0xB0;             
                tx_buffer[tx_cnt++] = MIDI_CC_TAB1[pot];   
                tx_buffer[tx_cnt++] = final_midi_7bit;              
            }
        }
    }
       // 3. ОТПРАВЛЯЕМ ВСЮ ПАЧКУ ЗА ОДИН РАЗ В USB (если события были)
    if (tx_cnt > 0)
    {
        WriteReg(INDEX, 1); 
        while (ReadReg(INCSR1) & INIPRDY) { __asm NOP __endasm; }
        
        WriteFifo(FIFO1, tx_buffer, tx_cnt); 
        WriteReg(INCSR1, INIPRDY);
        
    }

}

// ============================================================================
void delay_ms(u16 ms)
{
    u16 i;
    while(ms--)
    {
        for(i = 0; i < 2000; i++) 
        {
            __asm NOP __endasm;
        }
    }
}
// ============================================================================
u8 ReadReg(u8 addr)
{
    u8 dat;
    while (USBADR & 0x80);
    USBADR = addr | 0x80;
    while (USBADR & 0x80);
    dat = USBDAT;
    return dat;
}
// ============================================================================
void WriteReg(u8 addr, u8 dat)
{
    while (USBADR & 0x80);
    USBADR = addr & 0x7f;
    USBDAT = dat;
}
// ============================================================================
u8 ReadFifo(u8 fifo, u8 *pdat)
{
    u8 cnt, ret;
    ret = cnt = ReadReg(COUNT0);
    while (cnt--) { *pdat++ = ReadReg(fifo); }
    return ret;
}
// ============================================================================
void WriteFifo(u8 fifo, u8 *pdat, u8 cnt)
{
    while (cnt--) { WriteReg(fifo, *pdat++); }
}
// ============================================================================
void UsbInit()
{
    //P3M0 = 0x00;P3M1 = 0x03;
    
    P3M1 |= 0x03; P3M0 &= ~0x03;

    P_SW2 |= 0x80;
    IRC48MCR = 0x80;
    while (!(IRC48MCR & 0x01));
    P_SW2 &= ~0x80;
    USBCLK = 0x00;
    USBCON = 0x90;
    WriteReg(FADDR, 0x00);
    WriteReg(POWER, 0x08);
    WriteReg(INTRIN1E, 0x3f);
    WriteReg(INTROUT1E, 0x3f);
    WriteReg(INTRUSBE, 0x00);
    WriteReg(POWER, 0x01);
    Ep0Stage.bStage = EPIDLE;
}
// ============================================================================
void usb_ctrl_in()
{
    u8 cnt;
    cnt = Ep0Stage.wResidue;
    if (Ep0Stage.wResidue > 64) cnt = 64;
    WriteFifo(FIFO0, Ep0Stage.pData, cnt);
    Ep0Stage.wResidue -= cnt;
    Ep0Stage.pData += cnt;
    if (Ep0Stage.wResidue == 0)
    {
        WriteReg(CSR0, IPRDY | DATEND);
        Ep0Stage.bStage = EPIDLE;
    }
    else
    {
        WriteReg(CSR0, IPRDY);
    }
}
// ============================================================================
void usb_ctrl_out()
{
    u8 cnt;
    cnt = ReadFifo(FIFO0, Ep0Stage.pData);
    Ep0Stage.wResidue -= cnt;
    Ep0Stage.pData += cnt;
    if (Ep0Stage.wResidue == 0)
    {
        WriteReg(CSR0, SOPRDY | DATEND);
        Ep0Stage.bStage = EPIDLE;
    }
    else
    {
        WriteReg(CSR0, SOPRDY);
    }
}
// ============================================================================
void CONF_SET()
{
    WriteReg(INDEX, 1);
    WriteReg(INCSR2, INMODEIN);
    WriteReg(INMAXP, 8);
    WriteReg(INCSR1, INCLRDT | INFLUSH);

    WriteReg(INDEX, 1);
    WriteReg(OUTCSR2, INMODEOUT);
    WriteReg(OUTMAXP, 8);
    WriteReg(OUTCSR1, OUTCLRDT | OUTFLUSH);
    WriteReg(INDEX, 0);
}
// ============================================================================
// ============================================================================
void usb_isr() __interrupt 25
{
    u8 intrusb;
    u8 intrin;
    u8 introut;
    u8 csr;
    u16 len = 0;

    intrusb = ReadReg(INTRUSB);
    intrin  = ReadReg(INTRIN1);
    introut = ReadReg(INTROUT1);

    // 1. Обработка сброса шины USB (Bus Reset)
    if (intrusb & RSTIF)
    {
        WriteReg(INDEX, 1);
        WriteReg(INCSR1, INCLRDT);
        WriteReg(INDEX, 1);
        WriteReg(OUTCSR1, OUTCLRDT);
        Ep0Stage.bStage = EPIDLE;
    }

    // 2. Обработка Конечной Точки 0 (Управление / Перечисление)
    if (intrin & EP0IF) 
    {
        WriteReg(INDEX, 0); 
        csr = ReadReg(CSR0); 

        if (csr & STSTL)
        {
            WriteReg(CSR0, csr & ~STSTL);
            Ep0Stage.bStage = EPIDLE;
        }
        if (csr & SUEND)
        {
            WriteReg(CSR0, csr | SSUEND);
        }

        switch (Ep0Stage.bStage)
        {
            case EPIDLE:
                if (csr & OPRDY)
                {
                    Ep0Stage.bStage = EPSTATUS;
                    ReadFifo(FIFO0, (u8 *)&Setup); 
                    ((u8*)&Ep0Stage.wResidue)[0] = Setup.wLengthL;
                    ((u8*)&Ep0Stage.wResidue)[1] = Setup.wLengthH;

                    switch (Setup.bmRequestType & REQUEST_MASK)
                    {
                        case STANDARD_REQUEST:
                            switch (Setup.bRequest)
                            {
                                case GET_STATUS:
                                    MidiInput[0] = 0x00;
                                    MidiInput[1] = 0x00;
                                    Ep0Stage.pData = (void *)MidiInput;
                                    len = 2;
                                    Ep0Stage.bStage = EPDATAIN;
                                    break;

                                case SET_ADDRESS:
                                    WriteReg(FADDR, Setup.wValueL);
                                    break;

                                case SET_CONFIG:
                                    CONF_SET();
                                    break;

                                case SET_INTERFACE:
                                    WriteReg(INDEX, 0); 
                                    WriteReg(CSR0, SOPRDY | DATEND); // Очищаем OPRDY и закрываем транзакцию
                                    Ep0Stage.bStage = EPIDLE;
                                    break;

                                case GET_DESCRIPTOR:
                                    Ep0Stage.bStage = EPDATAIN;
                                    switch (Setup.wValueH)
                                    {
                                        case DESC_DEVICE:
                                            Ep0Stage.pData = (u8 *)&DEVICEDESC;
                                            len = 18;
                                            break;
                                        case DESC_CONFIG:
                                            Ep0Stage.pData = (u8 *)&CONFIGDESC;
                                            len = 86;
                                            break;
                                        case DESC_STRING:
                                            switch (Setup.wValueL)
                                            {
                                                case 0:
                                                    Ep0Stage.pData = (u8 *)&LANGIDDESC;
                                                    len = 4;
                                                    break;
                                                case 1:
                                                    Ep0Stage.pData = (u8 *)&MANUFACTDESC;
                                                    len = 8;
                                                    break;
                                                case 2:
                                                    Ep0Stage.pData = (u8 *)&PRODUCTDESC;
                                                    len = 30;
                                                    break;
                                                default:
                                                    Ep0Stage.bStage = EPSTALL;
                                                    break;
                                            }
                                            break;
                                        default:
                                            Ep0Stage.bStage = EPSTALL;
                                            break;
                                    }

                                    if (len < Ep0Stage.wResidue)
                                    {
                                        Ep0Stage.wResidue = len;
                                    }
                                    break;

                                default:
                                    Ep0Stage.bStage = EPSTALL;
                                    break;
                            }
                            break;

                        case CLASS_REQUEST:
                            Ep0Stage.bStage = EPSTALL;
                            break;

                        default:
                            Ep0Stage.bStage = EPSTALL;
                            break;
                    }

                    // Аппаратный ответ на Endpoint 0
                    switch (Ep0Stage.bStage)
                    {
                        case EPDATAIN:   WriteReg(CSR0, SOPRDY); usb_ctrl_in(); break;
                        case EPDATAOUT:  WriteReg(CSR0, SOPRDY); break;
                        case EPSTATUS:   WriteReg(CSR0, SOPRDY | DATEND); Ep0Stage.bStage = EPIDLE; break;
                        case EPSTALL:    WriteReg(CSR0, SOPRDY | SDSTL);  Ep0Stage.bStage = EPIDLE; break;
                    }
                }
                break;

            case EPDATAIN:
                if (!(csr & IPRDY)) usb_ctrl_in();
                break;

            case EPDATAOUT:
                if (csr & OPRDY) usb_ctrl_out();
                break;
        }
    }

    // 3. Конечная точка 1 IN (Передача MIDI с МК на компьютер)
    if (intrin & EP1INIF)
    {
        WriteReg(INDEX, 1);
        csr = ReadReg(INCSR1);
        if (csr & INSTSTL) WriteReg(INCSR1, INCLRDT);
        WriteReg(INCSR1, 0); // Чистим статус, прерывание обработано
    }

    // 4. Конечная точка 1 OUT (Прием MIDI из компьютера в МК)
    if (introut & EP1OUTIF)
    {
        WriteReg(INDEX, 1);
        csr = ReadReg(OUTCSR1);
        if (csr & OUTSTSTL) WriteReg(OUTCSR1, OUTCLRDT);
        if (csr & OUTOPRDY)
        {
            ReadFifo(FIFO1, MidiOutput);
            WriteReg(OUTCSR1, 0); // Освобождаем FIFO
        }
        else
        {
            WriteReg(OUTCSR1, 0);
        }
        WriteReg(INDEX, 1);
    }
}
// ============================================================================
// ============================================================================
__code u8 DEVICEDESC [18] =
{
    0x12,       // bLength (18)
    0x01,       // bDescriptorType (Device)
    0x10, 0x01, // bcdUSB (1.10 - вполне достаточно для MIDI 1.0)
    0x00,       // bDeviceClass (Класс определяется в интерфейсах)
    0x00,       // bDeviceSubClass
    0x00,       // bDeviceProtocol
    0x40,       // bMaxPacketSize0 (64)
    0xbf, 0x34, // idVendor(34bf);   stc
    0x01, 0x00, // idProduct 
    0x00, 0x01, // bcdDevice (1.00)
    0x01,       // iManufacturer (1)
    0x02,       // iProduct (2)
    0x00,       // iSerialNumber (0)
    0x01        // bNumConfigurations (1)
};


__code u8 CONFIGDESC[86] = {
    // --- Configuration Descriptor --- (9 байт)
    0x09,       // bLength
    0x02,       // bDescriptorType (Configuration)
    0x56, 0x00, // wTotalLength (86 байт -> 0x0056)
    0x02,       // bNumInterfaces (2 интерфейса: AC и MS)
    0x01,       // bConfigurationValue
    0x00,       // iConfiguration
    0x80,       // bmAttributes (Bus Powered)
    0x32,       // bMaxPower (100mA)

    // --- Standard AC (Audio Control) Interface Descriptor --- (9 байт)
    0x09,       // bLength
    0x04,       // bDescriptorType (Interface)
    0x00,       // bInterfaceNumber (0)
    0x00,       // bAlternateSetting
    0x00,       // bNumEndpoints (0 конечных точек для AC)
    0x01,       // bInterfaceClass (AUDIO)
    0x01,       // bInterfaceSubClass (AUDIO_CONTROL)
    0x00,       // bInterfaceProtocol
    0x00,       // iInterface

    // --- Class-specific AC Interface Descriptor --- (9 байт)
    0x09,       // bLength
    0x24,       // bDescriptorType (CS_INTERFACE)
    0x01,       // bDescriptorSubtype (HEADER)
    0x00, 0x01, // bcdADC (1.00)
    0x09, 0x00, // wTotalLength (9 байт)
    0x01,       // bInCollection (1 аудиоинтерфейс стриминга далее)
    0x01,       // baInterfaceNr(1) (Интерфейс номер 1 - это MS)

    // --- Standard MS (MIDI Streaming) Interface Descriptor --- (9 байт)
    0x09,       // bLength
    0x04,       // bDescriptorType (Interface)
    0x01,       // bInterfaceNumber (1)
    0x00,       // bAlternateSetting
    0x02,       // bNumEndpoints (2 конечные точки: IN и OUT)
    0x01,       // bInterfaceClass (AUDIO)
    0x03,       // bInterfaceSubClass (MIDI_STREAMING)
    0x00,       // bInterfaceProtocol
    0x00,       // iInterface

    // --- Class-specific MS Interface Header Descriptor --- (7 байт)
    0x07,       // bLength
    0x24,       // bDescriptorType (CS_INTERFACE)
    0x01,       // bDescriptorSubtype (MS_HEADER)
    0x00, 0x01, // bcdMSC (1.00)
    0x26, 0x00, // wTotalLength подкласса (50 байт -> 0x0026)

    // --- MIDI IN Jack Descriptor (Embedded) --- (6 байт)
    0x06,       // bLength
    0x24,       // bDescriptorType (CS_INTERFACE)
    0x02,       // bDescriptorSubtype (MIDI_IN_JACK)
    0x01,       // bJackType (EMBEDDED)
    0x01,       // bJackID (1)
    0x00,       // iJack

    // --- MIDI OUT Jack Descriptor (Embedded) --- (9 байт)
    0x09,       // bLength
    0x24,       // bDescriptorType (CS_INTERFACE)
    0x03,       // bDescriptorSubtype (MIDI_OUT_JACK)
    0x01,       // bJackType (EMBEDDED)
    0x02,       // bJackID (2)
    0x01,       // bNrInputPins (1)
    0x01,       // baSourceID(1)
    0x01,       // baSourcePin(1)
    0x00,       // iJack

    // --- Standard Interrupt IN Endpoint Descriptor --- (9 байт)
    0x09,       // bLength
    0x05,       // bDescriptorType (Endpoint)
    0x81,       // bEndpointAddress (EP1 IN)
    0x03,       // bmAttributes (0x03 — переключено в режим Interrupt!)
    0x40, 0x00, // wMaxPacketSize (64 байта)
    0x0A,       //           bInterval (0x0A — опрос хостом раз в 10 миллисекунд!)
    0x00,       // bRefresh
    0x00,       // bSynchAddress

    // === КЛАСС-СПЕЦИФИЧНЫЙ ДЕСКРИПТОР ЭНДПОИНТА IN === (5 байт)
    0x05,       // bLength
    0x25,       // bDescriptorType (CS_ENDPOINT)
    0x01,       // bDescriptorSubtype (MS_GENERAL)
    0x01,       // bNumEmbeddedJacks (1 виртуальный разъем)
    0x01,       // baAssocJackID(1) (Привязываем к Embedded MIDI IN Jack ID 1)

    // --- Standard Interrupt OUT Endpoint Descriptor --- (9 байт)
    0x09,       // bLength
    0x05,       // bDescriptorType (Endpoint)
    0x01,       // bEndpointAddress (EP1 OUT)
    0x03,       // bmAttributes (0x03 — режим Interrupt)
    0x40, 0x00, // wMaxPacketSize (64 байта)
    0x0A,       //             bInterval (0x0A — опрос раз в 10 мс)
    0x00,       // bRefresh
    0x00,       // bSynchAddress

    // === КЛАСС-СПЕЦИФИЧНЫЙ ДЕСКРИПТОР ЭНДПОИНТА OUT === (5 байт)
    0x05,       // bLength
    0x25,       // bDescriptorType (CS_ENDPOINT)
    0x01,       // bDescriptorSubtype (MS_GENERAL)
    0x01,       // bNumEmbeddedJacks (1 виртуальный разъем)
    0x02        // baAssocJackID(1) (Привязываем к Embedded MIDI OUT Jack ID 2)
};

                                                                      
__code u8 LANGIDDESC[4] =                                             
{                                                                     
    0x04,0x03,                                                        
    0x09,0x04,                                                        
};                                                                    
                                                                      
__code u8 MANUFACTDESC[8] =                                           
{                                                                     
    0x08,0x03,                                                        
    'S',0,                                                            
    'T',0,                                                            
    'C',0,                                                            
};                                                                    
                                                                      
__code u8 PRODUCTDESC[30] =                                           
{                                                                     
    0x1e,0x03,                                                        
    'S',0,                                                            
    'T',0,                                                            
    'C',0,                                                            
    ' ',0,                                                            
    'U',0,                                                            
    'S',0,                                                            
    'B',0,                                                            
    ' ',0,                                                            
    'D',0,                                                            
    'e',0,                                                            
    'v',0,                                                            
    'i',0,                                                            
    'c',0,                                                            
    'e',0,                                                            
};

