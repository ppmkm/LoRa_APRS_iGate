#include <Task.h>
#include <TimeLib.h>
#include <logger.h>

#include "TaskRadiolib.h"

RadiolibTask::RadiolibTask(TaskQueue<std::shared_ptr<APRSMessage>> &fromModem, TaskQueue<std::shared_ptr<APRSMessage>> &toModem) : Task(TASK_RADIOLIB, TaskRadiolib), _fromModem(fromModem), _toModem(toModem) {
}

RadiolibTask::~RadiolibTask() {
  radio->clearDio0Action();
}

volatile bool RadiolibTask::enableInterrupt = true;  // Need to catch interrupt or not.
volatile bool RadiolibTask::operationDone   = false; // Caught IRQ or not.
volatile bool RadiolibTask::dio1Triggered   = false; // Caught IRQ on DIO1 or not.

void RadiolibTask::setFlag(void) {
  if (!enableInterrupt) {
    return;
  }

  operationDone = true;
}

// this function is called when LoRa preamble
  // is detected within timeout period
  // IMPORTANT: this function MUST be 'void' type
  //            and MUST NOT have any arguments!
  #if defined(ESP8266) || defined(ESP32)
    ICACHE_RAM_ATTR
  #endif
void RadiolibTask::setDio1Flag(void) {
    	  if (!enableInterrupt) {
    	    return;
    	  }
  dio1Triggered = true;
}


bool RadiolibTask::setup(System &system) {
  SPI.begin(system.getBoardConfig()->LoraSck, system.getBoardConfig()->LoraMiso, system.getBoardConfig()->LoraMosi, system.getBoardConfig()->LoraCS);
  module = new Module(system.getBoardConfig()->LoraCS, system.getBoardConfig()->LoraIRQ, system.getBoardConfig()->LoraReset, system.getBoardConfig()->LoraGPIO);
  radio  = new SX1278(module);

  configs[0] = system.getUserConfig()->lora;
  configs[1] = system.getUserConfig()->lora2;
  Configuration::LoRa config = configs[0];

  rxEnable = true;
  txEnable = config.tx_enable;

  float freqMHz = (float)config.frequencyRx / 1000000;
  float BWkHz   = (float)config.signalBandwidth / 1000;



  int16_t state = radio->begin(freqMHz, BWkHz, config.spreadingFactor, config.codingRate4, RADIOLIB_SX127X_SYNC_WORD, config.power, config.preambleLength, config.gainRx);
  if (state != RADIOLIB_ERR_NONE) {
    switch (state) {
    case RADIOLIB_ERR_INVALID_FREQUENCY:
      system.getLogger().log(logging::LoggerLevel::LOGGER_LEVEL_ERROR, getName(), "[%s] SX1278 init failed, The supplied frequency value (%fMHz) is invalid for this module.", timeString().c_str(), freqMHz);
      rxEnable = false;
      txEnable = false;
      break;
    case RADIOLIB_ERR_INVALID_BANDWIDTH:
      system.getLogger().log(logging::LoggerLevel::LOGGER_LEVEL_ERROR, getName(), "[%s] SX1278 init failed, The supplied bandwidth value (%fkHz) is invalid for this module. Should be 7800, 10400, 15600, 20800, 31250, 41700 ,62500, 125000, 250000, 500000.", timeString().c_str(), BWkHz);
      rxEnable = false;
      txEnable = false;
      break;
    case RADIOLIB_ERR_INVALID_SPREADING_FACTOR:
      system.getLogger().log(logging::LoggerLevel::LOGGER_LEVEL_ERROR, getName(), "[%s] SX1278 init failed, The supplied spreading factor value (%d) is invalid for this module.", timeString().c_str(), config.spreadingFactor);
      rxEnable = false;
      txEnable = false;
      break;
    case RADIOLIB_ERR_INVALID_CODING_RATE:
      system.getLogger().log(logging::LoggerLevel::LOGGER_LEVEL_ERROR, getName(), "[%s] SX1278 init failed, The supplied coding rate value (%d) is invalid for this module.", timeString().c_str(), config.codingRate4);
      rxEnable = false;
      txEnable = false;
      break;
    case RADIOLIB_ERR_INVALID_OUTPUT_POWER:
      system.getLogger().log(logging::LoggerLevel::LOGGER_LEVEL_ERROR, getName(), "[%s] SX1278 init failed, The supplied output power value (%d) is invalid for this module.", timeString().c_str(), config.power);
      txEnable = false;
      break;
    case RADIOLIB_ERR_INVALID_PREAMBLE_LENGTH:
      system.getLogger().log(logging::LoggerLevel::LOGGER_LEVEL_ERROR, getName(), "[%s] SX1278 init failed, The supplied preamble length is invalid.", timeString().c_str());
      txEnable = false;
      break;
    case RADIOLIB_ERR_INVALID_GAIN:
      system.getLogger().log(logging::LoggerLevel::LOGGER_LEVEL_ERROR, getName(), "[%s] SX1278 init failed, The supplied gain value (%d) is invalid.", timeString().c_str(), config.gainRx);
      rxEnable = false;
      break;
    default:
      system.getLogger().log(logging::LoggerLevel::LOGGER_LEVEL_ERROR, getName(), "[%s] SX1278 init failed, code %d", timeString().c_str(), state);
      rxEnable = false;
      txEnable = false;
    }
    _stateInfo = "LoRa-Modem failed";
    _state     = Error;
  }

  state = radio->setCRC(true);
  if (state != RADIOLIB_ERR_NONE) {
    system.getLogger().log(logging::LoggerLevel::LOGGER_LEVEL_ERROR, getName(), "[%s] setCRC failed, code %d", timeString().c_str(), state);
    _stateInfo = "LoRa-Modem failed";
    _state     = Error;
  }

  radio->setDio0Action(setFlag);
  radio->setDio1Action(setDio1Flag);
  system.getLogger().log(logging::LoggerLevel::LOGGER_LEVEL_INFO, getName(), "[%s] rxEnabled? %d", timeString().c_str(), rxEnable);
  if (rxEnable) {
	int state = radio->startChannelScan();
    if (state != RADIOLIB_ERR_NONE) {
      system.getLogger().log(logging::LoggerLevel::LOGGER_LEVEL_ERROR, getName(), "[%s] startChannelScan failed, code %d", timeString().c_str(), state);
      rxEnable   = false;
      _stateInfo = "LoRa-Modem failed";
      _state     = Error;
    }
    system.getLogger().log(logging::LoggerLevel::LOGGER_LEVEL_INFO, getName(), "[%s] initial startChannelScan", timeString().c_str());
  }


  _stateInfo = "";
  return true;
}

int  transmissionState = RADIOLIB_ERR_NONE;
bool transmitFlag      = false; // Transmitting or not.
static volatile bool receiving = false;  //receiving data
static int configIdx = 0;

bool RadiolibTask::loop(System &system) {

  if (operationDone || dio1Triggered) { // occurs interrupt.
    enableInterrupt = false;

    if (transmitFlag) { // transmitted.
      if (transmissionState == RADIOLIB_ERR_NONE) {
        system.getLogger().log(logging::LoggerLevel::LOGGER_LEVEL_DEBUG, getName(), "[%s] TX done", timeString().c_str());

      } else {
        system.getLogger().log(logging::LoggerLevel::LOGGER_LEVEL_ERROR, getName(), "[%s] transmitFlag failed, code %d", timeString().c_str(), transmissionState);
      }
      operationDone = false;
      dio1Triggered = false;
      transmitFlag  = false;

      txWaitTimer.setTimeout(configs[configIdx].preambleDurationMilliSec * 2);
      txWaitTimer.start();

    } else { //not transmit flag -> detect/receive mode. if (transmitFlag)
        if (receiving) { //actually was receiving
		  operationDone = false;
		  dio1Triggered = false;
		  receiving = false;
		  String str = String("");
		  system.getLogger().log(logging::LoggerLevel::LOGGER_LEVEL_DEBUG, getName(), "Receiving...");
		  int    state = radio->readData(str);
		  system.getLogger().log(logging::LoggerLevel::LOGGER_LEVEL_DEBUG, getName(), "Received...");
		  if (state != RADIOLIB_ERR_NONE) {
			system.getLogger().log(logging::LoggerLevel::LOGGER_LEVEL_ERROR, getName(), "[%s] readData failed, code %d", timeString().c_str(), state);
		  } else {
			if (str.substring(0, 3) != "<\xff\x01") {
			  system.getLogger().log(logging::LoggerLevel::LOGGER_LEVEL_DEBUG, getName(), "[%s] Unknown packet '%s' with RSSI %.0fdBm, SNR %.2fdB and FreqErr %fHz%s", timeString().c_str(), str.c_str(), radio->getRSSI(), radio->getSNR(), -radio->getFrequencyError());
			} else {
			  std::shared_ptr<APRSMessage> msg = std::shared_ptr<APRSMessage>(new APRSMessage());
			  msg->decode(str.substring(3));
			  _fromModem.addElement(msg);
			  system.getLogger().log(logging::LoggerLevel::LOGGER_LEVEL_DEBUG, getName(), "[%s] Received packet '%s' with RSSI %.0fdBm, SNR %.2fdB and FreqErr %fHz", timeString().c_str(), msg->toString().c_str(), radio->getRSSI(), radio->getSNR(), -radio->getFrequencyError());
			  system.getDisplay().addFrame(std::shared_ptr<DisplayFrame>(new TextFrame("LoRa", msg->toString().c_str())));
			}
		  }
      }
      // check if we got a preamble
      if(dio1Triggered) {
		  operationDone = false;
		  dio1Triggered = false;
         // LoRa preamble was detected
         system.getLogger().log(logging::LoggerLevel::LOGGER_LEVEL_DEBUG, getName(),"preamble detected...");
         int state = radio->startReceive(0, RADIOLIB_SX127X_RXSINGLE);
         if (state != RADIOLIB_ERR_NONE) {
           system.getLogger().log(logging::LoggerLevel::LOGGER_LEVEL_ERROR, getName(),"[%s] startReceive single failed, code %d",timeString().c_str(), state);
         } else {
           // set the flag for ongoing reception
           receiving = true;
         }
      }

    }

	if (rxEnable && !receiving) {
	  int state = radio->startChannelScan();
	  if (state != RADIOLIB_ERR_NONE) {
		  system.getLogger().log(logging::LoggerLevel::LOGGER_LEVEL_ERROR, getName(),"[%s] startChannelScan failed, code %d",timeString().c_str(), state);
	  }
	}



    dio1Triggered = false;
    operationDone = false;
    enableInterrupt = true;
  } else { // not interrupt.
    if (!txWaitTimer.check()) {
    } else {
      if (!txEnable) {
        // system.getLogger().log(logging::LoggerLevel::LOGGER_LEVEL_DEBUG, getName(), "[%s] TX is not enabled", timeString().c_str());
      } else {
        if (transmitFlag) {
          // system.getLogger().log(logging::LoggerLevel::LOGGER_LEVEL_DEBUG, getName(), "[%s] TX signal detected. Waiting TX", timeString().c_str());
        } else {
          if (!_toModem.empty()) {
            if (configs[configIdx].frequencyRx == configs[configIdx].frequencyTx && (radio->getModemStatus() & 0x01) == 0x01) {
              // system.getLogger().log(logging::LoggerLevel::LOGGER_LEVEL_DEBUG, getName(), "[%s] RX signal detected. Waiting TX", timeString().c_str());
            } else {
              std::shared_ptr<APRSMessage> msg = _toModem.getElement();
              system.getLogger().log(logging::LoggerLevel::LOGGER_LEVEL_DEBUG, getName(), "[%s] Transmitting packet '%s'", timeString().c_str(), msg->toString().c_str());

              int16_t state = startTX("<\xff\x01" + msg->encode());
              if (state != RADIOLIB_ERR_NONE) {
                system.getLogger().log(logging::LoggerLevel::LOGGER_LEVEL_ERROR, getName(), "[%s] startTX failed, code %d", timeString().c_str(), state);
                txEnable = false;
                return true;
              }
            }
          }
        }
      }
    }
  }

  return true;
}

int16_t RadiolibTask::startRX(uint8_t mode) {
  if (configs[configIdx].frequencyTx != configs[configIdx].frequencyRx) {
    int16_t state = radio->setFrequency((float)configs[configIdx].frequencyRx / 1000000);
    if (state != RADIOLIB_ERR_NONE) {
      return state;
    }
  }
  receiving = true;
  return radio->startReceive(0, mode);
}



int16_t RadiolibTask::startTX(String &str) {
  if (configs[configIdx].frequencyTx != configs[configIdx].frequencyRx) {
    int16_t state = radio->setFrequency((float)configs[configIdx].frequencyTx / 1000000);
    if (state != RADIOLIB_ERR_NONE) {
      return state;
    }
  }

  transmissionState = radio->startTransmit(str);
  transmitFlag      = true;
  return RADIOLIB_ERR_NONE;
}
