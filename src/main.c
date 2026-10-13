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

#ifdef __cplusplus
extern "C" {
#endif

/*==================================================================================================
 *                                        INCLUDE FILES
 * 1) system and project includes
 * 2) needed interfaces from external units
 * 3) internal and external interfaces from this unit
==================================================================================================*/
#include "Adc_Sar_Ip.h"
#include "Clock_Ip.h"
#include "Siul2_Port_Ip.h"
#include "Siul2_Dio_Ip.h"
#include "Lpspi_Ip.h"
#include "OsIf.h"
#include "epaper.h"

/* SPI transfer timeout (ms), consumed by the Epaper module. */
const uint32 Epaper_SpiTimeoutMs = 1000U;

/* =========================================
   TEMPERATURE GAUGE SCENE
   ========================================= */

/* Temperature display range */
#define TEMP_MIN_C      (-40)
#define TEMP_MAX_C      (125)

/* Gauge bar geometry (horizontal, centered on panel) */
#define GAUGE_X         16
#define GAUGE_Y         110
#define GAUGE_W         84
#define GAUGE_H         30
#define GAUGE_WALL      3
#define GAUGE_PAD       4

/* =========================================
   ADC / TEMPSENSE HELPERS
   ========================================= */

/*
 * Stub notification - required by the generated Adc_Sar_Ip_PBcfg.c link
 * even when not using group-based conversions. Keep empty.
 */
void Adc_Adc0Group0Notification(void)
{
    /* Intentionally empty - temperature is read via Adc_Sar_Ip_TempSenseGetTemp */
}

/* =========================================
   DISPLAY HELPERS
   ========================================= */

/*
 * Format a temperature value as a string, e.g. "+27.3C" or "-10.5C".
 * out must be at least 10 chars.
 */
static void temp_to_str(float temp, char *out)
{
    int sign = (temp < 0.0f) ? -1 : 1;
    float abs_t = (temp < 0.0f) ? -temp : temp;
    int whole = (int)abs_t;
    int frac  = (int)((abs_t - (float)whole) * 10.0f);

    /* Build string manually (no printf/snprintf dependency in this module) */
    int i = 0;
    out[i++] = (sign < 0) ? '-' : '+';

    if (whole >= 100) { out[i++] = (char)('0' + whole / 100); }
    if (whole >=  10) { out[i++] = (char)('0' + (whole / 10) % 10); }
    out[i++] = (char)('0' + whole % 10);
    out[i++] = '.';
    out[i++] = (char)('0' + frac);
    out[i++] = 'C';
    out[i]   = '\0';
}

/*
 * Clamp an integer value to [lo, hi].
 */
static int clamp_int(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/*
 * Draw the temperature gauge scene for a given temperature (float, degC).
 * Range: TEMP_MIN_C to TEMP_MAX_C, displayed as a filled horizontal bar.
 */
static void draw_temp_gauge(float temp_c)
{
    char str[12];

    Epaper_ClearBuffer(1); /* white background */

    /* Title */
    Epaper_DrawStringCentered(20, "FRDM-A-S32K344", 1);
    Epaper_DrawStringCentered(40, "Temperature", 1);

    /* Divider */
    Epaper_FillRect(8, 60, EPAPER_WIDTH - 16, 2, 1);

    /* Gauge outline */
    Epaper_DrawRect(GAUGE_X, GAUGE_Y, GAUGE_W, GAUGE_H, GAUGE_WALL, 1);

    /* Min / max labels */
    Epaper_DrawString(GAUGE_X,               GAUGE_Y - 14, "-40C", 1);
    Epaper_DrawString(GAUGE_X + GAUGE_W - 24, GAUGE_Y - 14, "125C", 1);

    /* Filled portion proportional to temperature */
    int range       = TEMP_MAX_C - TEMP_MIN_C;   /* 165 degrees */
    int inner_x     = GAUGE_X + GAUGE_WALL + GAUGE_PAD;
    int inner_y     = GAUGE_Y + GAUGE_WALL + GAUGE_PAD;
    int inner_w     = GAUGE_W - 2 * (GAUGE_WALL + GAUGE_PAD);
    int inner_h     = GAUGE_H - 2 * (GAUGE_WALL + GAUGE_PAD);

    int temp_int    = (int)(temp_c * 10.0f);          /* scale to avoid float division */
    int range_int   = range * 10;
    int offset_int  = (int)((temp_c - (float)TEMP_MIN_C) * 10.0f);
    int fill_w      = (offset_int * inner_w) / range_int;
    fill_w = clamp_int(fill_w, 0, inner_w);

    if (fill_w > 0)
    {
        Epaper_FillRect(inner_x, inner_y, fill_w, inner_h, 1);
    }

    /* Suppress unused variable warning */
    (void)temp_int;

    /* Numeric readout below the bar (scale 2 for large text) */
    temp_to_str(temp_c, str);
    Epaper_DrawStringCentered(GAUGE_Y + GAUGE_H + 20, str, 2);
}

/* =========================================
   MAIN APPLICATION
   ========================================= */

int main(void)
{
    float        temp_c = 25.0f;   /* current temperature reading in degC */
    uint16       tempFp = 0U;      /* raw Q11.4 result from TempSenseGetTemp */
    unsigned int frame  = 0U;
    StatusType   status;

    /* Initialize Clocks, Pins, and SPI */
    Clock_Ip_Init(&Clock_Ip_aClockConfig[0]);
    Siul2_Port_Ip_Init(NUM_OF_CONFIGURED_PINS_PortContainer_0_BOARD_InitPeripherals,
                       g_pin_mux_InitConfigArr_PortContainer_0_BOARD_InitPeripherals);
    Lpspi_Ip_Init(&Lpspi_Ip_PhyUnitConfig_SpiPhyUnit_0_Instance_1);

    /* OsIf is used by Epaper_Delay() for accurate time-based delays. */
    OsIf_Init(NULL_PTR);

    /* Enable TempSense BEFORE ADC init (required by the IP layer). */
    Adc_Sar_Ip_TempSenseEnable(0U);

    /* ADC SAR IP init for ADC0 using the S32CT-generated configuration */
    status = (StatusType)Adc_Sar_Ip_Init(ADCHWUNIT_0_INSTANCE, &AdcHwUnit_0);
    (void)status;

    /* Call Calibration function multiple times, to mitigate instability of board source */
    for(uint8 Index = 0; Index <= 5; Index++)
    {
        status = (StatusType) Adc_Sar_Ip_DoCalibration(ADCHWUNIT_0_INSTANCE);
        if(status == E_OK)
        {
            break;
        }
    }

    /* Full SSD1675A / GDEH0213B72 init sequence (encapsulated in the driver).
     * Uploads the full waveform LUT and leaves the framebuffer cleared to white. */
    Epaper_Init();

    /* Draw the first frame and push it as the full-update baseline. */
    draw_temp_gauge(temp_c);
    Epaper_Flush();

    /* =========================================
       TEMPERATURE GAUGE LOOP
       - Each iteration reads the on-chip TempSense via ADC0 ch49 and redraws.
       - Every REBASE_PERIOD steps, run a full refresh to clear ghosting.
       ========================================= */

#define REBASE_PERIOD   60U   /* full-refresh cadence to clear ghosting */

    while (1)
    {
        /* Read on-chip temperature via Adc_Sar_Ip_TempSenseGetTemp.
         * Result is Q11.4 format: integer in bits [14:4], fractional in bits [3:0].
         * Extract parts separately to get positive float degC. */

        if (ADC_SAR_IP_STATUS_SUCCESS == Adc_Sar_Ip_TempSenseGetTemp(ADCHWUNIT_0_INSTANCE, 0U, &tempFp))
        {
            temp_c = (float)((tempFp & 0x7FF0U) >> 4U)
                   + (float)(tempFp & 0x0FU) / 16.0f;
        }

        frame++;
        draw_temp_gauge(temp_c);

        if ((frame % REBASE_PERIOD) == 0U)
        {
            Epaper_Flush();        /* periodic full refresh clears ghosting */
        }
        else
        {
            Epaper_FlushPartial(); /* flicker-free partial update */
        }

        Epaper_Delay(1000U);       /* update once per second */
    }

    return 0;
}


#ifdef __cplusplus
}
#endif
