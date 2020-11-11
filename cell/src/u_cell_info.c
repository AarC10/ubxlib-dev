/*
 * Copyright 2020 u-blox Ltd
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* Only #includes of u_* and the C standard library are allowed here,
 * no platform stuff and no OS stuff.  Anything required from
 * the platform/OS must be brought in through u_port* to maintain
 * portability.
 */

/** @file
 * @brief Implementation of the info API for cellular.
 */

#ifdef U_CFG_OVERRIDE
# include "u_cfg_override.h" // For a customer's configuration override
#endif

#include "errno.h"
#include "limits.h"    // INT_MAX
#include "stdlib.h"    // malloc(), free(), strol(), atoi(), strol(), strtof()
#include "stddef.h"    // NULL, size_t etc.
#include "stdint.h"    // int32_t etc.
#include "stdbool.h"
#include "math.h"      // pow(), log10()
#include "time.h"      // mktime(), struct tm

#include "u_cfg_sw.h"

#include "u_error_common.h"

#include "u_port_debug.h"
#include "u_port_os.h"
#include "u_port_clib_platform_specific.h" // Required for strtok_r()

#include "u_at_client.h"

#include "u_cell_module_type.h"
#include "u_cell.h"         // Order is
#include "u_cell_net.h"     // important here
#include "u_cell_private.h" // don't change it
#include "u_cell_info.h"

/* ----------------------------------------------------------------
 * COMPILE-TIME MACROS
 * -------------------------------------------------------------- */

/* ----------------------------------------------------------------
 * TYPES
 * -------------------------------------------------------------- */

/* ----------------------------------------------------------------
 * VARIABLES
 * -------------------------------------------------------------- */

/** Array to convert the LTE RSSI number from AT+CSQ into a
 * dBm value rounded up to the nearest whole number.
 */
static const int32_t gRssiConvertLte[] = {-118, -115, -113, -110, -108, -105, -103, -100,  /* 0 - 7   */
                                          -98,  -95,  -93,  -90,  -88,  -85,  -83,  -80,   /* 8 - 15  */
                                          -78,  -76,  -74,  -73,  -71,  -69,  -68,  -65,   /* 16 - 23 */
                                          -63,  -61,  -60,  -59,  -58,  -55,  -53,  -48
                                          };  /* 24 - 31 */

/* ----------------------------------------------------------------
 * STATIC FUNCTIONS
 * -------------------------------------------------------------- */

// Convert RSRP in 36.133 format to dBm.
// Returns 0 if the number is not known.
// 0: -141 dBm or less,
// 1..96: from -140 dBm to -45 dBm with 1 dBm steps,
// 97: -44 dBm or greater,
// 255: not known or not detectable.
static int32_t rsrpToDbm(int32_t rsrp)
{
    int32_t rsrpDbm = 0;

    if ((rsrp >= 0) && (rsrp <= 97)) {
        rsrpDbm = rsrp - (97 + 44);
        if (rsrpDbm < -141) {
            rsrpDbm = -141;
        }
    }

    return rsrpDbm;
}

// Convert RSRQ in 36.133 format to dB.
// Returns 0 if the number is not known.
// 0: less than -19.5 dB
// 1..33: from -19.5 dB to -3.5 dB with 0.5 dB steps
// 34: -3 dB or greater
// 255: not known or not detectable.
static int32_t rsrqToDb(int32_t rsrq)
{
    int32_t rsrqDb = 0;

    if ((rsrq >= 0) && (rsrq <= 34)) {
        rsrqDb = (rsrq - (34 + 6)) / 2;
        if (rsrqDb < -19) {
            rsrqDb = -19;
        }
    }

    return rsrqDb;
}

// Get an ID string from the cellular module.
static int32_t getString(uAtClientHandle_t atHandle,
                         const char *pCmd, char *pBuffer,
                         size_t bufferSize)
{
    int32_t errorCodeOrSize;
    int32_t bytesRead;
    char delimiter;

    uAtClientLock(atHandle);
    uAtClientCommandStart(atHandle, pCmd);
    uAtClientCommandStop(atHandle);
    // Don't want characters in the string being interpreted
    // as delimiters
    delimiter = uAtClientDelimiterGet(atHandle);
    uAtClientDelimiterSet(atHandle, '\x00');
    uAtClientResponseStart(atHandle, NULL);
    bytesRead = uAtClientReadString(atHandle, pBuffer,
                                    bufferSize, false);
    uAtClientResponseStop(atHandle);
    // Restore the delimiter
    uAtClientDelimiterSet(atHandle, delimiter);
    errorCodeOrSize = uAtClientUnlock(atHandle);
    if ((bytesRead >= 0) && (errorCodeOrSize == 0)) {
        uPortLog("U_CELL_INFO: ID string, length %d character(s),"
                 " returned by %s is \"%s\".\n",
                 errorCodeOrSize, pCmd, pBuffer);
        errorCodeOrSize = bytesRead;
    } else {
        errorCodeOrSize = (int32_t) U_CELL_ERROR_AT;
        uPortLog("U_CELL_INFO: unable to read ID string using"
                 " %s.\n", pCmd);
    }

    return errorCodeOrSize;
}

// Fill in the radio parameters the AT+CSQ way
static int32_t getRadioParamsCsq(uAtClientHandle_t atHandle,
                                 uCellPrivateRadioParameters_t *pRadioParameters)
{
    int32_t errorCode;
    int32_t x;
    int32_t y;

    uAtClientLock(atHandle);
    uAtClientCommandStart(atHandle, "AT+CSQ");
    uAtClientCommandStop(atHandle);
    uAtClientResponseStart(atHandle, "+CSQ:");
    x = uAtClientReadInt(atHandle);
    y = uAtClientReadInt(atHandle);
    if (y == 99) {
        y = -1;
    }
    uAtClientResponseStop(atHandle);
    errorCode = uAtClientUnlock(atHandle);

    if (errorCode == 0) {
        if ((x >= 0) &&
            (x < (int32_t) (sizeof(gRssiConvertLte) / sizeof(gRssiConvertLte[0])))) {
            pRadioParameters->rssiDbm = gRssiConvertLte[x];
        }
        pRadioParameters->rxQual = y;
    }

    return errorCode;
}

// Fill in the radio parameters the AT+UCGED=2 way
static int32_t getRadioParamsUcged2(uAtClientHandle_t atHandle,
                                    uCellPrivateRadioParameters_t *pRadioParameters)
{
    // +UCGED: 2
    // <rat>,<svc>,<MCC>,<MNC>
    // <earfcn>,<Lband>,<ul_BW>,<dl_BW>,<tac>,<LcellId>,<PCID>,<mTmsi>,<mmeGrId>,<mmeCode>, <rsrp>,<rsrq>,<Lsinr>,<Lrrc>,<RI>,<CQI>,<avg_rsrp>,<totalPuschPwr>,<avgPucchPwr>,<drx>, <l2w>,<volte_mode>[,<meas_gap>,<tti_bundling>]
    // e.g.
    // 6,4,001,01
    // 2525,5,50,50,e8fe,1a2d001,1,d60814d1,8001,01,28,31,13.75,3,1,10,28,-50,-6,0,255,255,0
    uAtClientLock(atHandle);
    uAtClientCommandStart(atHandle, "AT+UCGED?");
    uAtClientCommandStop(atHandle);
    // The line with just "+UCGED: 2" on it
    uAtClientResponseStart(atHandle, "+UCGED:");
    uAtClientSkipParameters(atHandle, 1);
    // Don't want anything from the next line
    uAtClientResponseStart(atHandle, NULL);
    uAtClientSkipParameters(atHandle, 4);
    // Now the line of interest
    uAtClientResponseStart(atHandle, NULL);
    // EARFCN is the first integer
    pRadioParameters->earfcn = uAtClientReadInt(atHandle);
    // Skip <Lband>, <ul_BW>, <dl_BW>, <tac> and <LcellId>
    uAtClientSkipParameters(atHandle, 5);
    // Read <PCID>
    pRadioParameters->cellId = uAtClientReadInt(atHandle);
    // Skip <mTmsi>, <mmeGrId> and <mmeCode>
    uAtClientSkipParameters(atHandle, 3);
    // RSRP is element 15, coded as specified in TS 36.133
    pRadioParameters->rsrpDbm = rsrpToDbm(uAtClientReadInt(atHandle));
    // RSRQ is element 16, coded as specified in TS 36.133
    pRadioParameters->rsrqDb = rsrqToDb(uAtClientReadInt(atHandle));
    uAtClientResponseStop(atHandle);

    return uAtClientUnlock(atHandle);
}

// Fill in the radio parameters the AT+UCGED=5 way
static int32_t getRadioParamsUcged5(uAtClientHandle_t atHandle,
                                    uCellPrivateRadioParameters_t *pRadioParameters)
{
    char buffer[16];
    double rsrx;

    uAtClientLock(atHandle);
    uAtClientCommandStart(atHandle, "AT+UCGED?");
    uAtClientCommandStop(atHandle);
    uAtClientResponseStart(atHandle, "+RSRP:");
    pRadioParameters->cellId = uAtClientReadInt(atHandle);
    pRadioParameters->earfcn = uAtClientReadInt(atHandle);
    if (uAtClientReadString(atHandle, buffer, sizeof(buffer), false) > 0) {
        rsrx = strtof(buffer, NULL);
        if (rsrx >= 0) {
            pRadioParameters->rsrpDbm = (int32_t) (rsrx + 0.5);
        } else {
            pRadioParameters->rsrpDbm = (int32_t) (rsrx - 0.5);
        }
    }
    uAtClientResponseStart(atHandle, "+RSRQ:");
    // Skip past cell ID and EARFCN since they will be the same
    uAtClientSkipParameters(atHandle, 2);
    if (uAtClientReadString(atHandle, buffer, sizeof(buffer), false) > 0) {
        rsrx = strtof(buffer, NULL);
        if (rsrx >= 0) {
            pRadioParameters->rsrqDb = (int32_t) (rsrx + 0.5);
        } else {
            pRadioParameters->rsrqDb = (int32_t) (rsrx - 0.5);
        }
    }
    uAtClientResponseStop(atHandle);

    return uAtClientUnlock(atHandle);
}

/* ----------------------------------------------------------------
 * PUBLIC FUNCTIONS
 * -------------------------------------------------------------- */

// Refresh the RF status values;
int32_t uCellInfoRefreshRadioParameters(int32_t cellHandle)
{
    int32_t errorCode = (int32_t) U_ERROR_COMMON_NOT_INITIALISED;
    uCellPrivateInstance_t *pInstance;
    uCellPrivateRadioParameters_t *pRadioParameters;
    uAtClientHandle_t atHandle;
    uCellNetRat_t rat;

    if (gUCellPrivateMutex != NULL) {

        U_PORT_MUTEX_LOCK(gUCellPrivateMutex);

        pInstance = pUCellPrivateGetInstance(cellHandle);
        errorCode = (int32_t) U_ERROR_COMMON_INVALID_PARAMETER;
        if (pInstance != NULL) {
            errorCode = (int32_t) U_CELL_ERROR_NOT_REGISTERED;
            atHandle = pInstance->atHandle;
            pRadioParameters = &(pInstance->radioParameters);
            uCellPrivateClearRadioParameters(pRadioParameters);
            if (uCellPrivateIsRegistered(pInstance)) {
                // The mechanisms to get the radio information
                // are different between EUTRAN and GERAN but
                // AT+CSQ works in all cases though it sometimes
                // doesn't return a reading.  Collect what we can
                // with it
                errorCode = getRadioParamsCsq(atHandle, pRadioParameters);
                // Note that AT+UCGED is used next rather than AT+CESQ
                // as, in my experience, it is more reliable in
                // reporting answers.
                if (pInstance->pModule->moduleType == U_CELL_MODULE_TYPE_SARA_R5) {
                    // Allow a little sleepy-byes here, don't want to overtask
                    // the module if this is being called repeatedly
                    // to get an answr to AT+CSQ.
                    uPortTaskBlock(500);
                    // SARA-R5 supports UCGED=2
                    errorCode = getRadioParamsUcged2(atHandle, pRadioParameters);
                } else if (U_CELL_PRIVATE_MODULE_IS_SARA_R4(pInstance->pModule->moduleType)) {
                    // Allow a little sleepy-byes here, don't want to overtask
                    // the module if this is being called repeatedly
                    // to get an answr to AT+CSQ.
                    uPortTaskBlock(500);
                    // SARA-R4 only supports UCGED=5, and it only
                    // supports it in EUTRAN mode
                    rat = uCellPrivateGetActiveRat(pInstance);
                    if (U_CELL_PRIVATE_RAT_IS_EUTRAN(rat)) {
                        errorCode = getRadioParamsUcged5(atHandle, pRadioParameters);
                    } else {
                        // Can't use AT+UCGED, that's all we can get
                        errorCode = (int32_t) U_ERROR_COMMON_SUCCESS;
                    }
                }
            }

            if (errorCode == 0) {
                uPortLog("U_CELL_INFO: radio parameters refreshed:\n");
                uPortLog("             RSSI:    %d dBm\n", pRadioParameters->rssiDbm);
                uPortLog("             RSRP:    %d dBm\n", pRadioParameters->rsrpDbm);
                uPortLog("             RSRQ:    %d dB\n", pRadioParameters->rsrqDb);
                uPortLog("             RxQual:  %d\n", pRadioParameters->rxQual);
                uPortLog("             cell ID: %d\n", pRadioParameters->cellId);
                uPortLog("             EARFCN:  %d\n", pRadioParameters->earfcn);
            } else {
                uPortLog("U_CELL_INFO: unable to refresh radio parameters.\n");
            }
        }

        U_PORT_MUTEX_UNLOCK(gUCellPrivateMutex);
    }

    return errorCode;
}

// Get the RSSI.
int32_t uCellInfoGetRssiDbm(int32_t cellHandle)
{
    // Zero is the error code here as negative values are valid
    int32_t errorCodeOrValue = 0;
    uCellPrivateInstance_t *pInstance;

    if (gUCellPrivateMutex != NULL) {

        U_PORT_MUTEX_LOCK(gUCellPrivateMutex);

        pInstance = pUCellPrivateGetInstance(cellHandle);
        if (pInstance != NULL) {
            errorCodeOrValue = pInstance->radioParameters.rssiDbm;
        }

        U_PORT_MUTEX_UNLOCK(gUCellPrivateMutex);
    }

    return errorCodeOrValue;
}

// Get the RSRP.
int32_t uCellInfoGetRsrpDbm(int32_t cellHandle)
{
    // Zero is the error code here as negative values are valid
    int32_t errorCodeOrValue = 0;
    uCellPrivateInstance_t *pInstance;

    if (gUCellPrivateMutex != NULL) {

        U_PORT_MUTEX_LOCK(gUCellPrivateMutex);

        pInstance = pUCellPrivateGetInstance(cellHandle);
        if (pInstance != NULL) {
            errorCodeOrValue = pInstance->radioParameters.rsrpDbm;
        }

        U_PORT_MUTEX_UNLOCK(gUCellPrivateMutex);
    }

    return errorCodeOrValue;
}

// Get the RSRQ.
int32_t uCellInfoGetRsrqDb(int32_t cellHandle)
{
    // Zero is the error code here as negative values are valid
    int32_t errorCodeOrValue = 0;
    uCellPrivateInstance_t *pInstance;

    if (gUCellPrivateMutex != NULL) {

        U_PORT_MUTEX_LOCK(gUCellPrivateMutex);

        pInstance = pUCellPrivateGetInstance(cellHandle);
        if (pInstance != NULL) {
            errorCodeOrValue = pInstance->radioParameters.rsrqDb;
        }

        U_PORT_MUTEX_UNLOCK(gUCellPrivateMutex);
    }

    return errorCodeOrValue;
}

// Get the RxQual.
int32_t uCellInfoGetRxQual(int32_t cellHandle)
{
    int32_t errorCodeOrValue = (int32_t) U_ERROR_COMMON_NOT_INITIALISED;
    uCellPrivateInstance_t *pInstance;

    if (gUCellPrivateMutex != NULL) {

        U_PORT_MUTEX_LOCK(gUCellPrivateMutex);

        pInstance = pUCellPrivateGetInstance(cellHandle);
        errorCodeOrValue = (int32_t) U_ERROR_COMMON_INVALID_PARAMETER;
        if (pInstance != NULL) {
            errorCodeOrValue = pInstance->radioParameters.rxQual;
        }

        U_PORT_MUTEX_UNLOCK(gUCellPrivateMutex);
    }

    return errorCodeOrValue;
}

// Get the SNR.
int32_t uCellInfoGetSnrDb(int32_t cellHandle, int32_t *pSnrDb)
{
    int32_t errorCode = (int32_t) U_ERROR_COMMON_NOT_INITIALISED;
    uCellPrivateInstance_t *pInstance;
    uCellPrivateRadioParameters_t *pRadioParameters;
    double rssi;
    double rsrp;
    double snrDb;

    if (gUCellPrivateMutex != NULL) {

        U_PORT_MUTEX_LOCK(gUCellPrivateMutex);

        pInstance = pUCellPrivateGetInstance(cellHandle);
        errorCode = (int32_t) U_ERROR_COMMON_INVALID_PARAMETER;
        if ((pInstance != NULL) && (pSnrDb != NULL)) {
            pRadioParameters = &(pInstance->radioParameters);
            errorCode = (int32_t) U_CELL_ERROR_VALUE_OUT_OF_RANGE;
            // SNR = RSRP / (RSSI - RSRP).
            if ((pRadioParameters->rssiDbm != 0) &&
                (pRadioParameters->rssiDbm == pRadioParameters->rsrpDbm)) {
                *pSnrDb = INT_MAX;
                errorCode = (int32_t) U_ERROR_COMMON_SUCCESS;
            } else if ((pRadioParameters->rssiDbm != 0) &&
                       (pRadioParameters->rsrpDbm != 0)) {
                // First convert from dBm
                errno = 0;
                rssi = pow(10.0, ((double) pRadioParameters->rssiDbm) / 10);
                rsrp = pow(10.0, ((double) pRadioParameters->rsrpDbm) / 10);
                if (errno == 0) {
                    snrDb = 10 * log10(rsrp / (rssi - rsrp));
                    if (errno == 0) {
                        *pSnrDb = (int32_t) snrDb;
                        errorCode = (int32_t) U_ERROR_COMMON_SUCCESS;
                    }
                }
            }
        }

        U_PORT_MUTEX_UNLOCK(gUCellPrivateMutex);
    }

    return errorCode;
}

// Get the cell ID.
int32_t uCellInfoGetCellId(int32_t cellHandle)
{
    int32_t errorCodeOrValue = (int32_t) U_ERROR_COMMON_NOT_INITIALISED;
    uCellPrivateInstance_t *pInstance;

    if (gUCellPrivateMutex != NULL) {

        U_PORT_MUTEX_LOCK(gUCellPrivateMutex);

        pInstance = pUCellPrivateGetInstance(cellHandle);
        errorCodeOrValue = (int32_t) U_ERROR_COMMON_INVALID_PARAMETER;
        if (pInstance != NULL) {
            errorCodeOrValue = pInstance->radioParameters.cellId;
        }

        U_PORT_MUTEX_UNLOCK(gUCellPrivateMutex);
    }

    return errorCodeOrValue;
}

// Get the EARFCN.
int32_t uCellInfoGetEarfcn(int32_t cellHandle)
{
    int32_t errorCodeOrValue = (int32_t) U_ERROR_COMMON_NOT_INITIALISED;
    uCellPrivateInstance_t *pInstance;

    if (gUCellPrivateMutex != NULL) {

        U_PORT_MUTEX_LOCK(gUCellPrivateMutex);

        pInstance = pUCellPrivateGetInstance(cellHandle);
        errorCodeOrValue = (int32_t) U_ERROR_COMMON_INVALID_PARAMETER;
        if (pInstance != NULL) {
            errorCodeOrValue = pInstance->radioParameters.earfcn;
        }

        U_PORT_MUTEX_UNLOCK(gUCellPrivateMutex);
    }

    return errorCodeOrValue;
}

// Get the IMEI of the cellular module.
int32_t uCellInfoGetImei(int32_t cellHandle,
                         char *pImei)
{
    int32_t errorCode = (int32_t) U_ERROR_COMMON_NOT_INITIALISED;
    uCellPrivateInstance_t *pInstance;
    uAtClientHandle_t atHandle;
    int32_t bytesRead;

    if (gUCellPrivateMutex != NULL) {

        U_PORT_MUTEX_LOCK(gUCellPrivateMutex);

        pInstance = pUCellPrivateGetInstance(cellHandle);
        errorCode = (int32_t) U_ERROR_COMMON_INVALID_PARAMETER;
        if ((pInstance != NULL) && (pImei != NULL)) {
            atHandle = pInstance->atHandle;
            // Try this ten times: unfortunately
            // the module can spit out a URC just when
            // we're expecting the IMEI and, since there
            // is no prefix on the response, we have
            // no way of telling the difference.  Hence
            // check the length and that length being
            // made up entirely of numerals
            errorCode = (int32_t) U_CELL_ERROR_AT;
            for (size_t x = 10; (x > 0) && (errorCode != 0); x--) {
                uAtClientLock(atHandle);
                uAtClientCommandStart(atHandle, "AT+CGSN");
                uAtClientCommandStop(atHandle);
                uAtClientResponseStart(atHandle, NULL);
                bytesRead = uAtClientReadBytes(atHandle, pImei,
                                               U_CELL_INFO_IMEI_SIZE,
                                               false);
                uAtClientResponseStop(atHandle);
                if ((uAtClientUnlock(atHandle) == 0) &&
                    (bytesRead == U_CELL_INFO_IMEI_SIZE) &&
                    uCellPrivateIsNumeric(pImei, U_CELL_INFO_IMEI_SIZE)) {
                    uPortLog("U_CELL_INFO: IMEI is %*s.\n",
                             U_CELL_INFO_IMEI_SIZE, pImei);
                    errorCode = (int32_t) U_ERROR_COMMON_SUCCESS;
                }
            }
            if (errorCode != 0) {
                uPortLog("U_CELL_INFO: unable to read IMEI.\n");
            }
        }

        U_PORT_MUTEX_UNLOCK(gUCellPrivateMutex);
    }

    return errorCode;
}

// Get the IMSI of the SIM in the cellular module.
int32_t uCellInfoGetImsi(int32_t cellHandle,
                         char *pImsi)
{
    int32_t errorCode = (int32_t) U_ERROR_COMMON_NOT_INITIALISED;
    uCellPrivateInstance_t *pInstance;

    if (gUCellPrivateMutex != NULL) {

        U_PORT_MUTEX_LOCK(gUCellPrivateMutex);

        pInstance = pUCellPrivateGetInstance(cellHandle);
        errorCode = (int32_t) U_ERROR_COMMON_INVALID_PARAMETER;
        if ((pInstance != NULL) && (pImsi != NULL)) {
            errorCode = uCellPrivateGetImsi(pInstance, pImsi);
            if (errorCode == 0) {
                uPortLog("U_CELL_INFO: IMSI is %*s.\n",
                         U_CELL_INFO_IMSI_SIZE, pImsi);
            } else {
                uPortLog("U_CELL_INFO: unable to read IMSI.\n");
            }
        }

        U_PORT_MUTEX_UNLOCK(gUCellPrivateMutex);
    }

    return errorCode;
}

// Get the ICCID string of the SIM in the cellular module.
int32_t uCellInfoGetIccidStr(int32_t cellHandle,
                             char *pStr, size_t size)
{
    int32_t errorCodeOrSize = (int32_t) U_ERROR_COMMON_NOT_INITIALISED;
    uCellPrivateInstance_t *pInstance;
    uAtClientHandle_t atHandle;
    int32_t bytesRead;

    if (gUCellPrivateMutex != NULL) {

        U_PORT_MUTEX_LOCK(gUCellPrivateMutex);

        pInstance = pUCellPrivateGetInstance(cellHandle);
        errorCodeOrSize = (int32_t) U_ERROR_COMMON_INVALID_PARAMETER;
        if ((pInstance != NULL) && (pStr != NULL) && (size > 0)) {
            atHandle = pInstance->atHandle;
            uAtClientLock(atHandle);
            uAtClientCommandStart(atHandle, "AT+CCID");
            uAtClientCommandStop(atHandle);
            uAtClientResponseStart(atHandle, "+CCID:");
            bytesRead = uAtClientReadString(atHandle, pStr, size, false);
            uAtClientResponseStop(atHandle);
            errorCodeOrSize = uAtClientUnlock(atHandle);
            if ((bytesRead >= 0) && (errorCodeOrSize == 0)) {
                errorCodeOrSize = bytesRead;
                uPortLog("U_CELL_INFO: ICCID is %s.\n", pStr);
            } else {
                errorCodeOrSize = (int32_t) U_CELL_ERROR_AT;
                uPortLog("U_CELL_INFO: unable to read ICCID.\n");
            }
        }

        U_PORT_MUTEX_UNLOCK(gUCellPrivateMutex);
    }

    return errorCodeOrSize;
}

// Get the manufacturer ID string from the cellular module.
int32_t uCellInfoGetManufacturerStr(int32_t cellHandle,
                                    char *pStr, size_t size)
{
    int32_t errorCodeOrSize = (int32_t) U_ERROR_COMMON_NOT_INITIALISED;
    uCellPrivateInstance_t *pInstance;

    if (gUCellPrivateMutex != NULL) {

        U_PORT_MUTEX_LOCK(gUCellPrivateMutex);

        pInstance = pUCellPrivateGetInstance(cellHandle);
        errorCodeOrSize = (int32_t) U_ERROR_COMMON_INVALID_PARAMETER;
        if ((pInstance != NULL) && (pStr != NULL) && (size > 0)) {
            errorCodeOrSize = getString(pInstance->atHandle, "AT+CGMI",
                                        pStr, size);
        }

        U_PORT_MUTEX_UNLOCK(gUCellPrivateMutex);
    }

    return errorCodeOrSize;
}

// Get the model identification string from the cellular module.
int32_t uCellInfoGetModelStr(int32_t cellHandle,
                             char *pStr, size_t size)
{
    int32_t errorCodeOrSize = (int32_t) U_ERROR_COMMON_NOT_INITIALISED;
    uCellPrivateInstance_t *pInstance;

    if (gUCellPrivateMutex != NULL) {

        U_PORT_MUTEX_LOCK(gUCellPrivateMutex);

        pInstance = pUCellPrivateGetInstance(cellHandle);
        errorCodeOrSize = (int32_t) U_ERROR_COMMON_INVALID_PARAMETER;
        if ((pInstance != NULL) && (pStr != NULL) && (size > 0)) {
            errorCodeOrSize = getString(pInstance->atHandle, "AT+CGMM",
                                        pStr, size);
        }

        U_PORT_MUTEX_UNLOCK(gUCellPrivateMutex);
    }

    return errorCodeOrSize;
}

// Get the firmware version string from the cellular module.
int32_t uCellInfoGetFirmwareVersionStr(int32_t cellHandle,
                                       char *pStr, size_t size)
{
    int32_t errorCodeOrSize = (int32_t) U_ERROR_COMMON_NOT_INITIALISED;
    uCellPrivateInstance_t *pInstance;

    if (gUCellPrivateMutex != NULL) {

        U_PORT_MUTEX_LOCK(gUCellPrivateMutex);

        pInstance = pUCellPrivateGetInstance(cellHandle);
        errorCodeOrSize = (int32_t) U_ERROR_COMMON_INVALID_PARAMETER;
        if ((pInstance != NULL) && (pStr != NULL) && (size > 0)) {
            errorCodeOrSize = getString(pInstance->atHandle, "AT+CGMR",
                                        pStr, size);
        }

        U_PORT_MUTEX_UNLOCK(gUCellPrivateMutex);
    }

    return errorCodeOrSize;
}

// Get the UTC time according to cellular.
int32_t uCellInfoGetTimeUtc(int32_t cellHandle)
{
    int32_t errorCodeOrValue = (int32_t) U_ERROR_COMMON_NOT_INITIALISED;
    uCellPrivateInstance_t *pInstance;
    uAtClientHandle_t atHandle;
    int32_t timeUtc;
    char buffer[32];
    struct tm timeInfo;
    int32_t bytesRead;
    size_t offset = 0;

    if (gUCellPrivateMutex != NULL) {

        U_PORT_MUTEX_LOCK(gUCellPrivateMutex);

        pInstance = pUCellPrivateGetInstance(cellHandle);
        errorCodeOrValue = (int32_t) U_ERROR_COMMON_INVALID_PARAMETER;
        if (pInstance != NULL) {
            atHandle = pInstance->atHandle;
            uAtClientLock(atHandle);
            uAtClientCommandStart(atHandle, "AT+CCLK?");
            uAtClientCommandStop(atHandle);
            uAtClientResponseStart(atHandle, "+CCLK:");
            bytesRead = uAtClientReadString(atHandle, buffer,
                                            sizeof(buffer), false);
            uAtClientResponseStop(atHandle);
            errorCodeOrValue = uAtClientUnlock(atHandle);
            if ((bytesRead >= 17) && (errorCodeOrValue == 0)) {
                errorCodeOrValue = (int32_t) U_ERROR_COMMON_UNKNOWN;
                uPortLog("U_CELL_INFO: time is %s.\n", buffer);
                // The format of the returned string is
                // "yy/MM/dd,hh:mm:ss+TZ" but the +TZ may be omitted

                // Two-digit year converted to years since 1900
                offset = 0;
                buffer[offset + 2] = 0;
                timeInfo.tm_year = atoi(&(buffer[offset])) + 2000 - 1900;
                // Months converted to months since January
                offset = 3;
                buffer[offset + 2] = 0;
                timeInfo.tm_mon = atoi(&(buffer[offset])) - 1;
                // Day of month
                offset = 6;
                buffer[offset + 2] = 0;
                timeInfo.tm_mday = atoi(&(buffer[offset]));
                // Hours since midnight
                offset = 9;
                buffer[offset + 2] = 0;
                timeInfo.tm_hour = atoi(&(buffer[offset]));
                // Minutes after the hour
                offset = 12;
                buffer[offset + 2] = 0;
                timeInfo.tm_min = atoi(&(buffer[offset]));
                // Seconds after the hour
                offset = 15;
                buffer[offset + 2] = 0;
                timeInfo.tm_sec = atoi(&(buffer[offset]));
                // Get the time in seconds from this
                timeUtc = (int32_t) mktime(&timeInfo);
                if ((timeUtc >= 0) && (bytesRead >= 20)) {
                    // There's a timezone, expressed in 15 minute intervals,
                    // subtract it to get UTC
                    offset = 18;
                    buffer[offset + 2] = 0;
                    timeUtc -= atoi(&(buffer[offset])) * 15 * 60;
                }

                if (timeUtc >= 0) {
                    errorCodeOrValue = (int32_t) U_ERROR_COMMON_SUCCESS;
                    uPortLog("U_CELL_INFO: UTC time is %d.\n", timeUtc);
                } else {
                    uPortLog("U_CELL_INFO: unable to calculate UTC time.\n");
                }
            } else {
                errorCodeOrValue = (int32_t) U_CELL_ERROR_AT;
                uPortLog("U_CELL_INFO: unable to read time with AT+CCLK.\n");
            }
        }

        U_PORT_MUTEX_UNLOCK(gUCellPrivateMutex);
    }

    return errorCodeOrValue;
}

// End of file