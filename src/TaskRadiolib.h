#ifndef TASK_LORA_H_
#define TASK_LORA_H_

#include "project_configuration.h"
#include <APRS-Decoder.h>
#include <BoardFinder.h>
#include <RadioLib.h>
#include <TaskManager.h>

class RadiolibTask : public Task {
public:
  explicit RadiolibTask(TaskQueue<std::shared_ptr<APRSMessage>> &fromModem, TaskQueue<std::shared_ptr<APRSMessage>> &_toModem);
  virtual ~RadiolibTask();

  virtual bool setup(System &system) override;
  virtual bool loop(System &system) override;

private:
  Module *module;
  SX1278 *radio;

  Configuration::LoRa configs[2];

  bool rxEnable, txEnable;

  TaskQueue<std::shared_ptr<APRSMessage>> &_fromModem;
  TaskQueue<std::shared_ptr<APRSMessage>> &_toModem;

  static volatile bool enableInterrupt; // Need to catch interrupt or not.
  static volatile bool operationDone;   // Caught IRQ or not.
  static volatile bool dio1Triggered;   // Caught IRQ on DIO1 or not.

  static void setFlag(void);
  static void setDio1Flag(void);

  int16_t startRX(uint8_t mode);
  int16_t startTX(String &str);

//  uint32_t preambleDurationMilliSec;
  Timer    txWaitTimer;
};

#endif
