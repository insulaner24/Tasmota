/*
  xdrv_62_improv.ino - IMPROV support for Tasmota

  SPDX-FileCopyrightText: 2022 Theo Arends

  SPDX-License-Identifier: GPL-3.0-only
*/

#ifdef USE_IMPROV
/*********************************************************************************************\
 * Serial implementation of IMPROV for initial wifi configuration using esp-web-tools
 *
 * See https://esphome.github.io/esp-web-tools/ and https://www.improv-wifi.com/serial/
\*********************************************************************************************/

#define XDRV_62                  62

#define IMPROV_WIFI_TIMEOUT      30             // Max seconds wait for wifi connection after reconfig

//#define IMPROV_DEBUG

enum ImprovError {
  IMPROV_ERROR_NONE = 0x00,
  IMPROV_ERROR_INVALID_RPC = 0x01,
  IMPROV_ERROR_UNKNOWN_RPC = 0x02,
  IMPROV_ERROR_UNABLE_TO_CONNECT = 0x03,
  IMPROV_ERROR_NOT_AUTHORIZED = 0x04,
  IMPROV_ERROR_UNKNOWN = 0xFF,
};

enum ImprovState {
  IMPROV_STATE_STOPPED = 0x00,
  IMPROV_STATE_AWAITING_AUTHORIZATION = 0x01,
  IMPROV_STATE_AUTHORIZED = 0x02,
  IMPROV_STATE_PROVISIONING = 0x03,
  IMPROV_STATE_PROVISIONED = 0x04,
};

enum ImprovCommand {
  IMPROV_UNKNOWN = 0x00,
  IMPROV_WIFI_SETTINGS = 0x01,
  IMPROV_GET_CURRENT_STATE = 0x02,
  IMPROV_GET_DEVICE_INFO = 0x03,
  IMPROV_GET_WIFI_NETWORKS = 0x04,
  IMPROV_BAD_CHECKSUM = 0xFF,
};

enum ImprovSerialType {
  IMPROV_TYPE_CURRENT_STATE = 0x01,
  IMPROV_TYPE_ERROR_STATE = 0x02,
  IMPROV_TYPE_RPC = 0x03,
  IMPROV_TYPE_RPC_RESPONSE = 0x04
};

static const uint8_t IMPROV_SERIAL_VERSION = 1;

struct IMPROV {
  uint32_t last_read_byte;
  uint8_t wifi_timeout;
  uint8_t seriallog_level;
  bool message;
} Improv;

/*********************************************************************************************/

void ImprovWriteData(uint8_t* data, uint32_t size) {
  data[0] = 'I';
  data[1] = 'M';
  data[2] = 'P';
  data[3] = 'R';
  data[4] = 'O';
  data[5] = 'V';
  data[6] = IMPROV_SERIAL_VERSION;                             // 0x01
  uint8_t checksum = 0x00;
  for (uint32_t i = 0; i < size -1; i++) {
    checksum += data[i];
  }
  data[size -1] = checksum;

  AddLog(LOG_LEVEL_DEBUG_MORE, PSTR("IMP: Send '%*_H'"), size, data);

//  Serial.write(data, size);
  for (uint32_t i = 0; i < size; i++) {
    Serial.write(data[i]);
  }
  Serial.write('\n');
}

void ImprovSendCmndState(uint32_t command, uint32_t state) {
  uint8_t data[11];
  data[7] = command;
  data[8] = 1;
  data[9] = state;
  ImprovWriteData(data, sizeof(data));
}

void ImprovSendState(uint32_t state) {
#ifdef IMPROV_DEBUG
  AddLog(LOG_LEVEL_DEBUG, PSTR("IMP: State %d"), state);
#endif
  RtcSettings.improv_state = state;
  ImprovSendCmndState(IMPROV_TYPE_CURRENT_STATE, state);       // 0x01
}

void ImprovSendError(uint32_t error) {
#ifdef IMPROV_DEBUG
  AddLog(LOG_LEVEL_DEBUG, PSTR("IMP: Error %d"), error);
#endif
  ImprovSendCmndState(IMPROV_TYPE_ERROR_STATE, error);         // 0x02
}

void ImprovSendResponse(uint8_t* response, uint32_t size) {
  uint8_t data[9 + size];
  data[7] = IMPROV_TYPE_RPC_RESPONSE;                          // 0x04
  data[8] = size -1;
  memcpy(data +9, response, size);
  ImprovWriteData(data, sizeof(data));
}

void ImprovSendSetting(uint32_t command) {
  char data[100];
  uint32_t len = 0;
#ifdef USE_WEBSERVER
  len = ext_snprintf_P(data, sizeof(data), PSTR("01|http://%_I:%d|"), (uint32_t)WiFi.localIP(), WEB_PORT);
  uint32_t str_pos = 2;
  for (uint32_t i = 3; i < len; i++) {
    if ('|' == data[i]) {
      data[str_pos] = i - str_pos -1;
    }
  }
  len -= 3;
#endif  // USE_WEBSERVER
  data[0] = command;
  data[1] = len;
  ImprovSendResponse((uint8_t*)data, len +3);
}

bool ImprovParseSerialByte(void) {
  // 0  1  2  3  4  5  6  7  8  9        8 + le +1
  // I  M  P  R  O  V  ve ty le data ... \n
  // 49 4D 50 52 4F 56 01 xx yy ........ 0A
  if (6 == TasmotaGlobal.serial_in_byte_counter) {
    return (IMPROV_SERIAL_VERSION == TasmotaGlobal.serial_in_byte);
  }
  if (TasmotaGlobal.serial_in_byte_counter <= 8) {
    return true;                                               // Wait for type and length
  }
  uint32_t data_len = TasmotaGlobal.serial_in_buffer[8];
  if (TasmotaGlobal.serial_in_byte_counter <= 9 + data_len) {  // Receive including '\n'
    return true;                                               // Wait for data
  }

  AddLog(LOG_LEVEL_DEBUG_MORE, PSTR("IMP: Rcvd '%*_H'"), TasmotaGlobal.serial_in_byte_counter, TasmotaGlobal.serial_in_buffer);

  TasmotaGlobal.serial_in_byte_counter--;                      // Drop '\n'
  uint8_t checksum = 0x00;
  for (uint32_t i = 0; i < TasmotaGlobal.serial_in_byte_counter; i++) {
    checksum += TasmotaGlobal.serial_in_buffer[i];
  }
  if (checksum != TasmotaGlobal.serial_in_buffer[TasmotaGlobal.serial_in_byte_counter]) {
    ImprovSendError(IMPROV_ERROR_INVALID_RPC);                 // 0x01 - CRC error
    return false;
  }

  uint32_t type = TasmotaGlobal.serial_in_buffer[7];
  if (IMPROV_TYPE_RPC == type) {                               // 0x03
    uint32_t data_length = TasmotaGlobal.serial_in_buffer[10];
    if (data_length != data_len - 2) {
      return false;
    }

    uint32_t command = TasmotaGlobal.serial_in_buffer[9];
    switch (command) {
      case IMPROV_WIFI_SETTINGS: {                             // 0x01
//        if (RtcSettings.improv_state != IMPROV_STATE_AUTHORIZED) {
//          ImprovSendError(IMPROV_ERROR_NOT_AUTHORIZED);        // 0x04
//        } else {
          // 0  1  2  3  4  5  6  7  8  9  10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25
          // I  M  P  R  O  V  vs ty le co dl sl s  s  i  d  pl p  a  s  s  w  o  r  d  cr
          uint32_t ssid_length = TasmotaGlobal.serial_in_buffer[11];
          uint32_t ssid_end = 12 + ssid_length;
          uint32_t pass_length = TasmotaGlobal.serial_in_buffer[ssid_end];
          uint32_t pass_start = ssid_end + 1;
          uint32_t pass_end = pass_start + pass_length;
          TasmotaGlobal.serial_in_buffer[ssid_end] = '\0';
          char* ssid = &TasmotaGlobal.serial_in_buffer[12];
          TasmotaGlobal.serial_in_buffer[pass_end] = '\0';
          char* password = &TasmotaGlobal.serial_in_buffer[pass_start];
#ifdef IMPROV_DEBUG
          AddLog(LOG_LEVEL_DEBUG, PSTR("IMP: Ssid '%s', Password '%s'"), ssid, password);
#endif  // IMPROV_DEBUG
          Improv.wifi_timeout = IMPROV_WIFI_TIMEOUT;           // Set WiFi connect timeout
          ImprovSendState(IMPROV_STATE_PROVISIONING);
          Settings->flag4.network_wifi = 1;                    // Enable WiFi
          char cmnd[TOPSZ];
          snprintf_P(cmnd, sizeof(cmnd), PSTR(D_CMND_BACKLOG "0 " D_CMND_SSID "1 %s;" D_CMND_PASSWORD "1 %s"), ssid, password);
          ExecuteCommand(cmnd, SRC_SERIAL);                    // Set SSID and Password and restart
//        }
        break;
      }
      case IMPROV_GET_CURRENT_STATE: {                         // 0x02
        ImprovSendState(RtcSettings.improv_state);
        if (IMPROV_STATE_PROVISIONED == RtcSettings.improv_state) {
          ImprovSendSetting(IMPROV_GET_CURRENT_STATE);
        }
        break;
      }
      case IMPROV_GET_DEVICE_INFO: {                           // 0x03
        char data[200];
        uint32_t len = snprintf_P(data, sizeof(data), PSTR("01|Tasmota|%s|%s|%s|"),
                                  TasmotaGlobal.version, GetDeviceHardware().c_str(), SettingsText(SET_DEVICENAME));
        data[0] = IMPROV_GET_DEVICE_INFO;
        data[1] = len -3;

        uint32_t str_pos = 2;
        for (uint32_t i = 3; i < len; i++) {
          if ('|' == data[i]) {
            data[str_pos] = i - str_pos -1;
            str_pos = i;
          }
        }
        ImprovSendResponse((uint8_t*)data, len);
        break;
      }
      case IMPROV_GET_WIFI_NETWORKS: {                         // 0x04
        char data[200];
        int n = WiFi.scanNetworks();
        if (n) {
          // Sort networks
          int indices[n];
          for (uint32_t i = 0; i < n; i++) {
            indices[i] = i;
          }
          // RSSI SORT
          for (uint32_t i = 0; i < n; i++) {
            for (uint32_t j = i + 1; j < n; j++) {
              if (WiFi.RSSI(indices[j]) > WiFi.RSSI(indices[i])) {
                std::swap(indices[i], indices[j]);
              }
            }
          }
          // Remove duplicates ( must be RSSI sorted )
          for (uint32_t i = 0; i < n; i++) {
            if (-1 == indices[i]) { continue; }
            String cssid = WiFi.SSID(indices[i]);
            uint32_t cschn = WiFi.channel(indices[i]);
            for (uint32_t j = i + 1; j < n; j++) {
              if ((cssid == WiFi.SSID(indices[j])) && (cschn == WiFi.channel(indices[j]))) {
                indices[j] = -1;  // set dup aps to index -1
              }
            }
          }

          // Send networks
          for (uint32_t i = 0; i < n; i++) {
            if (-1 == indices[i]) { continue; }                // Skip dups
            int32_t rssi = WiFi.RSSI(indices[i]);
            String ssid_copy = WiFi.SSID(indices[i]);
            if (!ssid_copy.length()) { ssid_copy = F("no_name"); }

            // Send each ssid separately to avoid overflowing the buffer
            uint32_t len = snprintf_P(data, sizeof(data), PSTR("01|%s|%d|%s|"), ssid_copy.c_str(), rssi, (ENC_TYPE_NONE == WiFi.encryptionType(indices[i]))?"NO":"YES");
            data[0] = IMPROV_GET_WIFI_NETWORKS;
            data[1] = len -3;

            uint32_t str_pos = 2;
            for (uint32_t i = 3; i < len; i++) {
              if ('|' == data[i]) {
                data[str_pos] = i - str_pos -1;
                str_pos = i;
              }
            }
            ImprovSendResponse((uint8_t*)data, len);
          }
        }

        // Send empty response to signify the end of the list.
        data[0] = IMPROV_GET_WIFI_NETWORKS;
        data[1] = 0;                                           // Empty string
        ImprovSendResponse((uint8_t*)data, 3);
        break;
      }
/*
      case IMPROV_BAD_CHECKSUM: {                              // 0xFF
        break;
      }
*/
      default:
        ImprovSendError(IMPROV_ERROR_UNKNOWN_RPC);             // 0x02 - Unknown payload
    }
  }

  return false;
}

/*********************************************************************************************/

bool ImprovSerialInput(void) {
  // Check if received data is IMPROV data
  if (6 == TasmotaGlobal.serial_in_byte_counter) {
    TasmotaGlobal.serial_in_buffer[TasmotaGlobal.serial_in_byte_counter] = 0;
    if (!strcmp_P(TasmotaGlobal.serial_in_buffer, PSTR("IMPROV"))) {
      Improv.seriallog_level = TasmotaGlobal.seriallog_level;
      TasmotaGlobal.seriallog_level = 0;                       // Disable seriallogging interfering with IMPROV
      Improv.last_read_byte = millis();
      Improv.message = true;
    }
  }
  if (Improv.message) {
    uint32_t now = millis();
    if (now - Improv.last_read_byte < 50) {
      TasmotaGlobal.serial_in_buffer[TasmotaGlobal.serial_in_byte_counter] = TasmotaGlobal.serial_in_byte;
      if (ImprovParseSerialByte()) {
        TasmotaGlobal.serial_in_byte_counter++;
        TasmotaGlobal.serial_in_byte = 0;
        Improv.last_read_byte = now;
        return false;
      }
    }
    Improv.message = false;
    TasmotaGlobal.seriallog_level = Improv.seriallog_level;    // Restore seriallogging
    return true;
  }
  return false;
}

void ImprovEverySecond(void) {
  if (Improv.wifi_timeout) {
    Improv.wifi_timeout--;
    if (Improv.wifi_timeout < IMPROV_WIFI_TIMEOUT -3) {       // Tasmota restarts after ssid or password change
      if ((!TasmotaGlobal.global_state.wifi_down)) {
        Improv.wifi_timeout = 0;
        if (IMPROV_STATE_AUTHORIZED == RtcSettings.improv_state) {
          RtcSettings.improv_state = IMPROV_STATE_PROVISIONED;
        }
        if (IMPROV_STATE_PROVISIONING == RtcSettings.improv_state) {
          ImprovSendState(IMPROV_STATE_PROVISIONED);
          ImprovSendSetting(IMPROV_WIFI_SETTINGS);
        }
        return;
      }
    }
    if (!Improv.wifi_timeout) {
      if (IMPROV_STATE_PROVISIONING == RtcSettings.improv_state) {
        ImprovSendError(IMPROV_ERROR_UNABLE_TO_CONNECT);       // 0x03 - WiFi connect timeout
        ImprovSendState(IMPROV_STATE_AUTHORIZED);
      }
    }
  }
}

void ImprovInit(void) {
  if (!RtcSettings.improv_state) {
    RtcSettings.improv_state = IMPROV_STATE_AUTHORIZED;        // Power on state (persistent during restarts)
  }
  Improv.wifi_timeout = IMPROV_WIFI_TIMEOUT;                   // Try to update state after restart
#ifdef IMPROV_DEBUG
  AddLog(LOG_LEVEL_DEBUG, PSTR("IMP: State %d"), RtcSettings.improv_state);
#endif  // IMPROV_DEBUG
}

/*********************************************************************************************\
 * Interface
\*********************************************************************************************/

bool Xdrv62(uint8_t function) {
  bool result = false;

  switch (function) {
    case FUNC_EVERY_SECOND:
      ImprovEverySecond();
      break;
    case FUNC_SERIAL:
      result = ImprovSerialInput();
      break;
    case FUNC_PRE_INIT:
      ImprovInit();
      break;
  }
  return result;
}

#endif // USE_IMPROV