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
 *   @file    epaper.h
 *   @brief   E-Paper (SSD1675A) Display Driver - Header
 *   @details Driver for HINK-E0213A22, TSE0213A189, GDEH0213B72 2.13" e-paper module
 *            (SSD1675A/IL3897 controller), 122 x 250 dots, driven over the S32K312 
 *            RTD LPSPI + Siul2_Dio drivers. Ported from the GxEPD2 GDEH0213B72 driver.
 *
 *            RAM format is 1 bit/pixel (8 px/byte), 16 bytes per row, 4000 bytes
 *            total. Custom full and partial waveform LUTs are uploaded via
 *            command 0x32: the full LUT for a clean baseline (0x22=0xC4) and the
 *            partial LUT for flicker-free differential updates (0x22=0x04). A
 *            framebuffer set bit (1) is WHITE and a clear bit (0) is BLACK.
 ==================================================================================================*/

#ifndef EPAPER_H
#define EPAPER_H

#ifdef __cplusplus
extern "C" {
#endif

/*==================================================================================================
 *                                        INCLUDE FILES
 * 1) system and project includes
 * 2) needed interfaces from external units
 * 3) internal and external interfaces from this unit
==================================================================================================*/
#include "Siul2_Dio_Ip.h"
#include "Siul2_Port_Ip.h"
#include "Lpspi_Ip.h"
#include "Std_Types.h"   /* uint8, uint32 used in the declarations below */

/*==================================================================================================
 *                                   DISPLAY GEOMETRY
==================================================================================================*/

/* Physical panel is 122 x 250 dots.
 * Controller RAM is byte-aligned: 16 bytes per row (128 bits) x 250 rows.
 * Only the leftmost 122 columns are visible; the rest is off-screen padding.
 * In the framebuffer a set bit (1) is WHITE and a clear bit (0) is BLACK,
 * matching the SSD1675A convention where 0xFF flushed to RAM shows white.
 */

/** @brief Panel width in pixels (portrait). */
#define EPAPER_WIDTH            122

/** @brief Panel height in pixels (portrait). */
#define EPAPER_HEIGHT           250

/** @brief Bytes per row (row stride): 122 px -> 16 bytes (128 bits, 122 used). */
#define EPAPER_BYTES_PER_ROW    16

/** @brief Total framebuffer size in bytes (row-major, 1bpp): 16 x 250 = 4000. */
#define EPAPER_RESOLUTION       (EPAPER_BYTES_PER_ROW * EPAPER_HEIGHT)

/** @brief SSD1675A RAM convention: bit = 1 -> white, bit = 0 -> black. */
#define EPAPER_PX_WHITE         1U
#define EPAPER_PX_BLACK         0U

/*==================================================================================================
 *                                    EXTERNAL VARIABLES
==================================================================================================*/

/* SPI transfer timeout (milliseconds). Defined by the application in main.c. */
extern const uint32 Epaper_SpiTimeoutMs;

/*==================================================================================================
 *                                   FUNCTION PROTOTYPES
==================================================================================================*/

/* --- Low-level panel control --- */
void Epaper_Delay(uint32 ms);
void Epaper_WaitBusy(void);
void Epaper_Reset(void);
void Epaper_WriteCmd(uint8 cmd);
void Epaper_WriteData(uint8 data);
void Epaper_WriteDataBurst(const uint8* buf, uint32 len);

/* --- Panel initialisation ---
 * Runs the full SSD1675A power-on init sequence (HW reset, block
 * control, driver output, border, drive voltages, RAM window) and uploads the
 * full waveform LUT. Leaves the framebuffer cleared to white. */
void Epaper_Init(void);

/* --- Framebuffer + drawing primitives --- */

/** @brief Clear the entire local frame buffer (white != 0 -> white, else black). */
void Epaper_ClearBuffer(uint8 white);

/** @brief Set or clear a single pixel (black != 0 draws black, else white). */
void Epaper_DrawPixel(int x, int y, uint8 black);

/** @brief Fill a rectangular region with a solid colour. */
void Epaper_FillRect(int x, int y, int w, int h, uint8 black);

/** @brief Draw a rectangle outline of the given wall thickness. */
void Epaper_DrawRect(int x, int y, int w, int h, int t, uint8 black);

/** @brief Render a single character using the built-in 5x7 bitmap font. */
void Epaper_DrawChar(int x, int y, char c, int scale);

/** @brief Compute the rendered pixel width of a string at the given scale. */
int  Epaper_TextWidth(const char* s, int scale);

/** @brief Render a null-terminated string using the built-in 5x7 font. */
void Epaper_DrawString(int x, int y, const char* s, int scale);

/** @brief Render a string horizontally centred on the panel at row y. */
void Epaper_DrawStringCentered(int y, const char* s, int scale);

/* --- Flush framebuffer to panel ---
 * Usage:
 *   1. Draw the first frame, call Epaper_Flush() once for a clean baseline
 *      (full waveform LUT + 0x22=0xC4; also seeds the 0x26 reference image).
 *   2. Per update: draw the new frame, call Epaper_FlushPartial() (partial
 *      waveform LUT + 0x22=0x04) - only changed pixels re-drive, flicker-free.
 *   3. Re-seed periodically with Epaper_Flush() to clear accumulated ghosting. */

/* Full refresh (0x22=0xC4). Whole panel briefly flashes; clean baseline. */
void Epaper_Flush(void);

/* Flicker-free partial refresh (0x22=0x04). Must be preceded by Epaper_Flush(). */
void Epaper_FlushPartial(void);


#ifdef __cplusplus

}
#endif

#endif /* EPAPER_H */
