/*==================================================================================================
*   Project              : RTD AUTOSAR 4.9
*   Platform             : CORTEXM
*   Peripheral           : S32K3XX
*   Dependencies         : none
*
*   Autosar Version      : 4.9.0
*   Autosar Revision     : ASR_REL_4_9_REV_0000
*   Autosar Conf.Variant :
*   SW Version           : 7.0.1
*   Build Version        : S32K3_RTD_7_0_1_D2602_ASR_REL_4_9_REV_0000_20260206
*
*   Copyright 2020 - 2026 NXP
*
*   NXP Proprietary. This software is owned or controlled by NXP and may only be
*   used strictly in accordance with the applicable license terms. By expressly
*   accepting such terms or by downloading, installing, activating and/or otherwise
*   using the software, you are agreeing that you have read, and that you agree to
*   comply with and are bound by, such license terms. If you do not agree to be
*   bound by the applicable license terms, then you may not retain, install,
*   activate or otherwise use the software.
==================================================================================================*/

/*==================================================================================================
 *                                        INCLUDE FILES
 * 1) system and project includes
 * 2) needed interfaces from external units
 * 3) internal and external interfaces from this unit
==================================================================================================*/
#include "epaper.h"
#include "OsIf.h"


/* --- Hardware Definitions ---
 * SPI_INSTANCE and the transfer timeout live in main.c (application config).
 * The timeout is exposed here through the extern Epaper_SpiTimeoutMs. */
extern const Lpspi_Ip_ExternalDeviceType Lpspi_Ip_DeviceAttributes_SpiExternalDevice_0_Instance_1;

#define SPI_EXTERNAL_DEVICE &Lpspi_Ip_DeviceAttributes_SpiExternalDevice_0_Instance_1

/* =========================================
   SSD1675A COMMAND SET
   Named constants replace raw magic numbers throughout this driver.
   ========================================= */
#define SSD1675A_CMD_DRIVER_OUTPUT_CONTROL   0x01U
#define SSD1675A_CMD_DATA_ENTRY_MODE         0x11U
#define SSD1675A_CMD_SW_RESET                0x12U
#define SSD1675A_CMD_TEMP_SENSOR_CONTROL     0x18U
#define SSD1675A_CMD_WRITE_TEMP_REGISTER     0x1AU
#define SSD1675A_CMD_MASTER_ACTIVATION       0x20U
#define SSD1675A_CMD_DISPLAY_UPDATE_CTRL1    0x21U
#define SSD1675A_CMD_DISPLAY_UPDATE_CTRL2    0x22U
#define SSD1675A_CMD_WRITE_RAM               0x24U
#define SSD1675A_CMD_WRITE_RAM_RED           0x26U
#define SSD1675A_CMD_BORDER_WAVEFORM_CTRL    0x3CU
#define SSD1675A_CMD_SET_RAM_X_START_END     0x44U
#define SSD1675A_CMD_SET_RAM_Y_START_END     0x45U
#define SSD1675A_CMD_SET_RAM_X_COUNTER       0x4EU
#define SSD1675A_CMD_SET_RAM_Y_COUNTER       0x4FU


/* --- Helper Functions --- */

void Epaper_Delay(uint32 ms) {
    /* Real time-based delay using the OsIf system counter. This is independent
     * of the core clock frequency (unlike a nop busy-loop), so reset pulse
     * widths and setup/hold times are met regardless of the clock config. */
    uint32 cur     = OsIf_GetCounter(OSIF_COUNTER_SYSTEM);
    uint32 elapsed = 0U;
    uint32 timeout = OsIf_MicrosToTicks(ms * 1000U, OSIF_COUNTER_SYSTEM);

    while (elapsed < timeout) {
        elapsed += OsIf_GetElapsed(&cur, OSIF_COUNTER_SYSTEM);
    }
}

/* Poll the BUSY pin until the panel finishes its operation.
 * On the SSD1675A BUSY is active-high (1 = busy). A time-based guard gives a
 * deterministic ~4 s ceiling (a full refresh completes in well under 3 s)
 * regardless of core clock, preventing an infinite hang if BUSY never clears. */
void Epaper_WaitBusy(void) {
    /* Waveshare reference pattern: poll the BUSY level every 10 ms while it is
     * high. Simple and robust - no pre-settle needed because we loop on the
     * level, not an edge. A generous guard prevents an infinite hang. */
    uint32 guard = 0U;
    while (Siul2_Dio_Ip_ReadPin(BSY_PORT, BSY_PIN) == 1) {
        Epaper_Delay(10U);
        if (++guard > 400U) {
            break; /* safety timeout (~4 s) */
        }
    }
}


void Epaper_Reset(void) {
    Siul2_Dio_Ip_WritePin(CS_PORT, CS_PIN, 1);
    Epaper_Delay(20);
    Siul2_Dio_Ip_WritePin(RST_PORT, RST_PIN, 1);
    Epaper_Delay(20);
    Siul2_Dio_Ip_WritePin(RST_PORT, RST_PIN, 0); /* Hardware Reset */
    Epaper_Delay(20);
    Siul2_Dio_Ip_WritePin(RST_PORT, RST_PIN, 1);
    Epaper_Delay(50);
}

void Epaper_WriteCmd(uint8 cmd) {
    /* CS setup/hold delays give the SSD1675A timing margin around each frame.
     * At 120 MHz core / 2 MHz SPI these are needed or commands can mis-latch. */
    Siul2_Dio_Ip_WritePin(DC_PORT, DC_PIN, 0);
    Siul2_Dio_Ip_WritePin(CS_PORT, CS_PIN, 0);
    Epaper_Delay(1); /* CS setup time */
    Lpspi_Ip_SyncTransmit(SPI_EXTERNAL_DEVICE, &cmd, NULL, 1, Epaper_SpiTimeoutMs);
    Epaper_Delay(1); /* CS hold time */
    Siul2_Dio_Ip_WritePin(CS_PORT, CS_PIN, 1);
}

void Epaper_WriteData(uint8 data) {
    Siul2_Dio_Ip_WritePin(DC_PORT, DC_PIN, 1);
    Siul2_Dio_Ip_WritePin(CS_PORT, CS_PIN, 0);
    Epaper_Delay(1); /* CS setup time */
    Lpspi_Ip_SyncTransmit(SPI_EXTERNAL_DEVICE, &data, NULL, 1, Epaper_SpiTimeoutMs);
    Epaper_Delay(1); /* CS hold time */
    Siul2_Dio_Ip_WritePin(CS_PORT, CS_PIN, 1);
}

/* Write a contiguous block of data bytes as ONE burst.
 * CS is held low for the entire transfer while the SSD1675A auto-increments
 * its RAM write pointer. This is required for the 0x24 RAM load; toggling CS
 * per byte corrupts the burst and shows uninitialized RAM (random pixels). */
void Epaper_WriteDataBurst(const uint8* buf, uint32 len) {
    Siul2_Dio_Ip_WritePin(DC_PORT, DC_PIN, 1);
    Siul2_Dio_Ip_WritePin(CS_PORT, CS_PIN, 0);
    Epaper_Delay(1); /* CS setup time */
    Lpspi_Ip_SyncTransmit(SPI_EXTERNAL_DEVICE, buf, NULL, len, Epaper_SpiTimeoutMs);
    Epaper_Delay(1); /* CS hold time */
    Siul2_Dio_Ip_WritePin(CS_PORT, CS_PIN, 1);
}


/* =========================================
   PANEL INITIALISATION (SSD1675A)
   Ported from the GxEPD2 GDEH0213B72 driver. The panel needs custom
   full and partial waveform LUTs uploaded via command 0x32; the full LUT is
   used for a clean baseline (0x22=0xC4) and the partial LUT for flicker-free
   differential updates (0x22=0x04).
   ========================================= */
/* Waveform LUTs (SSD1675A) 70 bytes each. */
static const uint8 LUT_DATA_full[70] = {
    0x80,0x60,0x40,0x00,0x00,0x00,0x00,
    0x10,0x60,0x20,0x00,0x00,0x00,0x00,
    0x80,0x60,0x40,0x00,0x00,0x00,0x00,
    0x10,0x60,0x20,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x03,0x03,0x00,0x00,0x02,
    0x09,0x09,0x00,0x00,0x02,
    0x03,0x03,0x00,0x00,0x02,
    0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00
};
static const uint8 LUT_DATA_part[70] = {
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x80,0x00,0x00,0x00,0x00,0x00,0x00,
    0x40,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x0A,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00
};

/* Set the full-frame RAM window + counters (SetPartialRamArea). */
static void Epaper_SetRamArea(void) {
    Epaper_WriteCmd(SSD1675A_CMD_DATA_ENTRY_MODE);
    Epaper_WriteData(0x03);                 /* X+, Y+ */
    Epaper_WriteCmd(SSD1675A_CMD_SET_RAM_X_START_END);
    Epaper_WriteData(0x00);                 /* x/8 start */
    Epaper_WriteData((uint8)((EPAPER_WIDTH - 1) / 8));   /* x/8 end */
    Epaper_WriteCmd(SSD1675A_CMD_SET_RAM_Y_START_END);
    Epaper_WriteData(0x00);                 /* y start lo */
    Epaper_WriteData(0x00);                 /* y start hi */
    Epaper_WriteData((uint8)((EPAPER_HEIGHT - 1) % 256));/* y end lo */
    Epaper_WriteData((uint8)((EPAPER_HEIGHT - 1) / 256));/* y end hi */
    Epaper_WriteCmd(SSD1675A_CMD_SET_RAM_X_COUNTER);
    Epaper_WriteData(0x00);
    Epaper_WriteCmd(SSD1675A_CMD_SET_RAM_Y_COUNTER);
    Epaper_WriteData(0x00);
    Epaper_WriteData(0x00);
}

/* InitDisplay: block control, driver output, border, voltages. */
static void Epaper_InitDisplay(void) {
    Epaper_Reset();

    Epaper_WriteCmd(0x74); Epaper_WriteData(0x54);   /* analog block control  */
    Epaper_WriteCmd(0x7E); Epaper_WriteData(0x3B);   /* digital block control */

    Epaper_WriteCmd(SSD1675A_CMD_DRIVER_OUTPUT_CONTROL);
    Epaper_WriteData(0xF9);   /* (HEIGHT-1) % 256 */
    Epaper_WriteData(0x00);   /* (HEIGHT-1) / 256 */
    Epaper_WriteData(0x00);

    Epaper_WriteCmd(SSD1675A_CMD_BORDER_WAVEFORM_CTRL);
    Epaper_WriteData(0x03);

    Epaper_WriteCmd(0x2C); Epaper_WriteData(0x70);   /* VCOM voltage           */
    Epaper_WriteCmd(0x03); Epaper_WriteData(0x15);   /* gate driving voltage   */
    Epaper_WriteCmd(0x04);                            /* source driving voltage */
    Epaper_WriteData(0x41);  /* VSH1 15V */
    Epaper_WriteData(0xA8);  /* VSH2 5V  */
    Epaper_WriteData(0x32);  /* VSL -15V */
    Epaper_WriteCmd(0x3A); Epaper_WriteData(0x30);   /* dummy line */
    Epaper_WriteCmd(0x3B); Epaper_WriteData(0x0A);   /* gate time  */

    Epaper_SetRamArea();
}

/* Power on the panel (PowerOn: 0x22=0xC0 -> activate). */
static void Epaper_PowerOn(void) {
    Epaper_WriteCmd(SSD1675A_CMD_DISPLAY_UPDATE_CTRL2);
    Epaper_WriteData(0xC0);
    Epaper_WriteCmd(SSD1675A_CMD_MASTER_ACTIVATION);
    Epaper_WaitBusy();
}

/* Full-mode init: init sequence + full LUT + power on. */
void Epaper_Init(void) {
    Epaper_InitDisplay();
    Epaper_WriteCmd(0x32);
    { uint16 i; for (i = 0; i < 70U; i++) Epaper_WriteData(LUT_DATA_full[i]); }
    Epaper_PowerOn();

    /* Start from a white buffer (no refresh issued here). */
    Epaper_ClearBuffer(1);
}



/* =========================================
   FRAMEBUFFER + DRAWING PRIMITIVES
   ========================================= */

static uint8 fb[EPAPER_RESOLUTION];

/* Fill whole framebuffer: white = 1, black = 0 */
void Epaper_ClearBuffer(uint8 white) {
    uint8 val = white ? 0xFF : 0x00;
    for(int i = 0; i < EPAPER_RESOLUTION; i++) {
        fb[i] = val;
    }
}

/* Set a single pixel. black != 0 draws black, else white.
 * Pixel x=0 maps to the MSB (bit 7) of the first byte in the row. */
void Epaper_DrawPixel(int x, int y, uint8 black) {
    if (x < 0 || x >= EPAPER_WIDTH || y < 0 || y >= EPAPER_HEIGHT) {
        return;
    }
    int idx = y * EPAPER_BYTES_PER_ROW + (x >> 3);
    uint8 mask = (uint8)(0x80U >> (x & 7));
    if (black) {
        fb[idx] &= (uint8)~mask;   /* clear bit -> black */
    } else {
        fb[idx] |= mask;           /* set bit -> white */
    }
}

/* Filled rectangle */
void Epaper_FillRect(int x, int y, int w, int h, uint8 black) {
    for(int yy = y; yy < y + h; yy++) {
        for(int xx = x; xx < x + w; xx++) {
            Epaper_DrawPixel(xx, yy, black);
        }
    }
}

/* Rectangle outline of given thickness */
void Epaper_DrawRect(int x, int y, int w, int h, int t, uint8 black) {
    Epaper_FillRect(x, y, w, t, black);               /* top */
    Epaper_FillRect(x, y + h - t, w, t, black);        /* bottom */
    Epaper_FillRect(x, y, t, h, black);                /* left */
    Epaper_FillRect(x + w - t, y, t, h, black);        /* right */
}


/* =========================================
   5x7 BITMAP FONT
   Each glyph is 5 columns wide, 7 rows tall.
   Bytes are column-major; bit 0 (LSB) is the top row.
   Rendered with 1px spacing -> 6px character pitch.
   ========================================= */

typedef struct {
    char c;
    uint8 col[5];
} GlyphType;

static const GlyphType FONT[] = {
    {' ', {0x00,0x00,0x00,0x00,0x00}},
    {'-', {0x08,0x08,0x08,0x08,0x08}},
    {'.', {0x00,0x60,0x60,0x00,0x00}},
    {'%', {0x23,0x13,0x08,0x64,0x62}},
    {'0', {0x3E,0x51,0x49,0x45,0x3E}},
    {'1', {0x00,0x42,0x7F,0x40,0x00}},
    {'2', {0x42,0x61,0x51,0x49,0x46}},
    {'3', {0x21,0x41,0x45,0x4B,0x31}},
    {'4', {0x18,0x14,0x12,0x7F,0x10}},
    {'5', {0x27,0x45,0x45,0x45,0x39}},
    {'6', {0x3C,0x4A,0x49,0x49,0x30}},
    {'7', {0x01,0x71,0x09,0x05,0x03}},
    {'8', {0x36,0x49,0x49,0x49,0x36}},
    {'9', {0x06,0x49,0x49,0x29,0x1E}},
    {'A', {0x7E,0x11,0x11,0x11,0x7E}},
    {'B', {0x7F,0x49,0x49,0x49,0x36}},
    {'C', {0x3E,0x41,0x41,0x41,0x22}},
    {'D', {0x7F,0x41,0x41,0x22,0x1C}},
    {'E', {0x7F,0x49,0x49,0x49,0x41}},
    {'F', {0x7F,0x09,0x09,0x09,0x01}},
    {'G', {0x3E,0x41,0x49,0x49,0x7A}},
    {'H', {0x7F,0x08,0x08,0x08,0x7F}},
    {'I', {0x00,0x41,0x7F,0x41,0x00}},
    {'J', {0x20,0x40,0x41,0x3F,0x01}},
    {'K', {0x7F,0x08,0x14,0x22,0x41}},
    {'L', {0x7F,0x40,0x40,0x40,0x40}},
    {'M', {0x7F,0x02,0x0C,0x02,0x7F}},
    {'N', {0x7F,0x04,0x08,0x10,0x7F}},
    {'O', {0x3E,0x41,0x41,0x41,0x3E}},
    {'P', {0x7F,0x09,0x09,0x09,0x06}},
    {'Q', {0x3E,0x41,0x51,0x21,0x5E}},
    {'R', {0x7F,0x09,0x19,0x29,0x46}},
    {'S', {0x46,0x49,0x49,0x49,0x31}},
    {'T', {0x01,0x01,0x7F,0x01,0x01}},
    {'U', {0x3F,0x40,0x40,0x40,0x3F}},
    {'V', {0x1F,0x20,0x40,0x20,0x1F}},
    {'W', {0x7F,0x20,0x18,0x20,0x7F}},
    {'X', {0x63,0x14,0x08,0x14,0x63}},
    {'Y', {0x03,0x04,0x78,0x04,0x03}},
    {'Z', {0x61,0x51,0x49,0x45,0x43}},
    {'a', {0x20,0x54,0x54,0x54,0x78}},
    {'b', {0x7F,0x48,0x44,0x44,0x38}},
    {'c', {0x38,0x44,0x44,0x44,0x20}},
    {'d', {0x38,0x44,0x44,0x48,0x7F}},
    {'e', {0x38,0x54,0x54,0x54,0x18}},
    {'f', {0x08,0x7E,0x09,0x01,0x02}},
    {'g', {0x0C,0x52,0x52,0x52,0x3E}},
    {'h', {0x7F,0x08,0x04,0x04,0x78}},
    {'i', {0x00,0x44,0x7D,0x40,0x00}},
    {'j', {0x20,0x40,0x44,0x3D,0x00}},
    {'k', {0x7F,0x10,0x28,0x44,0x00}},
    {'l', {0x00,0x41,0x7F,0x40,0x00}},
    {'m', {0x7C,0x04,0x18,0x04,0x78}},
    {'n', {0x7C,0x08,0x04,0x04,0x78}},
    {'o', {0x38,0x44,0x44,0x44,0x38}},
    {'p', {0x7C,0x14,0x14,0x14,0x08}},
    {'q', {0x08,0x14,0x14,0x18,0x7C}},
    {'r', {0x7C,0x08,0x04,0x04,0x08}},
    {'s', {0x48,0x54,0x54,0x54,0x20}},
    {'t', {0x04,0x3F,0x44,0x40,0x20}},
    {'u', {0x3C,0x40,0x40,0x20,0x7C}},
    {'v', {0x1C,0x20,0x40,0x20,0x1C}},
    {'w', {0x3C,0x40,0x30,0x40,0x3C}},
    {'x', {0x44,0x28,0x10,0x28,0x44}},
    {'y', {0x0C,0x50,0x50,0x50,0x3C}},
    {'z', {0x44,0x64,0x54,0x4C,0x44}},
};

#define FONT_COUNT (sizeof(FONT) / sizeof(FONT[0]))

/* Look up a glyph; returns pointer to blank glyph (space) if unknown */
static const GlyphType* font_lookup(char c) {
    for(unsigned int i = 0; i < FONT_COUNT; i++) {
        if (FONT[i].c == c) {
            return &FONT[i];
        }
    }
    return &FONT[0]; /* space */
}

/* Draw a single character at (x,y), top-left origin, optional integer scale */
void Epaper_DrawChar(int x, int y, char c, int scale) {
    const GlyphType* g = font_lookup(c);
    for(int col = 0; col < 5; col++) {
        uint8 bits = g->col[col];
        for(int row = 0; row < 7; row++) {
            if (bits & (1U << row)) {
                Epaper_FillRect(x + col * scale, y + row * scale, scale, scale, 1);
            }
        }
    }
}

/* Compute rendered pixel width of a string at given scale */
int Epaper_TextWidth(const char* s, int scale) {
    int n = 0;
    while (s[n]) n++;
    if (n == 0) return 0;
    return (n * 6 - 1) * scale; /* 6px pitch, minus trailing space */
}

/* Draw a string starting at (x,y) */
void Epaper_DrawString(int x, int y, const char* s, int scale) {
    int cx = x;
    while (*s) {
        Epaper_DrawChar(cx, y, *s, scale);
        cx += 6 * scale;
        s++;
    }
}

/* Draw a string centered horizontally on the panel */
void Epaper_DrawStringCentered(int y, const char* s, int scale) {
    int w = Epaper_TextWidth(s, scale);
    int x = (EPAPER_WIDTH - w) / 2;
    if (x < 0) x = 0;
    Epaper_DrawString(x, y, s, scale);
}


/* =========================================
   FULL REFRESH (Update_Full, 0x22=0xC4)
   Loads the full waveform LUT, writes the framebuffer to working RAM (0x24),
   triggers a full refresh, then seeds the base RAM (0x26) with the same image
   so the next partial update has a valid reference to diff against. The whole
   panel briefly flashes but leaves a perfectly clean image and clears ghosting.
   Used for the boot baseline and periodic re-seed.
   ========================================= */
void Epaper_Flush(void) {
    /* Full-mode init + full LUT + power on (Init_Full). */
    Epaper_InitDisplay();
    Epaper_WriteCmd(0x32);
    { uint16 i; for (i = 0; i < 70U; i++) Epaper_WriteData(LUT_DATA_full[i]); }
    Epaper_PowerOn();

    /* Write working image (0x24), full update (0x22=0xC4), then seed old
     * data buffer (0x26) with the same image so the next partial diffs. */
    Epaper_SetRamArea();
    Epaper_WriteCmd(SSD1675A_CMD_WRITE_RAM);
    Epaper_WriteDataBurst(fb, EPAPER_RESOLUTION);

    Epaper_WriteCmd(SSD1675A_CMD_DISPLAY_UPDATE_CTRL2);
    Epaper_WriteData(0xC4);
    Epaper_WriteCmd(SSD1675A_CMD_MASTER_ACTIVATION);
    Epaper_WaitBusy();

    Epaper_SetRamArea();
    Epaper_WriteCmd(SSD1675A_CMD_WRITE_RAM_RED);
    Epaper_WriteDataBurst(fb, EPAPER_RESOLUTION);
}


/* =========================================
   PARTIAL REFRESH (Update_Part, 0x22=0x04)
   Runs the B72 partial init (partial VCOM 0x2C=0x26 + partial waveform LUT),
   writes the new framebuffer to working RAM (0x24), and triggers a partial
   update. Only pixels that differ from the base RAM (0x26) are re-driven, so
   unchanged regions stay put and the update is flicker-free. Afterwards the
   framebuffer is copied into 0x26 so the next partial update diffs against it.

   Usage:
     1. Draw the first frame, call Epaper_Flush() once to lay down the clean
        baseline (also seeds the 0x26 reference image).
     2. Per update: draw the new frame, call Epaper_FlushPartial().
     3. Re-seed periodically with Epaper_Flush() to clear accumulated ghosting.
   ========================================= */
void Epaper_FlushPartial(void) {
    /* Partial-mode init + partial VCOM + partial LUT + power on */
    Epaper_InitDisplay();
    Epaper_WriteCmd(0x2C); Epaper_WriteData(0x26);     /* partial VCOM */
    Epaper_WriteCmd(0x32);
    { uint16 i; for (i = 0; i < 70U; i++) Epaper_WriteData(LUT_DATA_part[i]); }
    Epaper_PowerOn();

    /* Write new image to 0x24, partial update (0x22=0x04), then seed 0x26
     * with the same image so the next partial diffs against it. */
    Epaper_SetRamArea();
    Epaper_WriteCmd(SSD1675A_CMD_WRITE_RAM);
    Epaper_WriteDataBurst(fb, EPAPER_RESOLUTION);

    Epaper_WriteCmd(SSD1675A_CMD_DISPLAY_UPDATE_CTRL2);
    Epaper_WriteData(0x04);
    Epaper_WriteCmd(SSD1675A_CMD_MASTER_ACTIVATION);
    Epaper_WaitBusy();

    Epaper_SetRamArea();
    Epaper_WriteCmd(SSD1675A_CMD_WRITE_RAM_RED);
    Epaper_WriteDataBurst(fb, EPAPER_RESOLUTION);
}
