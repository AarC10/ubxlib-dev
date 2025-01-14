/*
 * Copyright 2019-2024 u-blox
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
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
 * @brief Implementation of the GAP API for BLE.
 */

#ifndef U_CFG_BLE_MODULE_INTERNAL

#ifdef U_CFG_OVERRIDE
#include "u_cfg_override.h"  // For a customer's configuration override
#endif

#include "stdbool.h"
#include "stddef.h"  // NULL, size_t etc.
#include "stdint.h"  // int32_t etc.
#include "stdio.h"   // snprintf()
#include "stdlib.h"  // strol(), atoi(), strol(), strtof()
#include "string.h"  // memset(), strncpy(), strtok_r(), strtol()
#include "u_error_common.h"
#include "u_at_client.h"
#include "u_ble.h"
#include "u_ble_cfg.h"
#include "u_ble_extmod_private.h"
#include "u_ble_gap.h"
#include "u_ble_private.h"
#include "u_cfg_sw.h"
#include "u_port.h"
#include "u_port_os.h"
#include "u_port_heap.h"
#include "u_short_range.h"
#include "u_short_range_module_type.h"
#include "u_short_range_pbuf.h"
#include "u_short_range_private.h"
#include "u_network.h"
#include "u_network_shared.h"
#include "u_network_config_ble.h"
#include "u_ble_gatt.h"
#include "u_ble_context.h"

#include "u_cx_system.h"
#include "u_cx_bluetooth.h"
#include "u_cx_urc.h"
#include "u_cx_log.h"

/* ----------------------------------------------------------------
 * COMPILE-TIME MACROS
 * -------------------------------------------------------------- */

/* ----------------------------------------------------------------
 * TYPES
 * -------------------------------------------------------------- */

/* ----------------------------------------------------------------
 * VARIABLES
 * -------------------------------------------------------------- */

/* ----------------------------------------------------------------
 * STATIC FUNCTIONS
 * -------------------------------------------------------------- */

static void bleConnectCallback(struct uCxHandle *pUcxHandle, int32_t connHandle,
                               uBtLeAddress_t *pBdAddr)
{
    uBleDeviceState_t *pState = pGetBleContext(pUcxHandle->pAtClient->pConfig->pContext);
    if (pState != NULL) {
        pState->connHandle = connHandle;
        if (pState->connectCallback != NULL) {
            char bdAddrString[U_BD_STRING_MAX_LENGTH_BYTES];
            if (uCxBdAddressToString(pBdAddr, bdAddrString, sizeof(bdAddrString) == 0)) {
                pState->connectCallback(connHandle, bdAddrString, true);
            }
        }
    }
}

static void bleDisconnectCallback(struct uCxHandle *pUcxHandle, int32_t connHandle)
{
    uBleDeviceState_t *pState = pGetBleContext(pUcxHandle->pAtClient->pConfig->pContext);
    if (pState != NULL) {
        pState->connHandle = -1;
        if (pState->connectCallback != NULL) {
            pState->connectCallback(connHandle, NULL, false);
        }
    }
}

/* ----------------------------------------------------------------
 * PUBLIC FUNCTIONS
 * -------------------------------------------------------------- */

int32_t uBleGapGetMac(uDeviceHandle_t devHandle, char *pMac)
{
    int32_t errorCode = (int32_t)U_ERROR_COMMON_INVALID_PARAMETER;
    uCxHandle_t *pUcxHandle = pShortRangePrivateGetUcxHandle(devHandle);
    if (pUcxHandle != NULL) {
        uMacAddress_t macAddr;
        errorCode = uCxSystemGetLocalAddress(pUcxHandle,
                                             U_INTERFACE_ID_BLUETOOTH, &macAddr);
        if (errorCode == 0) {
            uCxMacAddressToString(&macAddr, pMac, U_SHORT_RANGE_BT_ADDRESS_SIZE);
        }
    }
    return errorCode;
}

int32_t uBleGapSetConnectCallback(uDeviceHandle_t devHandle, uBleGapConnectCallback_t cb)
{
    int32_t errorCode = uShortRangeLock();
    if (errorCode == 0) {
        errorCode = (int32_t)U_ERROR_COMMON_INVALID_PARAMETER;
        uCxHandle_t *pUcxHandle = pShortRangePrivateGetUcxHandle(devHandle);
        uShortRangePrivateInstance_t *pInstance = pUShortRangePrivateGetInstance(devHandle);
        if ((pUcxHandle != NULL) && (pInstance != NULL)) {
            errorCode = checkCreateBleContext(pInstance);
            if (errorCode == 0) {
                ((uBleDeviceState_t *)(pInstance->pBleContext))->connectCallback = cb;
                uCxUrcRegisterBluetoothConnect(pUcxHandle, bleConnectCallback);
                uCxUrcRegisterBluetoothDisconnect(pUcxHandle, bleDisconnectCallback);
                errorCode = (int32_t)U_ERROR_COMMON_SUCCESS;
            }
        }
        uShortRangeUnlock();
    }
    return errorCode;
}

int32_t uBleGapScan(uDeviceHandle_t devHandle,
                    uBleGapDiscoveryType_t discType,
                    bool activeScan,
                    uint32_t timeousMs,
                    uBleGapScanCallback_t cb)
{
    int32_t errorCode = uShortRangeLock();
    if (errorCode == 0) {
        errorCode = (int32_t)U_ERROR_COMMON_INVALID_PARAMETER;
        uCxHandle_t *pUcxHandle = pShortRangePrivateGetUcxHandle(devHandle);
        if (pUcxHandle != NULL) {
            // Map old discovery types to the new uCx ones
            uDiscoveryType_t uCxType;
            if ((discType == U_BLE_GAP_SCAN_DISCOVER_ALL_ONCE) ||
                (discType == U_BLE_GAP_SCAN_DISCOVER_LIMITED_ONCE)) {
                uCxType = U_DISCOVERY_TYPE_DISCOVER_ALL_NO_DUPLICATES;
            } else {
                uCxType = U_DISCOVERY_TYPE_DISCOVER_ALL;
            }
            // Need to turn off possible AT debug printouts during the scanning
            bool logWasOn = uCxLogIsEnabled();
            uCxLogDisable();
            // Start and loop over all received discoveries
            uCxBeginBluetoothDiscoveryEx3(pUcxHandle, uCxType,
                                          activeScan ? U_DISCOVERY_MODE_ACTIVE : U_DISCOVERY_MODE_PASSIVE,
                                          (int32_t)timeousMs);
            uCxBluetoothDiscoveryEx_t uCxResp;
            while (uCxBluetoothDiscoveryExGetResponse3(pUcxHandle, &uCxResp)) {
                uBleScanResult_t result;
                uCxBdAddressToString(&uCxResp.bd_addr, result.address, sizeof(result.address));
                int32_t length = MIN(sizeof(result.data), uCxResp.data.length);
                memcpy(result.data, uCxResp.data.pData, length);
                result.dataLength = length;
                result.dataType = uCxResp.data_type;
                strcpy(result.name, uCxResp.device_name);
                result.rssi = uCxResp.rssi;
                cb(&result);
            }
            errorCode = uCxEnd(pUcxHandle);
            if (logWasOn) {
                uCxLogEnable();
            }
            if (errorCode == U_CX_ERROR_CMD_TIMEOUT) {
                // *** UCX WORKAROUND FIX ***
                // Ignore timeout for now, not possible to set ucx timeout value
                errorCode = (int32_t)U_ERROR_COMMON_SUCCESS;
            }
        }
        uShortRangeUnlock();
    }
    return errorCode;
}

int32_t uBleGapConnect(uDeviceHandle_t devHandle, const char *pAddress)
{
    int32_t errorCode = uShortRangeLock();
    if (errorCode == 0) {
        errorCode = (int32_t)U_ERROR_COMMON_INVALID_PARAMETER;
        uShortRangePrivateInstance_t *pInstance = pUShortRangePrivateGetInstance(devHandle);
        uCxHandle_t *pUcxHandle = pShortRangePrivateGetUcxHandle(devHandle);
        if ((pUcxHandle != NULL) && (pInstance != NULL)) {
            errorCode = checkCreateBleContext(pInstance);
            if (errorCode == 0) {
                ((uBleDeviceState_t *)(pInstance->pBleContext))->connHandle = -1;
                uBtLeAddress_t bdAddr;
                errorCode = uCxStringToBdAddress(pAddress, &bdAddr);
                if (errorCode == 0) {
                    errorCode = uCxBluetoothConnect(pUcxHandle, &bdAddr);
                }
            }
        }
        uShortRangeUnlock();
    }
    return errorCode;
}

int32_t uBleGapDisconnect(uDeviceHandle_t devHandle, int32_t connHandle)
{
    int32_t errorCode = (int32_t)U_ERROR_COMMON_INVALID_PARAMETER;
    uCxHandle_t *pUcxHandle = pShortRangePrivateGetUcxHandle(devHandle);
    if (pUcxHandle != NULL) {
        errorCode = uCxBluetoothDisconnect(pUcxHandle, connHandle);
    }
    return errorCode;
}

int32_t uBleGapSetAdvData(const char *pName,
                          const uint8_t *pManufData, uint8_t manufDataSize,
                          uint8_t *pAdvData, uint8_t advDataSize)
{
    if (!pName && !pManufData) {
        return (int32_t)U_ERROR_COMMON_INVALID_PARAMETER;
    }
    uint8_t nameSize =
        pName ? (uint8_t)(strlen(pName) + 2) : 0; /* string length, tag and the string */
    manufDataSize = pManufData ? manufDataSize + 2 : 0;
    int32_t totSize = nameSize + manufDataSize;
    if (totSize > advDataSize) {
        return (int32_t)U_ERROR_COMMON_INVALID_PARAMETER;
    }
    if (pName) {
        nameSize--;
        *(pAdvData++) = nameSize--;
        *(pAdvData++) = U_BT_DATA_NAME_COMPLETE;
        memcpy(pAdvData, pName, nameSize);
        pAdvData += nameSize;
    }
    if (pManufData) {
        manufDataSize--;
        *(pAdvData++) = manufDataSize--;
        *(pAdvData++) = U_BT_DATA_MANUFACTURER_DATA;
        memcpy(pAdvData, pManufData, manufDataSize);
    }
    return totSize;
}

int32_t uBleGapAdvertiseStart(uDeviceHandle_t devHandle, const uBleGapAdvConfig_t *pConfig)
{
    int32_t errorCode = (int32_t)U_ERROR_COMMON_INVALID_PARAMETER;
    uCxHandle_t *pUcxHandle = pShortRangePrivateGetUcxHandle(devHandle);
    if (pUcxHandle != NULL) {
        errorCode = uCxBluetoothSetAdvertiseData(pUcxHandle, pConfig->pAdvData,
                                                 pConfig->advDataLength);
        if (errorCode == 0) {
            errorCode = uCxBluetoothSetAdvertisements(pUcxHandle, 1);
        }
    }
    return errorCode;
}

int32_t uBleGapAdvertiseStop(uDeviceHandle_t devHandle)
{
    int32_t errorCode = (int32_t)U_ERROR_COMMON_INVALID_PARAMETER;
    uCxHandle_t *pUcxHandle = pShortRangePrivateGetUcxHandle(devHandle);
    if (pUcxHandle != NULL) {
        errorCode = uCxBluetoothSetAdvertisements(pUcxHandle, 0);
    }
    return errorCode;
}

int32_t uBleGapReset(uDeviceHandle_t devHandle)
{
    uBleGapAdvertiseStop(devHandle);
    return uShortrangePrivateRestartDevice(devHandle, false);
}
#endif